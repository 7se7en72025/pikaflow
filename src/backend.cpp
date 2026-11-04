#include "pikaflow/backend.hpp"
#include "pikaflow/runtime.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <mutex>
#include <stdexcept>

namespace pika {
namespace {
void initializeLLVM() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (llvm::InitializeNativeTarget() || llvm::InitializeNativeTargetAsmPrinter()
            || llvm::InitializeNativeTargetAsmParser())
            throw std::runtime_error("could not initialize LLVM native target");
    });
}

std::unique_ptr<llvm::TargetMachine> makeTarget(unsigned optimization) {
    initializeLLVM();
    llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
    std::string error;
    const auto* target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target) throw std::runtime_error(error);
    llvm::TargetOptions options;
    auto machine = std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
        triple, "generic", "", options, llvm::Reloc::PIC_, std::nullopt,
        optimization == 0 ? llvm::CodeGenOptLevel::None :
        optimization == 3 ? llvm::CodeGenOptLevel::Aggressive : llvm::CodeGenOptLevel::Default));
    if (!machine) throw std::runtime_error("could not create LLVM target machine");
    return machine;
}

std::string nativeName(const std::string& sourceName) {
    std::string name = "pf_fn_";
    for (unsigned char c : sourceName) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') name += static_cast<char>(c);
        else {
            static const char hex[] = "0123456789abcdef";
            name += '_'; name += hex[c >> 4]; name += hex[c & 15];
        }
    }
    return name;
}

// LLVM cannot infer the contracts of our checked runtime calls. These identities
// are valid even with checked overflow, so folding them never removes a failure.
class CheckedIdentityPass : public llvm::OptionalPassInfoMixin<CheckedIdentityPass> {
public:
    llvm::PreservedAnalyses run(llvm::Function& function,llvm::FunctionAnalysisManager&) {
        bool changed=false;
        for(auto& block:function) {
            for(auto it=block.begin();it!=block.end();) {
                auto* instruction=&*it++;
                auto* call=llvm::dyn_cast<llvm::CallInst>(instruction);
                if(!call || call->arg_size()!=2 || !call->getCalledFunction()) continue;
                auto* constant=llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1));
                if(!constant || !constant->getType()->isIntegerTy(64)) continue;
                auto name=call->getCalledFunction()->getName();
                llvm::Value* replacement=nullptr;
                if(constant->isZero() && (name=="pf_checked_add" || name=="pf_checked_sub"))
                    replacement=call->getArgOperand(0);
                else if(constant->isOne() && (name=="pf_checked_mul" || name=="pf_checked_div"))
                    replacement=call->getArgOperand(0);
                else if((constant->isZero() && name=="pf_checked_mul") ||
                    (constant->isOne() && name=="pf_checked_mod"))
                    replacement=llvm::ConstantInt::get(call->getType(),0);
                if(replacement) {
                    call->replaceAllUsesWith(replacement);
                    call->eraseFromParent(); changed=true;
                }
            }
        }
        return changed?llvm::PreservedAnalyses::none():llvm::PreservedAnalyses::all();
    }
};

class Emitter {
    const Program& program;
    llvm::LLVMContext& context;
    llvm::Module& module;
    llvm::IRBuilder<> builder;
    std::map<std::string, llvm::Function*> functions;
    std::unique_ptr<llvm::DIBuilder> debugBuilder;
    llvm::DIFile* debugFile = nullptr;
    llvm::DISubprogram* debugScope = nullptr;
    const Function* current = nullptr;
    llvm::Value* environment = nullptr;
    llvm::Value* rootFrame = nullptr;
    std::vector<llvm::AllocaInst*> registers;
    std::vector<llvm::Value*> roots;
    std::vector<llvm::BasicBlock*> blocks;

