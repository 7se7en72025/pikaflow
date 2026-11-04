#pragma once
#include "pikaflow/ir.hpp"

namespace pika {
std::string emitLLVM(const Program& program, unsigned optimization = 0, bool debug = false);
void emitObject(const Program& program, const std::string& output, unsigned optimization = 0, bool debug = false);
int runJIT(const Program& program, unsigned optimization = 0);
void buildExecutable(const Program& program, const std::string& output,
    const std::string& runtimeLibrary, const std::string& compilerDriver,
    unsigned optimization = 0, bool debug = false);
std::string targetTriple();
}
