#include "CheckedArithmeticPass.hpp"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

namespace pika {
namespace {
constexpr llvm::StringLiteral contract = "pikaflow.checked-arithmetic";
constexpr llvm::StringLiteral version = "v1";
enum class Operation { None, Add, Sub, Mul, Div, Mod };

Operation operation(llvm::StringRef name) {
    if(name == "pf_checked_add") return Operation::Add;
    if(name == "pf_checked_sub") return Operation::Sub;
    if(name == "pf_checked_mul") return Operation::Mul;
    if(name == "pf_checked_div") return Operation::Div;
    if(name == "pf_checked_mod") return Operation::Mod;
    return Operation::None;
}
bool checkedSignature(const llvm::Function& function) {
    const auto* type = function.getFunctionType();
    return function.getCallingConv() == llvm::CallingConv::C && !type->isVarArg() &&
        type->getNumParams() == 2 && type->getReturnType()->isIntegerTy(64) &&
        type->getParamType(0)->isIntegerTy(64) && type->getParamType(1)->isIntegerTy(64);
}
} // namespace

void markCheckedArithmeticRuntime(llvm::Function& function) {
    if(operation(function.getName()) != Operation::None && checkedSignature(function))
        function.addFnAttr(contract, version);
}

llvm::PreservedAnalyses CheckedArithmeticPass::run(llvm::Function& function,
                                                 llvm::FunctionAnalysisManager&) {
    bool changed = false;
    for(auto& block : function) {
        for(auto iterator = block.begin(); iterator != block.end();) {
            auto* call = llvm::dyn_cast<llvm::CallInst>(&*iterator++);
            if(!call || call->arg_size() != 2 || call->hasOperandBundles() || call->isMustTailCall() ||
                call->getCallingConv() != llvm::CallingConv::C) continue;
            const auto* callee = call->getCalledFunction();
            if(!callee || !checkedSignature(*callee) || call->getFunctionType() != callee->getFunctionType() ||
                !callee->hasFnAttribute(contract) ||
                callee->getFnAttribute(contract).getValueAsString() != version) continue;
            const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1));
            if(!constant) continue;
            const auto op = operation(callee->getName());
            llvm::Value* replacement = nullptr;
            if((constant->isZero() && (op == Operation::Add || op == Operation::Sub)) ||
               (constant->isOne() && (op == Operation::Mul || op == Operation::Div)))
                replacement = call->getArgOperand(0);
            else if((constant->isZero() && op == Operation::Mul) ||
                    (constant->isOne() && op == Operation::Mod))
                replacement = llvm::ConstantInt::get(call->getType(), 0);
            if(replacement) {
                call->replaceAllUsesWith(replacement);
                call->eraseFromParent(); changed = true;
            }
        }
    }
    return changed ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
}
} // namespace pika