    llvm::Type* i64() { return llvm::Type::getInt64Ty(context); }
    llvm::Type* i32() { return llvm::Type::getInt32Ty(context); }
    llvm::Type* ptr() { return llvm::PointerType::getUnqual(context); }
    llvm::Constant* n64(int64_t n) { return llvm::ConstantInt::get(i64(), n, true); }
    llvm::Constant* tag(const Type& t) { return llvm::ConstantInt::get(i32(), uint32_t(t.kind)); }
    llvm::Constant* null() { return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr())); }
    llvm::Type* type(const Type& t) {
        switch (t.kind) {
        case TypeKind::Unit: return llvm::Type::getVoidTy(context);
        case TypeKind::I64: return i64();
        case TypeKind::F64: return llvm::Type::getDoubleTy(context);
        case TypeKind::Bool: return llvm::Type::getInt1Ty(context);
        default: return ptr();
        }
    }
    llvm::Type* slotType(const Type& t) {
        return t.kind == TypeKind::Unit ? llvm::Type::getInt8Ty(context) : type(t);
    }
    llvm::Value* load(int reg) {
        if (reg < 0 || size_t(reg) >= registers.size()) throw std::runtime_error("invalid IR register");
        return builder.CreateLoad(slotType(current->registers[reg]), registers[reg], "r" + std::to_string(reg));
    }
    void store(int reg, llvm::Value* value) {
        if (reg < 0) return;
        if (!value || value->getType()->isVoidTy() || current->registers[reg].kind == TypeKind::Unit) return;
        builder.CreateStore(value, registers.at(size_t(reg)));
        if (roots[reg]) builder.CreateStore(value, roots[reg]);
    }
    llvm::Value* bits(llvm::Value* value, const Type& t) {
        if (t.kind == TypeKind::Unit) return n64(0);
        if (t.kind == TypeKind::I64) return value;
        if (t.kind == TypeKind::F64) return builder.CreateBitCast(value, i64());
        if (t.kind == TypeKind::Bool) return builder.CreateZExt(value, i64());
        return builder.CreatePtrToInt(value, i64());
    }
    llvm::Value* fromBits(llvm::Value* value, const Type& t) {
        if (t.kind == TypeKind::I64) return value;
        if (t.kind == TypeKind::F64) return builder.CreateBitCast(value, type(t));
        if (t.kind == TypeKind::Bool) return builder.CreateTrunc(value, type(t));
        if (t.kind == TypeKind::Unit) return nullptr;
        return builder.CreateIntToPtr(value, ptr());
    }
    llvm::Value* runtime(const char* name, llvm::Type* result,
        llvm::ArrayRef<llvm::Type*> parameters, llvm::ArrayRef<llvm::Value*> args) {
        auto callee = module.getOrInsertFunction(name, llvm::FunctionType::get(result, parameters, false));
        return builder.CreateCall(callee, args);
    }
    llvm::FunctionType* signature(const std::vector<Type>& params, const Type& result, bool hiddenEnv) {
        std::vector<llvm::Type*> types;
        if (hiddenEnv) types.push_back(ptr());
        for (const auto& p : params) types.push_back(type(p));
        return llvm::FunctionType::get(type(result), types, false);
    }
    llvm::DIType* debugType(const Type& t) {
        if (t.kind == TypeKind::Unit) return nullptr;
        if (t.kind == TypeKind::I64) return debugBuilder->createBasicType("i64", 64, llvm::dwarf::DW_ATE_signed);
        if (t.kind == TypeKind::F64) return debugBuilder->createBasicType("f64", 64, llvm::dwarf::DW_ATE_float);
        if (t.kind == TypeKind::Bool) return debugBuilder->createBasicType("bool", 8, llvm::dwarf::DW_ATE_boolean);
        return debugBuilder->createPointerType(nullptr, 64, 0, std::nullopt, t.str());
    }
    void setupDebug(llvm::Function* fn, const Function& source) {
        if (!debugBuilder) return;
        if(!source.location.file.empty()) {
            std::filesystem::path path(source.location.file);
            debugFile=debugBuilder->createFile(path.filename().string(),path.parent_path().string());
        }
        llvm::SmallVector<llvm::Metadata*, 8> parameters;
        parameters.push_back(debugType(source.result));
        for (const auto& t : source.parameters) parameters.push_back(debugType(t));
        auto* functionType = debugBuilder->createSubroutineType(debugBuilder->getOrCreateTypeArray(parameters));
        debugScope = debugBuilder->createFunction(debugFile, source.name, fn->getName(), debugFile,
            source.location.line, functionType, source.location.line,
            llvm::DINode::FlagPrototyped, llvm::DISubprogram::SPFlagDefinition);
        fn->setSubprogram(debugScope);
    }
    void lower(const Instruction& ins) {
        auto arg = [&](size_t n) { return load(ins.args.at(n)); };
        auto argType = [&](size_t n) -> const Type& { return current->registers.at(ins.args.at(n)); };
        auto voidTy = llvm::Type::getVoidTy(context);
        llvm::Value* result = nullptr;
        switch (ins.op) {
        case Op::ConstI64: result = n64(ins.integer); break;
        case Op::ConstF64: result = llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), ins.real); break;
        case Op::ConstBool: result = llvm::ConstantInt::get(llvm::Type::getInt1Ty(context), ins.integer != 0); break;
        case Op::ConstString: {
            auto* text = builder.CreateGlobalString(ins.text, "literal");
            result = runtime("pf_string", ptr(), {ptr(), i64()}, {text, n64(int64_t(ins.text.size()))});
            break;
        }
        case Op::Move: result = arg(0); break;
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: {
            if (argType(0).kind == TypeKind::F64) {
                auto* a = arg(0); auto* b = arg(1);
                if (ins.op == Op::Add) result = builder.CreateFAdd(a,b);
                if (ins.op == Op::Sub) result = builder.CreateFSub(a,b);
                if (ins.op == Op::Mul) result = builder.CreateFMul(a,b);
                if (ins.op == Op::Div) result = builder.CreateFDiv(a,b);
                if (ins.op == Op::Mod) result = builder.CreateFRem(a,b);
            } else {
                const char* names[] = {"pf_checked_add","pf_checked_sub","pf_checked_mul","pf_checked_div","pf_checked_mod"};
                result = runtime(names[unsigned(ins.op)-unsigned(Op::Add)], i64(), {i64(),i64()}, {arg(0),arg(1)});
            }
            break;
        }
        case Op::Neg:
            result = argType(0).kind == TypeKind::F64 ? builder.CreateFNeg(arg(0))
                : runtime("pf_checked_neg", i64(), {i64()}, {arg(0)});
            break;
        case Op::Not: result = builder.CreateNot(arg(0)); break;
        case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: {
            auto* a = arg(0); auto* b = arg(1);
            unsigned offset = unsigned(ins.op)-unsigned(Op::Eq);
            if (argType(0).kind == TypeKind::F64) {
                const llvm::CmpInst::Predicate predicates[] = {llvm::CmpInst::FCMP_OEQ,llvm::CmpInst::FCMP_UNE,
                    llvm::CmpInst::FCMP_OLT,llvm::CmpInst::FCMP_OLE,llvm::CmpInst::FCMP_OGT,llvm::CmpInst::FCMP_OGE};
                result = builder.CreateFCmp(predicates[offset],a,b);
            } else if (argType(0).kind == TypeKind::String && (ins.op == Op::Eq || ins.op == Op::Ne)) {
                auto* equal = runtime("pf_string_equal", i32(), {ptr(),ptr()}, {a,b});
                result = builder.CreateICmp(ins.op == Op::Eq ? llvm::CmpInst::ICMP_NE : llvm::CmpInst::ICMP_EQ,
                    equal, llvm::ConstantInt::get(i32(),0));
            } else {
                const llvm::CmpInst::Predicate predicates[] = {llvm::CmpInst::ICMP_EQ,llvm::CmpInst::ICMP_NE,
                    llvm::CmpInst::ICMP_SLT,llvm::CmpInst::ICMP_SLE,llvm::CmpInst::ICMP_SGT,llvm::CmpInst::ICMP_SGE};
                result = builder.CreateICmp(predicates[offset],a,b);
            }
            break;
        }
        case Op::Print:
            runtime("pf_print",voidTy,{i64(),i32()},{bits(arg(0),argType(0)),tag(argType(0))}); break;
        case Op::Call: {
            auto it = functions.find(ins.text);
            if (it == functions.end()) throw std::runtime_error("unknown IR callee: " + ins.text);
            std::vector<llvm::Value*> args{null()};
            for (int r : ins.args) args.push_back(load(r));
            result = builder.CreateCall(it->second,args); break;
        }
        case Op::CallIndirect: {
            const auto& closureType = argType(0);
            if (closureType.kind != TypeKind::Function || closureType.args.empty())
                throw std::runtime_error("indirect call requires a function type");
            std::vector<Type> params(closureType.args.begin(),closureType.args.end()-1);
            auto* closure = arg(0);
            auto* code = runtime("pf_closure_code",ptr(),{ptr()},{closure});
            std::vector<llvm::Value*> args{closure};
            for (size_t n=1;n<ins.args.size();++n) args.push_back(arg(n));
            result = builder.CreateCall(signature(params,closureType.args.back(),true),code,args); break;
        }
        case Op::MakeClosure: {
            auto it = functions.find(ins.text);
            if (it == functions.end()) throw std::runtime_error("unknown closure body: " + ins.text);
            result = runtime("pf_closure_new",ptr(),{ptr(),i64()},{it->second,n64(int64_t(ins.args.size()))});
            store(ins.dst,result);
            for (size_t n=0;n<ins.args.size();++n)
                runtime("pf_closure_set",voidTy,{ptr(),i64(),ptr()},{result,n64(int64_t(n)),arg(n)});
            break;
        }
        case Op::Capture:
            result = runtime("pf_closure_get",ptr(),{ptr(),i64()},{environment,n64(ins.integer)}); break;
        case Op::MakeCell:
            if(ins.args.empty()) result=runtime("pf_cell_new",ptr(),{i64(),i32()},
                {n64(0),tag(current->registers.at(ins.dst).args.at(0))});
            else result = runtime("pf_cell_new",ptr(),{i64(),i32()},{bits(arg(0),argType(0)),tag(argType(0))});
            break;
        case Op::LoadCell:
            result = fromBits(runtime("pf_cell_get",i64(),{ptr()},{arg(0)}),current->registers.at(ins.dst)); break;
        case Op::StoreCell:
            runtime("pf_cell_set",voidTy,{ptr(),i64(),i32()},{arg(0),bits(arg(1),argType(1)),tag(argType(1))}); break;
        case Op::MakeArray: case Op::MakeTuple: {
            if (ins.op == Op::MakeArray) {
                auto element = current->registers.at(ins.dst).args.at(0);
                result = runtime("pf_array_new",ptr(),{i64(),i32()},{n64(int64_t(ins.args.size())),tag(element)});
            } else result = runtime("pf_tuple_new",ptr(),{i64()},{n64(int64_t(ins.args.size()))});
            store(ins.dst,result);
            for (size_t n=0;n<ins.args.size();++n)
                runtime("pf_set_index",voidTy,{ptr(),i64(),i64(),i32()},
                    {result,n64(int64_t(n)),bits(arg(n),argType(n)),tag(argType(n))});
            break;
        }
        case Op::Index:
            result = fromBits(runtime("pf_index",i64(),{ptr(),i64()},{arg(0),arg(1)}),current->registers.at(ins.dst)); break;
        case Op::SetIndex:
            runtime("pf_set_index",voidTy,{ptr(),i64(),i64(),i32()},
                {arg(0),arg(1),bits(arg(2),argType(2)),tag(argType(2))}); break;
        case Op::Length: result = runtime("pf_length",i64(),{ptr()},{arg(0)}); break;
        case Op::Convert: {
            const auto& destination = current->registers.at(ins.dst);
            if (destination == argType(0)) result = arg(0);
            else if (destination.kind == TypeKind::F64 && argType(0).kind == TypeKind::I64)
                result = builder.CreateSIToFP(arg(0),type(destination));
            else if (destination.kind == TypeKind::I64 && argType(0).kind == TypeKind::F64)
                result = runtime("pf_float_to_int",i64(),{llvm::Type::getDoubleTy(context)},{arg(0)});
            else throw std::runtime_error("unsupported IR conversion");
            break;
        }
        case Op::Jump: builder.CreateBr(blocks.at(size_t(ins.target))); break;
        case Op::JumpFalse: {
            const auto index = size_t(&ins-current->code.data());
            if (index+1 >= blocks.size()) throw std::runtime_error("conditional branch has no fallthrough");
            builder.CreateCondBr(arg(0),blocks[index+1],blocks.at(size_t(ins.target))); break;
        }
        case Op::Return: {
            auto* value = ins.args.empty() ? nullptr : arg(0);
            runtime("pf_roots_pop",voidTy,{ptr()},{rootFrame});
            if (current->result.kind == TypeKind::Unit) builder.CreateRetVoid();
            else builder.CreateRet(value);
            break;
        }
        case Op::Collect: runtime("pf_collect",voidTy,{},{}); break;
        case Op::Panic:
            runtime("pf_panic",voidTy,{ptr()},{arg(0)});
            builder.CreateUnreachable(); break;
        case Op::Unwrap:
            result=fromBits(runtime("pf_result_value",i64(),{ptr()},{arg(0)}),current->registers.at(ins.dst)); break;
        case Op::RawAlloc:
            result=runtime("pf_raw_alloc",ptr(),{i64(),i32()},
                {arg(0),tag(current->registers.at(ins.dst).args.at(0))}); break;
        case Op::RawFree:
            runtime("pf_raw_free",voidTy,{ptr()},{arg(0)}); break;
        case Op::RawLoad:
            result=fromBits(runtime("pf_raw_load",i64(),{ptr(),i64(),i32()},
                {arg(0),arg(1),tag(current->registers.at(ins.dst))}),current->registers.at(ins.dst)); break;
        case Op::RawStore:
            runtime("pf_raw_store",voidTy,{ptr(),i64(),i64(),i32()},
                {arg(0),arg(1),bits(arg(2),argType(2)),tag(argType(2))}); break;
        case Op::CString:
            result=runtime("pf_cstr",ptr(),{ptr()},{arg(0)}); break;
        case Op::Pin:
            result=runtime("pf_pin",ptr(),{ptr()},{arg(0)}); break;
        case Op::Unpin:
            runtime("pf_unpin",voidTy,{ptr()},{arg(0)}); break;
        case Op::Zero:
            result=llvm::Constant::getNullValue(slotType(current->registers.at(ins.dst))); break;
        case Op::ResultError:
            result=fromBits(runtime("pf_result_error",i64(),{ptr()},{arg(0)}),current->registers.at(ins.dst)); break;
        case Op::ExternalCall: {
            auto it = std::find_if(program.externals.begin(),program.externals.end(),
                [&](const External& e){return e.name==ins.text;});
            if (it==program.externals.end()) throw std::runtime_error("unknown external: " + ins.text);
            std::vector<llvm::Type*> parameters;
            for (const auto& t : it->parameters) {
                if (t.managed()) throw std::runtime_error("managed objects require an explicit C handle; direct extern arguments are unsupported");
                parameters.push_back(type(t));
            }
            std::vector<llvm::Value*> args;
            for (size_t n=0;n<ins.args.size();++n) {
                auto* value=arg(n);
                if (n>=it->parameters.size() && argType(n).kind==TypeKind::Bool)
                    value=builder.CreateZExt(value,i32());
                args.push_back(value);
            }
            auto callee=module.getOrInsertFunction(ins.text,
                llvm::FunctionType::get(type(it->result),parameters,it->variadic));
            auto* call=builder.CreateCall(callee,args);
            for(size_t n=0;n<it->parameters.size();++n)
                if(it->parameters[n].kind==TypeKind::Bool) call->addParamAttr(unsigned(n),llvm::Attribute::ZExt);
            if(it->result.kind==TypeKind::Bool) call->addRetAttr(llvm::Attribute::ZExt);
            result=call; break;
        }
        }
        store(ins.dst,result);
    }
    void emitFunction(const Function& source) {
        current=&source;
        auto* fn=functions.at(source.name);
        auto* entry=llvm::BasicBlock::Create(context,"entry",fn);
        builder.SetInsertPoint(entry);
        builder.SetCurrentDebugLocation({});
        setupDebug(fn,source);
        registers.clear(); roots.assign(source.registers.size(),nullptr); blocks.clear();
        size_t count=0;
        for (const auto& t:source.registers) if(t.managed()) ++count;
        auto* rootArray=builder.CreateAlloca(ptr(),n64(int64_t(std::max<size_t>(1,count))),"gc_roots");
        size_t rootIndex=0;
        for(size_t n=0;n<source.registers.size();++n) {
            auto* slot=builder.CreateAlloca(slotType(source.registers[n]),nullptr,"v"+std::to_string(n));
            builder.CreateStore(llvm::Constant::getNullValue(slotType(source.registers[n])),slot);
            registers.push_back(slot);
            if(source.registers[n].managed()) {
                roots[n]=builder.CreateGEP(ptr(),rootArray,n64(int64_t(rootIndex++)));
                builder.CreateStore(null(),roots[n]);
            }
        }
        auto argument=fn->arg_begin(); environment=&*argument++; environment->setName("closure_env");
        for(size_t n=0;n<source.parameters.size();++n) {
            auto* value=&*argument++;
            const std::string name=n<source.parameterNames.size()?source.parameterNames[n]:"arg"+std::to_string(n);
            value->setName(name); store(int(n),value);
            if(debugBuilder) {
                auto* variable=debugBuilder->createParameterVariable(debugScope,name,unsigned(n+1),debugFile,
                    source.location.line,debugType(source.parameters[n]),true);
                debugBuilder->insertDeclare(registers[n],variable,debugBuilder->createExpression(),
                    llvm::DILocation::get(context,source.location.line,1,debugScope),entry);
            }
        }
        rootFrame=runtime("pf_roots_push",ptr(),{ptr(),i64()},{rootArray,n64(int64_t(count))});
        for(size_t n=0;n<source.code.size();++n)
            blocks.push_back(llvm::BasicBlock::Create(context,"op"+std::to_string(n),fn));
        if(blocks.empty()) throw std::runtime_error("IR function has no instructions: "+source.name);
        builder.CreateBr(blocks.front());
        for(size_t n=0;n<source.code.size();++n) {
            builder.SetInsertPoint(blocks[n]);
            if(debugBuilder) builder.SetCurrentDebugLocation(llvm::DILocation::get(context,
                source.code[n].location.line,source.code[n].location.column,debugScope));
            lower(source.code[n]);
            if(!blocks[n]->getTerminator()) {
                if(n+1<blocks.size()) builder.CreateBr(blocks[n+1]);
                else {
                    runtime("pf_roots_pop",llvm::Type::getVoidTy(context),{ptr()},{rootFrame});
                    if(source.result.kind==TypeKind::Unit) builder.CreateRetVoid();
                    else throw std::runtime_error("IR function falls through without returning: "+source.name);
                }
            }
        }
        if(debugBuilder) debugBuilder->finalizeSubprogram(debugScope);
        builder.SetCurrentDebugLocation({});
    }
