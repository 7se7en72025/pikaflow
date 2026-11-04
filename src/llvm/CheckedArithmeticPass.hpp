#pragma once
#include "llvm/IR/PassManager.h"

namespace llvm { class Function; }

namespace pika {
// Emit the v2 checked-i64 contract; v1 callers retain identity-only compatibility.
void markCheckedArithmeticRuntime(llvm::Function& function);

class CheckedArithmeticPass : public llvm::OptionalPassInfoMixin<CheckedArithmeticPass> {
public:
    llvm::PreservedAnalyses run(llvm::Function& function, llvm::FunctionAnalysisManager& analyses);
};
} // namespace pika
