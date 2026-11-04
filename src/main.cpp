#include "pikaflow/frontend.hpp"
#include "pikaflow/ir.hpp"
#ifdef PF_HAS_LLVM
#include "pikaflow/backend.hpp"
#endif
#include <algorithm>
#include <cctype>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace {
bool sameFile(const fs::path &a, const fs::path &b) {
    std::error_code error;
    if (fs::equivalent(a, b, error) && !error)
        return true;
    return fs::weakly_canonical(fs::absolute(a)) == fs::weakly_canonical(fs::absolute(b));
}
std::string read(const fs::path &file) {
    std::ifstream input(file, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open source file: " + file.u8string());
    input.seekg(0, std::ios::end);
    auto size = input.tellg();
    if (size < 0 || size > 16 * 1024 * 1024)
        throw std::runtime_error("source file exceeds 16 MiB limit");
    input.seekg(0);
    std::ostringstream content;
    content << input.rdbuf();
    if (input.bad())
        throw std::runtime_error("cannot read source file: " + file.u8string());
    return content.str();
}
void write(const fs::path &file, const std::string &content) {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("cannot write output file: " + file.u8string());
    output << content;
    if (!output)
        throw std::runtime_error("failed writing output file: " + file.u8string());
}
fs::path executablePath(const char *executable) {
    fs::path executablePath = fs::absolute(fs::u8path(executable));
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, &buffer[0], static_cast<DWORD>(buffer.size()));
    if (size && size < buffer.size()) {
        buffer.resize(size);
        executablePath = buffer;
    }
#else
    std::error_code error;
    auto resolved = fs::read_symlink("/proc/self/exe", error);
    if (!error)
        executablePath = resolved;
#endif
    return executablePath;
}
fs::path standardLibrary(const char *executable) {
    auto installed =
        executablePath(executable).parent_path().parent_path() / "share" / "pikaflow" / "stdlib";
    if (fs::is_directory(installed))
        return installed;
    return fs::u8path(PF_STDLIB_DIR);
}
#ifdef PF_HAS_LLVM
std::string runtimeLibrary(const char *executable) {
#ifdef _WIN32
    const char *libraryName = "pikaflow_runtime.lib";
#else
    const char *libraryName = "libpikaflow_runtime.a";
#endif
    auto installed = executablePath(executable).parent_path().parent_path() / "lib" / libraryName;
    if (fs::exists(installed))
        return installed.u8string();
    if (fs::exists(fs::u8path(PF_RUNTIME_LIBRARY)))
        return PF_RUNTIME_LIBRARY;
    throw std::runtime_error(
        "PikaFlow runtime library not found; install the compiler and runtime together");
}
#endif
class Modules {
    std::set<std::string> active, loaded;
    std::vector<pika::StmtPtr> statements;
    fs::path standard;
    fs::path packageRoot;
    fs::path packageImport(const std::string &name, const pika::Location &location) const {
        auto slash = name.find('/');
        std::string alias = name.substr(0, slash);
        if (alias.empty() || (!std::isalpha(static_cast<unsigned char>(alias[0])) && alias[0] != '_') ||
            !std::all_of(alias.begin(), alias.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '_';
            }))
            throw pika::Error(location, "invalid package dependency name");
        if (packageRoot.empty())
            throw pika::Error(location, "pkg: imports require pika.json and pika.lock; run deps lock");
        std::istringstream lock(read(packageRoot / "pika.lock"));
        std::string line;
        std::getline(lock, line);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line != "pikaflow-lock-v1")
            throw pika::Error(location, "unsupported package lock format");
        fs::path dependency;
        while (std::getline(lock, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto first = line.find('\t'), second = line.find('\t', first == std::string::npos ? 0 : first + 1);
            if (first == std::string::npos || second == std::string::npos || line.size() - second - 1 != 64 ||
                !std::all_of(line.begin() + second + 1, line.end(), [](char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                }) || line.find('\0') != std::string::npos)
                throw pika::Error(location, "invalid package lock entry");
            if (line.substr(0, first) == alias) {
                if (!dependency.empty()) throw pika::Error(location, "duplicate dependency in package lock");
                dependency = packageRoot / fs::u8path(line.substr(first + 1, second - first - 1));
            }
        }
        if (dependency.empty())
            throw pika::Error(location, "package dependency is not locked: " + alias);
        dependency = fs::weakly_canonical(fs::absolute(dependency));
        fs::path module = slash == std::string::npos ? fs::path("lib.pf") : fs::u8path(name.substr(slash + 1));
        if (module.empty() || module.is_absolute() || module.has_root_name())
            throw pika::Error(location, "package module must be a relative path");
        for (const auto &part : module)
            if (part == "..") throw pika::Error(location, "package module cannot escape its package");
        if (module.extension().empty()) module += ".pf";
        auto resolved = fs::weakly_canonical(dependency / module);
        auto a = dependency.begin(), b = resolved.begin();
        for (; a != dependency.end() && b != resolved.end() && *a == *b; ++a, ++b) {}
        if (a != dependency.end())
            throw pika::Error(location, "package module cannot escape its package");
        return resolved;
    }
    void load(const fs::path &requested) {
        std::string path = fs::weakly_canonical(fs::absolute(requested)).u8string();
        if (active.count(path))
            throw pika::Error({path, 1, 1}, "cyclic module import");
        if (loaded.count(path))
            return;
        active.insert(path);
        auto source = read(fs::u8path(path));
        auto tree = pika::parse(pika::lex(source, path), path);
        for (auto &statement : tree.statements)
            if (statement->kind == pika::Stmt::Kind::Import) {
                if (statement->other)
                    throw pika::Error(
                        statement->location,
                        "named import aliases are not implemented; import exported names directly");
                fs::path imported;
                if (statement->name.rfind("std:", 0) == 0) {
                    std::string module = statement->name.substr(4);
                    if (module.empty())
                        throw pika::Error(statement->location,
                                          "std: import requires a module name");
                    for (unsigned char c : module)
                        if (!std::isalnum(c) && c != '_')
                            throw pika::Error(
                                statement->location,
                                "std: module name must contain letters, digits or underscores");
                    imported = standard / fs::u8path(module + ".pf");
                } else if (statement->name.rfind("pkg:", 0) == 0)
                    imported = packageImport(statement->name.substr(4), statement->location);
                else
                    imported = fs::u8path(path).parent_path() / fs::u8path(statement->name);
                imported = fs::weakly_canonical(imported);
                statement->name = imported.u8string();
                load(imported);
            }
        statements.insert(statements.end(), tree.statements.begin(), tree.statements.end());
        active.erase(path);
        loaded.insert(path);
    }

  public:
    explicit Modules(fs::path directory) : standard(std::move(directory)) {}
    void protect(const fs::path &output) const {
        for (const auto &source : loaded)
            if (sameFile(output, fs::u8path(source)))
                throw std::runtime_error("output path would overwrite a source file");
    }
    pika::SyntaxTree program(const fs::path &file) {
        std::string path = fs::weakly_canonical(fs::absolute(file)).u8string();
        for (auto directory = fs::u8path(path).parent_path(); !directory.empty();) {
            if (fs::is_regular_file(directory / "pika.json")) {
                packageRoot = directory;
                break;
            }
            auto parent = directory.parent_path();
            if (parent == directory) break;
            directory = parent;
        }
        load(file);
        return {std::move(statements), path};
    }
};
void diagnostic(const pika::Error &error) {
    std::cerr << error.location.file << ":" << error.location.line << ":" << error.location.column
              << ": error: " << error.what() << "\n";
    try {
        std::istringstream input(read(fs::u8path(error.location.file)));
        std::string line;
        for (unsigned n = 1; n <= error.location.line && std::getline(input, line); ++n)
            if (n == error.location.line) {
                std::cerr << "  " << line << "\n  "
                          << std::string(error.location.column > 0 ? error.location.column - 1 : 0,
                                         ' ')
                          << "^\n";
            }
    } catch (const std::exception &) {
    }
}
void help() {
    std::cout << "PikaFlow - typed structured and stack programming\n"
              << "usage: pikaflow COMMAND FILE [-O0|-O1|-O2|-O3] [--debug] [-o OUTPUT]\n"
              << "  check         parse, type-check and verify typed IR\n"
              << "                --library checks modules without a source main\n"
              << "  interpret     execute the reference interpreter\n"
              << "  run           execute through LLVM ORC JIT\n"
              << "  emit-ir       print/write verified LLVM IR\n"
              << "  emit-obj      emit a native object file (-o required)\n"
              << "  build         compile and link a native executable\n"
              << "  dump-tokens   inspect lexer output\n"
              << "  dump-ast      inspect syntax tree\n"
              << "  dump-ir       inspect custom typed IR\n"
              << "  dump-asm      inspect target machine assembly\n"
              << "  --version     show compiler/backend version\n";
}
} // namespace
int runCLI(int argc, char **argv) {
    try {
        if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
            help();
            return 0;
        }
        std::string command = argv[1];
        if (command == "--version") {
            std::cout << "PikaFlow 0.2.0; "
#ifdef PF_HAS_LLVM
                      << "LLVM enabled; " << pika::targetTriple()
#else
                      << "LLVM disabled; reference interpreter only"
#endif
                      << "\n";
            return 0;
        }
        if (argc < 3)
            throw std::runtime_error("missing source file; use --help");
        fs::path source = fs::u8path(argv[2]);
        std::string output;
        [[maybe_unused]] unsigned optimization = 0;
        [[maybe_unused]] bool debug = false;
        bool libraryMode = false;
        std::vector<std::string> links, loads;
        for (int i = 3; i < argc; ++i) {
            std::string option = argv[i];
            if (option == "-o") {
                if (++i >= argc)
                    throw std::runtime_error("-o requires output path");
                output = argv[i];
            } else if (option == "--link" || option == "--load") {
                if (++i >= argc)
                    throw std::runtime_error(option + " requires a library path");
                (option == "--link" ? links : loads)
                    .push_back(fs::absolute(fs::u8path(argv[i])).u8string());
            } else if (option == "--library")
                libraryMode = true;
            else if (option == "--debug" || option == "-g")
                debug = true;
            else if (option.size() == 3 && option[0] == '-' && option[1] == 'O' &&
                     option[2] >= '0' && option[2] <= '3')
                optimization = static_cast<unsigned>(option[2] - '0');
            else
                throw std::runtime_error("unknown option: " + option);
        }
        if (!links.empty() && command != "build")
            throw std::runtime_error("--link is supported by build only");
        if (!loads.empty() && command != "run")
            throw std::runtime_error("--load is supported by JIT run only");
        if (libraryMode && command != "check")
            throw std::runtime_error("--library is supported by check only");
        if (!output.empty() && sameFile(fs::u8path(output), source))
            throw std::runtime_error("output path would overwrite the source file");
        std::string text;
        if (command == "dump-tokens") {
            text = pika::dumpTokens(
                pika::lex(read(source), fs::weakly_canonical(fs::absolute(source)).u8string()));
        } else {
            Modules modules(standardLibrary(argv[0]));
            auto tree = modules.program(source);
            if (libraryMode && std::none_of(tree.statements.begin(), tree.statements.end(),
                    [&](const pika::StmtPtr &statement) {
                        return statement->name == "main" && statement->location.file == tree.filename;
                    })) {
                auto entry = std::make_shared<pika::Stmt>(pika::Stmt::Kind::Function,
                                                          pika::Location{tree.filename, 1, 1});
                entry->name = "main";
                entry->returnType = pika::Unit;
                entry->body.push_back(std::make_shared<pika::Stmt>(pika::Stmt::Kind::Return,
                                                                  entry->location));
                tree.statements.push_back(std::move(entry));
            }
            if (!output.empty())
                modules.protect(fs::u8path(output));
            if (command == "dump-ast")
                text = pika::dumpAST(tree);
            else {
                auto program = pika::compile(tree);
                if (command == "check") {
                    std::cout << "OK: " << source.u8string() << "\n";
                    return 0;
                }
                if (command == "dump-ir")
                    text = pika::dumpIR(program);
                else if (command == "interpret")
                    return pika::interpret(program);
#ifdef PF_HAS_LLVM
                else if (command == "run") {
                    for (const auto &library : loads)
                        pika::loadLibrary(library);
                    return pika::runJIT(program, optimization);
                } else if (command == "emit-ir")
                    text = pika::emitLLVM(program, optimization, debug);
                else if (command == "dump-asm")
                    text = pika::emitAssembly(program, optimization, debug);
                else if (command == "emit-obj") {
                    if (output.empty())
                        throw std::runtime_error("emit-obj requires -o OUTPUT");
                    pika::emitObject(program, output, optimization, debug);
                    return 0;
                } else if (command == "build") {
                    if (output.empty()) {
                        output = source.stem().u8string();
#ifdef _WIN32
                        output += ".exe";
#endif
                    }
                    modules.protect(fs::u8path(output));
                    pika::buildExecutable(program, output, runtimeLibrary(argv[0]),
                                          PF_COMPILER_DRIVER, optimization, debug, links);
                    std::cout << "Built " << output << "\n";
                    return 0;
                }
#else
                else if (command == "run" || command == "emit-ir" || command == "emit-obj" ||
                         command == "dump-asm" || command == "build")
                    throw std::runtime_error(
                        "LLVM backend is disabled; configure with PIKAFLOW_ENABLE_LLVM=ON");
#endif
                else
                    throw std::runtime_error("unknown command: " + command);
            }
        }
        if (output.empty())
            std::cout << text;
        else
            write(fs::u8path(output), text);
        return 0;
    } catch (const pika::Error &error) {
        diagnostic(error);
        return 1;
    } catch (const std::exception &error) {
        std::cerr << "pikaflow: " << error.what() << "\n";
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    std::vector<std::string> storage;
    storage.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        int length = static_cast<int>(std::wcslen(argv[i]));
        int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], length, nullptr, 0,
                                       nullptr, nullptr);
        if (length && !size) {
            std::cerr << "pikaflow: invalid Unicode command-line argument\n";
            return 1;
        }
        std::string argument(static_cast<size_t>(size), '\0');
        if (size)
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], length, &argument[0], size,
                                nullptr, nullptr);
        storage.push_back(std::move(argument));
    }
    std::vector<char *> arguments;
    for (auto &argument : storage)
        arguments.push_back(&argument[0]);
    arguments.push_back(nullptr);
    return runCLI(argc, arguments.data());
}
#else
int main(int argc, char **argv) {
    return runCLI(argc, argv);
}
#endif