public:
    Emitter(const Program& p,llvm::LLVMContext& c,llvm::Module& m,bool debug)
        : program(p),context(c),module(m),builder(c) {
        if(debug) {
            debugBuilder=std::make_unique<llvm::DIBuilder>(module);
            std::filesystem::path path(program.sourceFile);
            debugFile=debugBuilder->createFile(path.filename().string(),path.parent_path().string());
            debugBuilder->createCompileUnit(llvm::dwarf::DW_LANG_C,debugFile,"PikaFlow",false,"",0);
            module.addModuleFlag(llvm::Module::Warning,"Debug Info Version",llvm::DEBUG_METADATA_VERSION);
            if(module.getTargetTriple().isOSWindows()) module.addModuleFlag(llvm::Module::Warning,"CodeView",1);
            else module.addModuleFlag(llvm::Module::Warning,"Dwarf Version",4);
        }
    }
    void emit() {
        for(const auto& fn:program.functions) {
            auto* native=llvm::Function::Create(signature(fn.parameters,fn.result,true),
                llvm::Function::ExternalLinkage,nativeName(fn.name),module);
            functions.emplace(fn.name,native);
        }
        for(const auto& fn:program.functions) emitFunction(fn);
        auto it=functions.find(program.entry);
        if(it==functions.end()) throw std::runtime_error("program has no entry function");
        const auto& source=*std::find_if(program.functions.begin(),program.functions.end(),
            [&](const Function& f){return f.name==program.entry;});
        if(!source.parameters.empty() || (source.result.kind!=TypeKind::Unit && source.result.kind!=TypeKind::I64))
            throw std::runtime_error("main must take no arguments and return unit or i64");
        auto* wrapper=llvm::Function::Create(llvm::FunctionType::get(i32(),false),
            llvm::Function::ExternalLinkage,"main",module);
        builder.SetInsertPoint(llvm::BasicBlock::Create(context,"entry",wrapper));
        auto* result=builder.CreateCall(it->second,{null()});
        builder.CreateRet(source.result.kind==TypeKind::Unit?llvm::ConstantInt::get(i32(),0):builder.CreateTrunc(result,i32()));
        if(debugBuilder) debugBuilder->finalize();
    }
};

