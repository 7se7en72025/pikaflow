#include "CheckedArithmeticPass.hpp"
#include "llvm/Config/llvm-config.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/Compiler.h"

#ifndef PF_PLUGIN_VERSION
#define PF_PLUGIN_VERSION LLVM_VERSION_STRING
#endif

extern "C" LLVM_ATTRIBUTE_WEAK LLVM_EXTERNAL_VISIBILITY
llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION, "PikaFlowCheckedSimplify", PF_PLUGIN_VERSION,
        [](llvm::PassBuilder& builder) {
            builder.registerPipelineParsingCallback(
                [](llvm::StringRef name, llvm::FunctionPassManager& passes,
                   llvm::ArrayRef<llvm::PassBuilder::PipelineElement> inner) {
                    if(name != "pikaflow-checked-simplify" || !inner.empty()) return false;
                    passes.addPass(pika::CheckedArithmeticPass());
                    return true;
                });
        }, nullptr, nullptr};
}
