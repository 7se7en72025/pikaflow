#include "CheckedArithmeticPass.hpp"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

namespace pika {
namespace {
constexpr llvm::StringLiteral contract = "pikaflow.checked-arithmetic";
constexpr llvm::StringLiteral identityVersion = "v1";
constexpr llvm::StringLiteral checkedVersion = "v2";
constexpr char remarkPass[] = "pikaflow-checked-simplify";
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

llvm::ConstantInt* foldConstants(Operation op, const llvm::ConstantInt& left,
                                 const llvm::ConstantInt& right, llvm::StringRef& failure) {
    const auto& a = left.getValue();
    const auto& b = right.getValue();
    bool overflow = false;
    llvm::APInt value(64, 0);
    switch(op) {
    case Operation::Add: value = a.sadd_ov(b, overflow); break;
    case Operation::Sub: value = a.ssub_ov(b, overflow); break;
    case Operation::Mul: value = a.smul_ov(b, overflow); break;
    case Operation::Div:
        if(b.isZero()) { failure = "zero divisor"; return nullptr; }
        value = a.sdiv_ov(b, overflow);
        break;
    case Operation::Mod:
        // APInt's remainder alone does not diagnose PikaFlow's MIN % -1 failure.
        if(b.isZero()) { failure = "zero divisor"; return nullptr; }
        if(a.isMinSignedValue() && b.isAllOnes()) { failure = "signed overflow"; return nullptr; }
        value = a.srem(b);
        break;
    case Operation::None: return nullptr;
    }
    if(overflow) { failure = "signed overflow"; return nullptr; }
    return llvm::ConstantInt::get(left.getContext(), value);
}
} // namespace

void markCheckedArithmeticRuntime(llvm::Function& function) {
    if(operation(function.getName()) != Operation::None && checkedSignature(function))
        function.addFnAttr(contract, checkedVersion);
}

llvm::PreservedAnalyses CheckedArithmeticPass::run(llvm::Function& function,
                                                 llvm::FunctionAnalysisManager& analyses) {
    auto& remarks = analyses.getResult<llvm::OptimizationRemarkEmitterAnalysis>(function);
    bool changed = false;
    for(auto& block : function) {
        for(auto iterator = block.begin(); iterator != block.end();) {
            auto* call = llvm::dyn_cast<llvm::CallInst>(&*iterator++);
            if(!call || call->arg_size() != 2 || call->hasOperandBundles() || call->isMustTailCall() ||
                call->getCallingConv() != llvm::CallingConv::C) continue;
            const auto* callee = call->getCalledFunction();
            if(!callee || !checkedSignature(*callee) || call->getFunctionType() != callee->getFunctionType() ||
                !callee->hasFnAttribute(contract)) continue;
            const auto version = callee->getFnAttribute(contract).getValueAsString();
            if(version != identityVersion && version != checkedVersion) continue;
            const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1));
            if(!constant || llvm::isa<llvm::UndefValue>(call->getArgOperand(0)) ||
                llvm::isa<llvm::PoisonValue>(call->getArgOperand(0))) continue;
            const auto op = operation(callee->getName());
            llvm::Value* replacement = nullptr;
            llvm::StringRef failure;
            if(version == checkedVersion) {
                if(const auto* left = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(0)))
                    replacement = foldConstants(op, *left, *constant, failure);
            }
            const bool foldedConstants = replacement != nullptr;
            if(!replacement && ((constant->isZero() && (op == Operation::Add || op == Operation::Sub)) ||
                (constant->isOne() && (op == Operation::Mul || op == Operation::Div))))
                replacement = call->getArgOperand(0);
            else if(!replacement && ((constant->isZero() && op == Operation::Mul) ||
                    (constant->isOne() && op == Operation::Mod)))
                replacement = llvm::ConstantInt::get(call->getType(), 0);
            if(replacement) {
                remarks.emit([&]() {
                    return llvm::OptimizationRemark(remarkPass,
                        foldedConstants ? "ConstantFold" : "IdentityFold", call)
                        << "folded " << llvm::ore::NV("Operation", callee->getName())
                        << " under " << llvm::ore::NV("Contract", version)
                        << " to " << llvm::ore::NV("Replacement", replacement);
                });
                call->replaceAllUsesWith(replacement);
                call->eraseFromParent(); changed = true;
            } else if(!failure.empty()) {
                remarks.emit([&]() {
                    return llvm::OptimizationRemarkMissed(remarkPass, "CheckedFailure", call)
                        << "retained " << llvm::ore::NV("Operation", callee->getName())
                        << " under " << llvm::ore::NV("Contract", version)
                        << ": " << llvm::ore::NV("Reason", failure);
                });
            }
        }
    }
    return changed ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
}
} // namespace pika
