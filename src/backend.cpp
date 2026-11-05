#include "pikaflow/backend.hpp"
#include "pikaflow/optimizer.hpp"
#include "llvm/CheckedArithmeticPass.hpp"
#include "pikaflow/runtime.h"
#include "pikaflow/backend_config.hpp"

#include "llvm/ADT/SmallVector.h"
#ifndef _WIN32
#include "llvm/ABI/FunctionInfo.h"
#include "llvm/ABI/IRTypeMapper.h"
#include "llvm/ABI/TargetInfo.h"
#include "llvm/ABI/Types.h"
#include "llvm/Support/Allocator.h"
#endif
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
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
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
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>
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

class Emitter {
    const Program& program;
    llvm::LLVMContext& context;
    llvm::Module& module;
    llvm::IRBuilder<> builder;
    std::map<std::string, llvm::Function*> functions;
    std::unique_ptr<llvm::DIBuilder> debugBuilder;
    llvm::DIFile* debugFile = nullptr;
    llvm::DISubprogram* debugScope = nullptr;
    std::map<std::string,llvm::Constant*> sourceFiles;
    std::map<std::string,llvm::DILocalVariable*> debugParameters;
    const Function* current = nullptr;
    llvm::Value* environment = nullptr;
    llvm::Value* rootFrame = nullptr;
    llvm::Value* taskScope = nullptr;
    std::map<std::string,llvm::Function*> taskEntries;
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
        if(auto* function = llvm::dyn_cast<llvm::Function>(callee.getCallee())) markCheckedArithmeticRuntime(*function);
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
            auto path=std::filesystem::u8path(source.location.file);
            debugFile=debugBuilder->createFile(path.filename().u8string(),path.parent_path().u8string());
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
    void sourceLocation(const Location& location) {
        const auto& file=location.file.empty()?program.sourceFile:location.file;
        auto found=sourceFiles.find(file);
        if(found==sourceFiles.end())found=sourceFiles.emplace(file,builder.CreateGlobalString(file,"source_file")).first;
        runtime("pf_source_location",llvm::Type::getVoidTy(context),{ptr(),i32(),i32()},
            {found->second,llvm::ConstantInt::get(i32(),location.line),llvm::ConstantInt::get(i32(),location.column)});
    }
    void debugLocal(const Instruction& ins) {
        if(!debugBuilder || ins.dst<0 || (ins.op!=Op::MakeCell && ins.op!=Op::Capture))return;
        auto found=current->localNames.find(ins.dst);
        if(found==current->localNames.end())return;
        auto* address=runtime("pf_cell_address",ptr(),{ptr()},{load(ins.dst)});
        llvm::DILocalVariable* variable=nullptr;
        auto parameter=debugParameters.find(found->second);
        if(ins.op==Op::MakeCell && parameter!=debugParameters.end() && ins.location.line==current->location.line)
            variable=parameter->second;
        else variable=debugBuilder->createAutoVariable(debugScope,found->second,debugFile,
            ins.location.line,debugType(current->registers.at(ins.dst).args.at(0)),true);
        // Spill the stable cell payload address so debuggers can describe it
        // after calls, rather than losing a transient SSA address at -O0.
        auto& entry=builder.GetInsertBlock()->getParent()->getEntryBlock();
        llvm::IRBuilder<> prologue(entry.begin());
        auto* debugAddress=prologue.CreateAlloca(ptr(),nullptr,"debug_address_"+found->second);
        builder.CreateStore(address,debugAddress);
        debugBuilder->insertDeclare(debugAddress,variable,debugBuilder->createExpression({llvm::dwarf::DW_OP_deref}),
            llvm::DILocation::get(context,ins.location.line,ins.location.column,debugScope),builder.GetInsertBlock());
    }
    const CABIType& cField(const CABIType& native,size_t index) {
        static const CABIType ordinary;
        return native.fields.empty()?ordinary:native.fields.at(index);
    }
    llvm::Type* cMemoryType(const Type& source,const CABIType& native={}) {
        if(!source.name.empty())throw std::runtime_error("nominal managed types are not C records");
        if(native.bits)return llvm::IntegerType::get(context,native.bits);
        if(source.kind==TypeKind::Bool) return llvm::Type::getInt8Ty(context);
        if(source.kind!=TypeKind::Tuple) {
            if(source.managed()) throw std::runtime_error("C records cannot contain managed references");
            return type(source);
        }
        if(source.args.empty()) throw std::runtime_error("empty C records are unsupported");
        std::vector<llvm::Type*> fields;
        for(size_t n=0;n<source.args.size();++n) {
            const auto& field=source.args[n];
            if(field.kind!=TypeKind::I64 && field.kind!=TypeKind::F64 &&
                field.kind!=TypeKind::Bool && field.kind!=TypeKind::Pointer && field.kind!=TypeKind::Tuple)
                throw std::runtime_error("C records require scalar, raw-pointer, or nested record fields");
            fields.push_back(cMemoryType(field,cField(native,n)));
        }
        return llvm::StructType::get(context,fields,false);
    }
    llvm::AllocaInst* cTemporary(llvm::Type* memoryType,llvm::Type* coercion=nullptr,unsigned offset=0) {
        auto& entry=builder.GetInsertBlock()->getParent()->getEntryBlock();
        llvm::IRBuilder<> prologue(entry.begin());
        auto& layout=module.getDataLayout();
        auto bytes=layout.getTypeAllocSize(memoryType).getFixedValue();
        auto alignment=layout.getABITypeAlign(memoryType);
        llvm::Type* storage=memoryType;
        if(coercion) {
            // Register coercions can round a 12-byte C record to a 16-byte IR
            // struct. Reserve its padding before loading or storing that type.
            auto required=layout.getTypeStoreSize(coercion).getFixedValue()+offset;
            if(required>bytes) {bytes=required;storage=llvm::ArrayType::get(llvm::Type::getInt8Ty(context),bytes);}
            alignment=std::max(alignment,layout.getABITypeAlign(coercion));
        }
        auto* slot=prologue.CreateAlloca(storage,nullptr,"c_record");
        if(module.getTargetTriple().isOSWindows()) alignment=std::max(alignment,llvm::Align(16));
        slot->setAlignment(alignment);
        builder.CreateMemSet(slot,llvm::ConstantInt::get(llvm::Type::getInt8Ty(context),0),
            bytes,alignment);
        return slot;
    }
    llvm::Value* cIntegerArgument(llvm::Value* value,const CABIType& native) {
        if(!native.bits)return value;
        value=runtime("pf_c_integer",i64(),{i64(),i32(),i32()},
            {value,llvm::ConstantInt::get(i32(),native.bits),llvm::ConstantInt::get(i32(),native.isSigned)});
        return builder.CreateTruncOrBitCast(value,llvm::IntegerType::get(context,native.bits));
    }
    llvm::Value* cIntegerResult(llvm::Value* value,const CABIType& native) {
        if(!native.bits)return value;
        return native.isSigned?builder.CreateSExtOrTrunc(value,i64()):builder.CreateZExtOrTrunc(value,i64());
    }
    void packCFields(llvm::Value* object,const Type& source,const CABIType& native,llvm::Value* slot) {
        auto* memoryType=llvm::cast<llvm::StructType>(cMemoryType(source,native));
        for(size_t n=0;n<source.args.size();++n) {
            auto* field=fromBits(runtime("pf_index",i64(),{ptr(),i64()},{object,n64(int64_t(n))}),source.args[n]);
            auto* address=builder.CreateStructGEP(memoryType,slot,unsigned(n));
            if(source.args[n].kind==TypeKind::Tuple) {
                packCFields(field,source.args[n],cField(native,n),address);continue;
            }
            if(source.args[n].kind==TypeKind::Bool) field=builder.CreateZExt(field,llvm::Type::getInt8Ty(context));
            if(source.args[n].kind==TypeKind::I64)field=cIntegerArgument(field,cField(native,n));
            builder.CreateStore(field,address);
        }
    }
    llvm::Value* packCRecord(int reg,const CABIType& native,llvm::Type* coercion,unsigned offset) {
        const auto& source=current->registers.at(reg);
        auto* slot=cTemporary(cMemoryType(source,native),coercion,offset);
        packCFields(load(reg),source,native,slot);
        return slot;
    }
    llvm::Value* unpackCRecord(const Type& source,const CABIType& native,llvm::Value* slot) {
        auto* memoryType=llvm::cast<llvm::StructType>(cMemoryType(source,native));
        auto* tuple=runtime("pf_tuple_new",ptr(),{i64()},{n64(int64_t(source.args.size()))});
        auto& entry=builder.GetInsertBlock()->getParent()->getEntryBlock();
        llvm::IRBuilder<> prologue(entry.begin());
        auto* temporaryRoot=prologue.CreateAlloca(ptr(),nullptr,"c_result_root");
        builder.CreateStore(tuple,temporaryRoot);
        auto* frame=runtime("pf_roots_push",ptr(),{ptr(),i64()},{temporaryRoot,n64(1)});
        for(size_t n=0;n<source.args.size();++n) {
            const auto& field=source.args[n];
            auto* address=builder.CreateStructGEP(memoryType,slot,unsigned(n));
            llvm::Value* value;
            if(field.kind==TypeKind::Tuple)value=unpackCRecord(field,cField(native,n),address);
            else {
                value=builder.CreateLoad(cMemoryType(field,cField(native,n)),address);
                if(field.kind==TypeKind::Bool)value=builder.CreateTrunc(value,type(field));
                if(field.kind==TypeKind::I64)value=cIntegerResult(value,cField(native,n));
            }
            runtime("pf_set_index",llvm::Type::getVoidTy(context),{ptr(),i64(),i64(),i32()},
                {tuple,n64(int64_t(n)),bits(value,field),tag(field)});
        }
        runtime("pf_roots_pop",llvm::Type::getVoidTy(context),{ptr()},{frame});
        return tuple;
    }
    struct CArg {
        llvm::Type* wire=nullptr;
        bool indirect=false,byval=false,flatten=false,ignore=false;
        bool signExtend=false,zeroExtend=false;
        unsigned offset=0;
        llvm::Align alignment{1};
    };
#ifndef _WIN32
    const llvm::abi::Type* abiType(const Type& source,const CABIType& native,llvm::abi::TypeBuilder& types) {
        auto& layout=module.getDataLayout();
        if(native.bits)return types.getIntegerType(native.bits,llvm::Align(native.bits/8),native.isSigned);
        switch(source.kind) {
        case TypeKind::Unit:return types.getVoidType();
        case TypeKind::I64:return types.getIntegerType(64,llvm::Align(8),true);
        case TypeKind::F64:return types.getFloatType(llvm::APFloat::IEEEdouble(),llvm::Align(8));
        case TypeKind::Bool:return types.getIntegerType(8,llvm::Align(1),false);
        case TypeKind::Pointer:return types.getPointerType(64,llvm::Align(8));
        case TypeKind::Tuple: {
            auto* memory=llvm::cast<llvm::StructType>(cMemoryType(source,native));
            auto* recordLayout=layout.getStructLayout(memory);
            std::vector<llvm::abi::FieldInfo> fields;
            for(size_t n=0;n<source.args.size();++n)
                fields.emplace_back(abiType(source.args[n],cField(native,n),types),recordLayout->getElementOffsetInBits(unsigned(n)));
            auto alignment=layout.getABITypeAlign(memory);
            // Plain C records are PODs. Without this flag the classifier models
            // a C++ record that must travel through an invisible reference.
            return types.getRecordType(fields,layout.getTypeAllocSize(memory)*8,alignment,alignment,
                llvm::abi::StructPacking::Default,{}, {},llvm::abi::RecordFlags::CanPassInRegisters);
        }
        default:throw std::runtime_error("unsupported C ABI type");
        }
    }
    CArg sysvArgument(const llvm::abi::ArgInfo& argument,const Type& source,const CABIType& native,llvm::abi::IRTypeMapper& mapper) {
        CArg result;
        if(argument.isIgnore()) { result.ignore=true; return result; }
        result.alignment=module.getDataLayout().getABITypeAlign(cMemoryType(source,native));
        if(argument.isExtend()) {result.signExtend=argument.isSignExt();result.zeroExtend=argument.isZeroExt();}
        if(source.kind!=TypeKind::Tuple) { result.wire=native.bits?cMemoryType(source,native):type(source); return result; }
        if(argument.isIndirect()) {
            result.indirect=true;result.byval=argument.getIndirectByVal();
            result.alignment=argument.getIndirectAlign(); result.wire=ptr(); return result;
        }
        if(!argument.isDirect()) throw std::runtime_error("unsupported C aggregate ABI classification");
        result.wire=argument.getCoerceToType()?mapper.convertType(argument.getCoerceToType()):cMemoryType(source,native);
        result.offset=argument.getDirectOffset();
        result.flatten=argument.getCanBeFlattened() && result.wire->isStructTy();
        return result;
    }
    void classifySysv(const External& declaration,const std::vector<Type>& sourceTypes,const std::vector<CABIType>& nativeTypes,
        CArg& returnPlan,std::vector<CArg>& plans) {
        if(module.getTargetTriple().getArch()!=llvm::Triple::x86_64 || !module.getTargetTriple().isOSLinux())
            throw std::runtime_error("this C ABI classifier requires Linux x86-64 SysV");
        llvm::BumpPtrAllocator allocator;
        llvm::abi::TypeBuilder types(allocator);
        llvm::abi::IRTypeMapper mapper(context,module.getDataLayout());
        std::vector<const llvm::abi::Type*> arguments;
        for(size_t n=0;n<sourceTypes.size();++n) {
            if(n>=declaration.parameters.size() && sourceTypes[n].kind==TypeKind::Bool)
                arguments.push_back(types.getIntegerType(32,llvm::Align(4),true));
            else arguments.push_back(abiType(sourceTypes[n],nativeTypes[n],types));
        }
        auto information=llvm::abi::FunctionInfo::create(llvm::CallingConv::C,abiType(declaration.result,declaration.abiResult,types),arguments,
            declaration.variadic?llvm::abi::RequiredArgs(unsigned(declaration.parameters.size())):
                llvm::abi::RequiredArgs(llvm::abi::RequiredArgs::All));
        auto target=llvm::abi::createX86_64TargetInfo(types,llvm::abi::X86AVXABILevel::None,true,{});
        target->computeInfo(*information);
        returnPlan=sysvArgument(information->getReturnInfo(),declaration.result,declaration.abiResult,mapper);
        for(size_t n=0;n<sourceTypes.size();++n)
            plans.push_back(sysvArgument(information->getArgInfo(unsigned(n)).Info,sourceTypes[n],nativeTypes[n],mapper));
    }
#endif
    llvm::Function* cCallback(const std::string& name) {
        auto source=std::find_if(program.functions.begin(),program.functions.end(),
            [&](const Function& f){return f.name==name;});
        if(source==program.functions.end())throw std::runtime_error("unknown C callback function: "+name);
        if(std::any_of(source->code.begin(),source->code.end(),[](const Instruction& i){return i.op==Op::Capture;}))
            throw std::runtime_error("C callbacks cannot capture an environment");
        auto scalar=[](const Type& t){return t.kind==TypeKind::I64 || t.kind==TypeKind::F64 ||
            t.kind==TypeKind::Bool || t.kind==TypeKind::Pointer;};
        if(source->result!=Unit && !scalar(source->result))
            throw std::runtime_error("C callbacks require a scalar or unit result");
        for(const auto& parameter:source->parameters)
            if(!scalar(parameter))throw std::runtime_error("C callbacks require scalar parameters");
        auto wrapperName="pf_c_callback_"+nativeName(name);
        if(auto* existing=module.getFunction(wrapperName))return existing;
        auto* wrapper=llvm::Function::Create(signature(source->parameters,source->result,false),
            llvm::GlobalValue::InternalLinkage,wrapperName,module);
        auto* entry=llvm::BasicBlock::Create(context,"entry",wrapper);
        llvm::IRBuilder<> callbackBuilder(entry);
        std::vector<llvm::Value*> arguments{null()};
        for(auto& argument:wrapper->args()) {
            if(source->parameters[argument.getArgNo()].kind==TypeKind::Bool)
                wrapper->addParamAttr(argument.getArgNo(),llvm::Attribute::ZExt);
            arguments.push_back(&argument);
        }
        if(source->result.kind==TypeKind::Bool)wrapper->addRetAttr(llvm::Attribute::ZExt);
        auto* returned=callbackBuilder.CreateCall(functions.at(name),arguments);
        if(source->result==Unit)callbackBuilder.CreateRetVoid();else callbackBuilder.CreateRet(returned);
        return wrapper;
    }
    llvm::Function* taskEntry(const Type& resultType) {
        auto key=resultType.str();
        auto existing=taskEntries.find(key);
        if(existing!=taskEntries.end())return existing->second;
        auto* entry=llvm::Function::Create(llvm::FunctionType::get(i64(),{ptr(),ptr()},false),
            llvm::GlobalValue::InternalLinkage,"pf_task_entry_"+std::to_string(taskEntries.size()),module);
        taskEntries.emplace(key,entry);
        llvm::IRBuilder<> worker(llvm::BasicBlock::Create(context,"entry",entry));
        auto* closure=entry->getArg(0);
        auto codeFunction=module.getOrInsertFunction("pf_closure_code",llvm::FunctionType::get(ptr(),{ptr()},false));
        auto* code=worker.CreateCall(codeFunction,{closure});
        auto* value=worker.CreateCall(signature({},resultType,true),code,{closure});
        llvm::Value* packed;
        if(resultType==Unit)packed=n64(0);
        else if(resultType.kind==TypeKind::I64)packed=value;
        else if(resultType.kind==TypeKind::F64)packed=worker.CreateBitCast(value,i64());
        else if(resultType.kind==TypeKind::Bool)packed=worker.CreateZExt(value,i64());
        else packed=worker.CreatePtrToInt(value,i64());
        worker.CreateRet(packed);
        return entry;
    }
    llvm::Value* external(const Instruction& ins) {
        auto declaration=std::find_if(program.externals.begin(),program.externals.end(),
            [&](const External& e){return e.name==ins.text;});
        if(declaration==program.externals.end()) throw std::runtime_error("unknown external: "+ins.text);
        std::vector<Type> sourceTypes;
        for(int reg:ins.args) sourceTypes.push_back(current->registers.at(reg));
        std::vector<CABIType> nativeTypes(sourceTypes.size());
        for(size_t n=0;n<declaration->abiParameters.size();++n)nativeTypes[n]=declaration->abiParameters[n];
        std::vector<CArg> plans;
        CArg returnPlan;
        const auto& returnType=declaration->result;
        bool aggregate=returnType.kind==TypeKind::Tuple;
        for(const auto& t:sourceTypes) aggregate|=t.kind==TypeKind::Tuple;
        if(aggregate && module.getTargetTriple().getArch()!=llvm::Triple::x86_64)
            throw std::runtime_error("C aggregate ABI is implemented only for x86-64");
#ifdef _WIN32
        auto classify=[&](const Type& source,const CABIType& native) {
            CArg result;
            if(source.kind==TypeKind::Unit) {result.ignore=true;return result;}
            if(source.kind!=TypeKind::Tuple) {
                if(source.managed()) throw std::runtime_error("managed references require explicit C handles");
                result.wire=native.bits?cMemoryType(source,native):type(source);
                result.zeroExtend=source.kind==TypeKind::Bool;return result;
            }
            auto* record=cMemoryType(source,native);
            auto bytes=module.getDataLayout().getTypeAllocSize(record).getFixedValue();
            result.alignment=module.getDataLayout().getABITypeAlign(record);
            // Clang's WinX86_64ABIInfo uses integer coercion only at these sizes.
            if(bytes==1 || bytes==2 || bytes==4 || bytes==8)
                result.wire=llvm::IntegerType::get(context,unsigned(bytes*8));
            else {result.indirect=true;result.wire=ptr();}
            return result;
        };
        returnPlan=classify(returnType,declaration->abiResult);
        for(size_t n=0;n<sourceTypes.size();++n)plans.push_back(classify(sourceTypes[n],nativeTypes[n]));
#else
        classifySysv(*declaration,sourceTypes,nativeTypes,returnPlan,plans);
#endif
        std::vector<llvm::Value*> values;
        std::vector<llvm::Type*> parameters;
        std::vector<std::pair<unsigned,llvm::Attribute>> attributes;
        llvm::Value* returnSlot=nullptr;
        if(returnPlan.indirect) {
            auto* memory=cMemoryType(returnType,declaration->abiResult);returnSlot=cTemporary(memory);
            values.push_back(returnSlot);parameters.push_back(ptr());
            attributes.emplace_back(0,llvm::Attribute::getWithStructRetType(context,memory));
            attributes.emplace_back(0,llvm::Attribute::get(context,llvm::Attribute::NoAlias));
            attributes.emplace_back(0,llvm::Attribute::getWithAlignment(context,returnPlan.alignment));
        }
        for(size_t n=0;n<sourceTypes.size();++n) {
            const auto& plan=plans[n];const auto& source=sourceTypes[n];
            bool fixed=n<declaration->parameters.size();
            if(plan.ignore)continue;
            if(source.kind!=TypeKind::Tuple) {
                auto* value=load(ins.args[n]);
                if(source.kind==TypeKind::I64)value=cIntegerArgument(value,nativeTypes[n]);
                if(!fixed && source.kind==TypeKind::Bool) value=builder.CreateZExt(value,i32());
                if(fixed && plan.zeroExtend)
                    attributes.emplace_back(unsigned(values.size()),llvm::Attribute::get(context,llvm::Attribute::ZExt));
                if(fixed && plan.signExtend)
                    attributes.emplace_back(unsigned(values.size()),llvm::Attribute::get(context,llvm::Attribute::SExt));
                values.push_back(value);if(fixed)parameters.push_back(value->getType());
                continue;
            }
            auto* slot=packCRecord(ins.args[n],nativeTypes[n],plan.indirect?nullptr:plan.wire,plan.offset);
            if(plan.indirect) {
                unsigned index=unsigned(values.size());
                values.push_back(slot);if(fixed)parameters.push_back(ptr());
                if(plan.byval)attributes.emplace_back(index,llvm::Attribute::getWithByValType(context,cMemoryType(source,nativeTypes[n])));
                attributes.emplace_back(index,llvm::Attribute::getWithAlignment(context,plan.alignment));
            } else {
                auto* address=builder.CreateGEP(llvm::Type::getInt8Ty(context),slot,n64(plan.offset));
                auto* value=builder.CreateAlignedLoad(plan.wire,address,llvm::Align(1));
                if(plan.flatten) {
                    auto* record=llvm::cast<llvm::StructType>(plan.wire);
                    for(unsigned field=0;field<record->getNumElements();++field) {
                        auto* part=builder.CreateExtractValue(value,field);
                        values.push_back(part);if(fixed)parameters.push_back(part->getType());
                    }
                } else {values.push_back(value);if(fixed)parameters.push_back(value->getType());}
            }
        }
        auto* resultWire=returnPlan.indirect || returnPlan.ignore?llvm::Type::getVoidTy(context):returnPlan.wire;
        const std::string& symbol=declaration->symbol.empty()?declaration->name:declaration->symbol;
        auto* functionType=llvm::FunctionType::get(resultWire,parameters,declaration->variadic);
        if(auto* previous=module.getFunction(symbol))
            if(previous->getFunctionType()!=functionType)
                throw std::runtime_error("conflicting C ABI declarations for symbol: "+symbol);
        auto callee=module.getOrInsertFunction(symbol,functionType);
        auto* call=builder.CreateCall(callee,values);
        auto* declared=llvm::dyn_cast<llvm::Function>(callee.getCallee());
        for(const auto& attribute:attributes) {
            call->addParamAttr(attribute.first,attribute.second);
            if(declared && attribute.first<declared->arg_size())declared->addParamAttr(attribute.first,attribute.second);
        }
        if(returnPlan.zeroExtend) {
            call->addRetAttr(llvm::Attribute::ZExt);if(declared)declared->addRetAttr(llvm::Attribute::ZExt);
        }
        if(returnPlan.signExtend) {
            call->addRetAttr(llvm::Attribute::SExt);if(declared)declared->addRetAttr(llvm::Attribute::SExt);
        }
        if(returnType.kind!=TypeKind::Tuple) {
            if(returnType.kind==TypeKind::Unit)return nullptr;
            return returnType.kind==TypeKind::I64?cIntegerResult(call,declaration->abiResult):call;
        }
        auto* memory=llvm::cast<llvm::StructType>(cMemoryType(returnType,declaration->abiResult));
        if(!returnSlot) {
            returnSlot=cTemporary(memory,returnPlan.wire,returnPlan.offset);
            auto* address=builder.CreateGEP(llvm::Type::getInt8Ty(context),returnSlot,n64(returnPlan.offset));
            builder.CreateAlignedStore(call,address,llvm::Align(1));
        }
        return unpackCRecord(returnType,declaration->abiResult,returnSlot);
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
        case Op::CCallback: result=cCallback(ins.text);break;
        case Op::Spawn: {
            const auto& returnType=argType(0).args.back();
            result=runtime("pf_task_spawn",ptr(),{ptr(),ptr(),ptr(),i32()},
                {arg(0),taskEntry(returnType),null(),tag(returnType)});break;
        }
        case Op::Join:
            result=fromBits(runtime("pf_task_join",i64(),{ptr()},{arg(0)}),argType(0).args.at(0));break;
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
            runtime("pf_tasks_leave",voidTy,{ptr()},{taskScope});
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
        case Op::RawOffset:
            result=runtime("pf_raw_offset",ptr(),{ptr(),i64(),i32()},
                {arg(0),arg(1),tag(argType(0).args.at(0))});break;
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
            result=external(ins);break;
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
        debugParameters.clear();
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
                debugParameters.emplace(name,variable);
                debugBuilder->insertDeclare(registers[n],variable,debugBuilder->createExpression(),
                    llvm::DILocation::get(context,source.location.line,1,debugScope),entry);
            }
        }
        sourceLocation(source.location);
        rootFrame=runtime("pf_roots_push",ptr(),{ptr(),i64()},{rootArray,n64(int64_t(count))});
        taskScope=runtime("pf_tasks_enter",ptr(),{},{});
        for(size_t n=0;n<source.code.size();++n)
            blocks.push_back(llvm::BasicBlock::Create(context,"op"+std::to_string(n),fn));
        if(blocks.empty()) throw std::runtime_error("IR function has no instructions: "+source.name);
        builder.CreateBr(blocks.front());
        for(size_t n=0;n<source.code.size();++n) {
            builder.SetInsertPoint(blocks[n]);
            if(debugBuilder) builder.SetCurrentDebugLocation(llvm::DILocation::get(context,
                source.code[n].location.line,source.code[n].location.column,debugScope));
            sourceLocation(source.code[n].location);
            lower(source.code[n]);
            debugLocal(source.code[n]);
            if(!blocks[n]->hasTerminator()) {
                if(n+1<blocks.size()) builder.CreateBr(blocks[n+1]);
                else {
                    runtime("pf_tasks_leave",llvm::Type::getVoidTy(context),{ptr()},{taskScope});
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
        : program(p),context(c),module(m),builder(m) {
        if(debug) {
            debugBuilder=std::make_unique<llvm::DIBuilder>(module);
            auto path=std::filesystem::u8path(program.sourceFile);
            debugFile=debugBuilder->createFile(path.filename().u8string(),path.parent_path().u8string());
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
    Program optimized;
    const Program* lowering = &program;
    if(optimization) { optimized = optimizeIR(program); lowering = &optimized; }
    else verify(program);
    Generated g;
    g.context=std::make_unique<llvm::LLVMContext>();
    g.module=std::make_unique<llvm::Module>("PikaFlow",*g.context);
    g.target=makeTarget(optimization);
    g.module->setTargetTriple(g.target->getTargetTriple());
    g.module->setDataLayout(g.target->createDataLayout());
    Emitter(*lowering,*g.context,*g.module,debug).emit();
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
        custom.addPass(llvm::createModuleToFunctionPassAdaptor(CheckedArithmeticPass()));
        custom.run(*g.module,modules);
    }
    if(llvm::verifyModule(*g.module,&errors)) throw std::runtime_error("invalid optimized LLVM IR:\n"+errors.str());
    return g;
}
}

std::string targetTriple() { initializeLLVM(); return llvm::sys::getDefaultTargetTriple(); }
void loadLibrary(const std::string& path) {
    std::string error;
    if(llvm::sys::DynamicLibrary::LoadLibraryPermanently(path.c_str(),&error))
        throw std::runtime_error("cannot load library '"+path+"': "+error);
}
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
    PF_JIT_SYMBOL(pf_cell_address); PF_JIT_SYMBOL(pf_source_location);
    PF_JIT_SYMBOL(pf_tasks_enter); PF_JIT_SYMBOL(pf_tasks_leave);
    PF_JIT_SYMBOL(pf_task_spawn); PF_JIT_SYMBOL(pf_task_join);
    PF_JIT_SYMBOL(pf_raw_offset);
    PF_JIT_SYMBOL(pf_c_integer);
#define PF_C_SYMBOL(name, address) symbols[mangle(name)] = llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr::fromPtr(address),llvm::JITSymbolFlags::Exported)
    PF_C_SYMBOL("strlen",&std::strlen); PF_C_SYMBOL("malloc",&std::malloc); PF_C_SYMBOL("free",&std::free);
    PF_C_SYMBOL("memcpy",&std::memcpy); PF_C_SYMBOL("memmove",&std::memmove); PF_C_SYMBOL("memset",&std::memset);
    PF_C_SYMBOL("llabs",&std::llabs);
    PF_C_SYMBOL("abs",static_cast<int(*)(int)>(&std::abs));
    PF_C_SYMBOL("sqrt",static_cast<double(*)(double)>(&std::sqrt));
    PF_C_SYMBOL("sin",static_cast<double(*)(double)>(&std::sin));
    PF_C_SYMBOL("cos",static_cast<double(*)(double)>(&std::cos));
    PF_C_SYMBOL("tan",static_cast<double(*)(double)>(&std::tan));
    PF_C_SYMBOL("exp",static_cast<double(*)(double)>(&std::exp));
    PF_C_SYMBOL("log",static_cast<double(*)(double)>(&std::log));
    PF_C_SYMBOL("floor",static_cast<double(*)(double)>(&std::floor));
    PF_C_SYMBOL("ceil",static_cast<double(*)(double)>(&std::ceil));
    PF_C_SYMBOL("fabs",static_cast<double(*)(double)>(&std::fabs));
    PF_C_SYMBOL("pow",static_cast<double(*)(double,double)>(&std::pow));
    PF_C_SYMBOL("fmod",static_cast<double(*)(double,double)>(&std::fmod));
#undef PF_C_SYMBOL
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
std::string nativeDebugDatabasePath(const std::string& output) {
#ifdef _MSC_VER
    return output+".pdb";
#else
    return {};
#endif
}
void buildExecutable(const Program& program,const std::string& output,const std::string& runtimeLibrary,
    const std::string& compilerDriver,unsigned optimization,bool debug,const std::vector<std::string>& extraLibraries) {
    auto final=std::filesystem::u8path(output);
    if(!final.parent_path().empty()) std::filesystem::create_directories(final.parent_path());
    llvm::SmallString<128> temporary;
    if(auto error=llvm::sys::fs::createTemporaryFile("pikaflow","obj",temporary))
        throw std::runtime_error("cannot create temporary object: "+error.message());
    llvm::FileRemover cleanup(temporary);
    std::string object=temporary.str().str();
    emitObject(program,object,optimization,debug);
    struct LinkArtifacts {
        llvm::SmallString<128> directory;
        LinkArtifacts() {
            if(auto error=llvm::sys::fs::createUniqueDirectory("pikaflow-link",directory))
                throw std::runtime_error("cannot create temporary link directory: "+error.message());
        }
        ~LinkArtifacts() { llvm::sys::fs::remove_directories(directory); }
        std::string importLibrary() const {
            llvm::SmallString<128> path(directory);
            llvm::sys::path::append(path,"exports.lib");return path.str().str();
        }
    };
    std::unique_ptr<LinkArtifacts> artifacts;
    std::vector<std::string> arguments;
    std::string driverName=std::filesystem::u8path(compilerDriver).filename().u8string();
    std::transform(driverName.begin(),driverName.end(),driverName.begin(),[](unsigned char c){return char(std::tolower(c));});
    if(driverName=="cl.exe" || driverName=="cl") {
        artifacts=std::make_unique<LinkArtifacts>();
        arguments={compilerDriver,"/nologo",object,runtimeLibrary,"/Fe:"+output};
        arguments.push_back("/link");
        // cl otherwise appends .exe and LINK can overwrite adjacent .ilk/.lib/.exp files.
        arguments.push_back("/OUT:"+output);
        arguments.push_back("/INCREMENTAL:NO");
        arguments.push_back("/IMPLIB:"+artifacts->importLibrary());
        if(debug) {
            arguments.push_back("/DEBUG");
            auto database=nativeDebugDatabasePath(output);
            arguments.push_back("/PDB:"+(database.empty()?output+".pdb":database));
        }
        std::istringstream directories(PF_MSVC_LIBRARY_DIRS);
        for(std::string directory;std::getline(directories,directory,';');) {
            if(directory.size()>1 && directory.front()=='\"' && directory.back()=='\"')
                directory=directory.substr(1,directory.size()-2);
            if(!directory.empty())arguments.push_back("/LIBPATH:"+directory);
        }
    } else {
        arguments={compilerDriver,object,runtimeLibrary,"-o",output};
        if(debug) arguments.push_back("-g");
#ifndef _WIN32
        arguments.push_back("-lm"); arguments.push_back("-ldl"); arguments.push_back("-pthread");
#if PF_SANITIZERS_ENABLED
        arguments.push_back("-fsanitize=address,undefined");
#endif
#endif
    }
    arguments.insert(arguments.end(),extraLibraries.begin(),extraLibraries.end());
    std::vector<llvm::StringRef> refs;
    for(const auto& argument:arguments) refs.emplace_back(argument);
    auto driver=llvm::sys::findProgramByName(compilerDriver);
    if(!driver) throw std::runtime_error("cannot find native link driver: "+compilerDriver);
    std::string message;
    int result=llvm::sys::ExecuteAndWait(*driver,refs,std::nullopt,{},0,0,&message);
    if(result!=0) throw std::runtime_error("native link failed ("+std::to_string(result)+"): "+message);
}
}