struct Generated {
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::TargetMachine> target;
};
Generated generate(const Program& program,unsigned optimization,bool debug) {
    if(optimization>3) throw std::runtime_error("optimization level must be between 0 and 3");
    verify(program);
    Generated g;
    g.context=std::make_unique<llvm::LLVMContext>();
    g.module=std::make_unique<llvm::Module>("PikaFlow",*g.context);
    g.target=makeTarget(optimization);
    g.module->setTargetTriple(g.target->getTargetTriple());
    g.module->setDataLayout(g.target->createDataLayout());
    Emitter(program,*g.context,*g.module,debug).emit();
    std::string message;
    llvm::raw_string_ostream errors(message);
    if(llvm::verifyModule(*g.module,&errors)) throw std::runtime_error("invalid generated LLVM IR:\n"+errors.str());
    llvm::LoopAnalysisManager loops;
    llvm::FunctionAnalysisManager functions;
    llvm::CGSCCAnalysisManager cgscc;
    llvm::ModuleAnalysisManager modules;
    llvm::PassBuilder passes(*g.context,g.target.get());
    passes.registerModuleAnalyses(modules); passes.registerCGSCCAnalyses(cgscc);
    passes.registerFunctionAnalyses(functions); passes.registerLoopAnalyses(loops);
    passes.crossRegisterProxies(loops,functions,cgscc,modules);
    auto level=optimization==0?llvm::OptimizationLevel::O0:
        optimization==1?llvm::OptimizationLevel::O1:
        optimization==2?llvm::OptimizationLevel::O2:llvm::OptimizationLevel::O3;
    auto pipeline=optimization?passes.buildPerModuleDefaultPipeline(level)
        :passes.buildO0DefaultPipeline(level);
    pipeline.run(*g.module,modules);
    if(optimization) {
        llvm::ModulePassManager custom;
        custom.addPass(llvm::createModuleToFunctionPassAdaptor(CheckedIdentityPass()));
        custom.run(*g.module,modules);
    }
    if(llvm::verifyModule(*g.module,&errors)) throw std::runtime_error("invalid optimized LLVM IR:\n"+errors.str());
    return g;
}
}

