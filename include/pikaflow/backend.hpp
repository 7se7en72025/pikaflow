#pragma once
#include "pikaflow/ir.hpp"

namespace pika {
std::string emitLLVM(const Program& program, unsigned optimization = 0, bool debug = false);
std::string emitAssembly(const Program& program, unsigned optimization = 0, bool debug = false);
void emitObject(const Program& program, const std::string& output, unsigned optimization = 0, bool debug = false);
int runJIT(const Program& program, unsigned optimization = 0);
void loadLibrary(const std::string& path);
// Persistent MSVC debug sidecar; empty on hosts that do not emit PDBs.
std::string nativeDebugDatabasePath(const std::string& output);
void buildExecutable(const Program& program, const std::string& output,
    const std::string& runtimeLibrary, const std::string& compilerDriver,
    unsigned optimization = 0, bool debug = false,
    const std::vector<std::string>& extraLibraries = {});
std::string targetTriple();
}
