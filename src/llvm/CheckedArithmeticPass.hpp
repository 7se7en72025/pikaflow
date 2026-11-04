#pragma once
#include "llvm/IR/PassManager.h"

namespace llvm { class Function; }

namespace pika {
// Opt-in contract: the named checked helpers have no extra effects for identities.
void markCheckedArithmeticRuntime(llvm::Function& function);

class CheckedArithmeticPass : public llvm::OptionalPassInfoMixin<CheckedArithmeticPass> {
public:
    llvm::PreservedAnalyses run(llvm::Function& function, llvm::FunctionAnalysisManager& analyses);
};
} // namespace pika