std::string targetTriple() { initializeLLVM(); return llvm::sys::getDefaultTargetTriple(); }
std::string emitLLVM(const Program& program,unsigned optimization,bool debug) {
    auto g=generate(program,optimization,debug);
    std::string text; llvm::raw_string_ostream output(text); g.module->print(output,nullptr); return output.str();
}
void emitObject(const Program& program,const std::string& output,unsigned optimization,bool debug) {
    auto g=generate(program,optimization,debug);
    std::error_code error;
    llvm::raw_fd_ostream stream(output,error,llvm::sys::fs::OF_None);
    if(error) throw std::runtime_error("cannot write object file: "+error.message());
    llvm::legacy::PassManager passes;
    if(g.target->addPassesToEmitFile(passes,stream,nullptr,llvm::CodeGenFileType::ObjectFile))
        throw std::runtime_error("target does not support object emission");
    passes.run(*g.module); stream.flush();
    if(stream.has_error()) throw std::runtime_error("failed writing object file");
}
std::string emitAssembly(const Program& program,unsigned optimization,bool debug) {
    auto g=generate(program,optimization,debug);
    llvm::SmallVector<char,0> bytes;
    llvm::raw_svector_ostream stream(bytes);
    llvm::legacy::PassManager passes;
    if(g.target->addPassesToEmitFile(passes,stream,nullptr,llvm::CodeGenFileType::AssemblyFile))
        throw std::runtime_error("target does not support assembly emission");
    passes.run(*g.module);
    return std::string(bytes.begin(),bytes.end());
}
int runJIT(const Program& program,unsigned optimization) {
    auto generated=generate(program,optimization,false);
    auto jitExpected=llvm::orc::LLJITBuilder().create();
    if(!jitExpected) throw std::runtime_error(llvm::toString(jitExpected.takeError()));
    auto jit=std::move(*jitExpected);
    llvm::orc::MangleAndInterner mangle(jit->getExecutionSession(),jit->getDataLayout());
    llvm::orc::SymbolMap symbols;
#define PF_JIT_SYMBOL(name) symbols[mangle(#name)] = llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr::fromPtr(&name),llvm::JITSymbolFlags::Exported)
    PF_JIT_SYMBOL(pf_print); PF_JIT_SYMBOL(pf_fail); PF_JIT_SYMBOL(pf_string);
    PF_JIT_SYMBOL(pf_array_new); PF_JIT_SYMBOL(pf_tuple_new); PF_JIT_SYMBOL(pf_length);
    PF_JIT_SYMBOL(pf_index); PF_JIT_SYMBOL(pf_index_tag); PF_JIT_SYMBOL(pf_set_index);
    PF_JIT_SYMBOL(pf_cell_new); PF_JIT_SYMBOL(pf_cell_get); PF_JIT_SYMBOL(pf_cell_set);
    PF_JIT_SYMBOL(pf_closure_new); PF_JIT_SYMBOL(pf_closure_set); PF_JIT_SYMBOL(pf_closure_get);
    PF_JIT_SYMBOL(pf_closure_code); PF_JIT_SYMBOL(pf_roots_push); PF_JIT_SYMBOL(pf_roots_pop);
    PF_JIT_SYMBOL(pf_collect); PF_JIT_SYMBOL(pf_heap_objects); PF_JIT_SYMBOL(pf_checked_add);
    PF_JIT_SYMBOL(pf_checked_sub); PF_JIT_SYMBOL(pf_checked_mul); PF_JIT_SYMBOL(pf_checked_div);
    PF_JIT_SYMBOL(pf_checked_mod); PF_JIT_SYMBOL(pf_checked_neg); PF_JIT_SYMBOL(pf_float_to_int);
    PF_JIT_SYMBOL(pf_string_equal);
    PF_JIT_SYMBOL(pf_panic); PF_JIT_SYMBOL(pf_result_value);
    PF_JIT_SYMBOL(pf_raw_alloc); PF_JIT_SYMBOL(pf_raw_free); PF_JIT_SYMBOL(pf_raw_load);
    PF_JIT_SYMBOL(pf_raw_store); PF_JIT_SYMBOL(pf_cstr); PF_JIT_SYMBOL(pf_pin); PF_JIT_SYMBOL(pf_unpin);
    PF_JIT_SYMBOL(pf_result_error);
#undef PF_JIT_SYMBOL
    if(auto error=jit->getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(symbols))))
        throw std::runtime_error(llvm::toString(std::move(error)));
    auto generator=llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(jit->getDataLayout().getGlobalPrefix());
    if(!generator) throw std::runtime_error(llvm::toString(generator.takeError()));
    jit->getMainJITDylib().addGenerator(std::move(*generator));
    if(auto error=jit->addIRModule(llvm::orc::ThreadSafeModule(std::move(generated.module),std::move(generated.context))))
        throw std::runtime_error(llvm::toString(std::move(error)));
    auto entry=jit->lookup("main");
    if(!entry) throw std::runtime_error(llvm::toString(entry.takeError()));
    return entry->toPtr<int(*)()>()();
}
void buildExecutable(const Program& program,const std::string& output,const std::string& runtimeLibrary,
    const std::string& compilerDriver,unsigned optimization,bool debug) {
    std::filesystem::path final(output);
    if(!final.parent_path().empty()) std::filesystem::create_directories(final.parent_path());
    std::string object=output+".obj";
    emitObject(program,object,optimization,debug);
    std::vector<std::string> arguments;
    std::string driverName=std::filesystem::path(compilerDriver).filename().string();
    std::transform(driverName.begin(),driverName.end(),driverName.begin(),[](unsigned char c){return char(std::tolower(c));});
    if(driverName=="cl.exe" || driverName=="cl") {
        arguments={compilerDriver,"/nologo",object,runtimeLibrary,"/Fe:"+output};
        if(debug) { arguments.push_back("/link"); arguments.push_back("/DEBUG"); }
    } else {
        arguments={compilerDriver,object,runtimeLibrary,"-o",output};
        if(debug) arguments.push_back("-g");
#ifndef _WIN32
        arguments.push_back("-lm"); arguments.push_back("-ldl"); arguments.push_back("-pthread");
#endif
    }
    std::vector<llvm::StringRef> refs;
    for(const auto& argument:arguments) refs.emplace_back(argument);
    auto driver=llvm::sys::findProgramByName(compilerDriver);
    if(!driver) throw std::runtime_error("cannot find native link driver: "+compilerDriver);
    std::string message;
    int result=llvm::sys::ExecuteAndWait(*driver,refs,std::nullopt,{},0,0,&message);
    if(result!=0) throw std::runtime_error("native link failed ("+std::to_string(result)+"): "+message);
    std::error_code ignored;
    std::filesystem::remove(object,ignored);
}
}
