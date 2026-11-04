#include "pikaflow/ir.hpp"
#include "pikaflow/runtime.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

using namespace pika;

namespace {
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(2); }
}
Instruction instruction(Op op, int destination = -1, std::vector<int> arguments = {}) {
    Instruction result{op};
    result.dst = destination;
    result.args = std::move(arguments);
    result.location = {"interpreter_tests", 1, 1};
    return result;
}
Instruction number(int destination, int64_t value) {
    auto result = instruction(Op::ConstI64, destination);
    result.integer = value;
    return result;
}
Instruction text(int destination, const std::string& value) {
    auto result = instruction(Op::ConstString, destination);
    result.text = value;
    return result;
}
Instruction named(Op op, int destination, std::vector<int> arguments, const std::string& name) {
    auto result = instruction(op, destination, std::move(arguments));
    result.text = name;
    return result;
}
Instruction jump(Op op, int target, std::vector<int> arguments = {}) {
    auto result = instruction(op, -1, std::move(arguments));
    result.target = target;
    return result;
}
Function function(const std::string& name, std::vector<Type> parameters, Type result, std::vector<Type> registers) {
    Function value;
    value.name = name;
    value.parameters = std::move(parameters);
    value.result = std::move(result);
    value.registers = std::move(registers);
    value.location = {"interpreter_tests", 1, 1};
    return value;
}
std::string execute(const Program& program) {
    std::ostringstream output;
    auto* previous = std::cout.rdbuf(output.rdbuf());
    try {
        const int status = interpret(program);
        std::cout.rdbuf(previous);
        require(status == 0, "unit entry returns successful process status");
    } catch (...) {
        std::cout.rdbuf(previous);
        throw;
    }
    pf_collect();
    require(pf_heap_objects() == 0, "interpreter call frames release GC roots");
    return output.str();
}

void testFunctionsAndBranches() {
    Program program;
    auto main = function("main", {}, Unit, {I64, I64, I64});
    main.code = {number(0, 20), number(1, 22), named(Op::Call, 2, {0, 1}, "add"),
        instruction(Op::Print, -1, {2}), instruction(Op::Return)};
    auto add = function("add", {I64, I64}, I64, {I64, I64, I64});
    add.code = {instruction(Op::Add, 2, {0, 1}), instruction(Op::Return, -1, {2})};
    program.functions = {main, add};
    require(execute(program) == "42\n", "typed function calls");

    auto loop = function("main", {}, Unit, {I64, I64, I64, Bool, I64});
    loop.code = {number(0, 5), number(1, 0), number(2, 1), number(4, 0),
        instruction(Op::Gt, 3, {0, 4}), jump(Op::JumpFalse, 9, {3}),
        instruction(Op::Add, 1, {1, 0}), instruction(Op::Sub, 0, {0, 2}),
        jump(Op::Jump, 4), instruction(Op::Print, -1, {1}), instruction(Op::Return)};
    program.functions = {loop};
    require(execute(program) == "15\n", "backedges and mutable virtual registers");
}

void testClosuresAndGC() {
    Program program;
    const auto closureType = Type::function({I64}, I64);
    auto main = function("main", {}, Unit, {I64, Type::cell(I64), closureType, I64, I64, I64, I64});
    main.code = {number(0, 10), instruction(Op::MakeCell, 1, {0}),
        named(Op::MakeClosure, 2, {1}, "increment"), instruction(Op::Collect), number(3, 2),
        instruction(Op::CallIndirect, 4, {2, 3}), instruction(Op::Print, -1, {4}),
        instruction(Op::Collect), number(5, 3), instruction(Op::CallIndirect, 6, {2, 5}),
        instruction(Op::Print, -1, {6}), instruction(Op::Return)};
    auto increment = function("increment", {I64}, I64, {I64, Type::cell(I64), I64, I64});
    auto capture = instruction(Op::Capture, 1);
    capture.integer = 0;
    increment.code = {capture, instruction(Op::LoadCell, 2, {1}), instruction(Op::Add, 3, {0, 2}),
        instruction(Op::StoreCell, -1, {1, 3}), instruction(Op::Return, -1, {3})};
    program.functions = {main, increment};
    require(execute(program) == "12\n15\n", "indirect calls retain and mutate captured cells across collections");
}

void testArraysAndResults() {
    Program program;
    const Type resultType{TypeKind::Result, {String, String}};
    auto main = function("main", {}, Unit,
        {I64, I64, Type::array(I64), I64, I64, I64, I64, String, Bool, String, resultType, String});
    auto success = instruction(Op::ConstBool, 8);
    success.integer = 1;
    main.code = {number(0, 1), number(1, 2), instruction(Op::MakeArray, 2, {0, 1}),
        number(3, 1), number(4, 99), instruction(Op::SetIndex, -1, {2, 3, 4}),
        instruction(Op::Collect), instruction(Op::Index, 5, {2, 3}), instruction(Op::Print, -1, {5}),
        instruction(Op::Length, 6, {2}), instruction(Op::Print, -1, {6}), text(7, "success"), success,
        text(9, ""), instruction(Op::MakeTuple, 10, {8, 7, 9}), instruction(Op::Collect),
        instruction(Op::Unwrap, 11, {10}), instruction(Op::Print, -1, {11}), instruction(Op::Return)};
    program.functions = {main};
    require(execute(program) == "99\n2\nsuccess\n", "array mutation and managed Result unwrapping");
}

void testFloatingAndAuditedExterns() {
    Program program;
    auto main = function("main", {}, Unit, {F64, F64, I64, Bool, Bool});
    auto input = instruction(Op::ConstF64, 0);
    input.real = 9.0;
    main.code = {input, named(Op::ExternalCall, 1, {0}, "sqrt"), instruction(Op::Print, -1, {1}),
        instruction(Op::Convert, 2, {1}), instruction(Op::Print, -1, {2}), instruction(Op::Return)};
    program.functions = {main};
    program.externals = {External{"sqrt", {F64}, F64, false}};
    require(execute(program) == "3\n3\n", "audited C math signature and numeric conversion");

    input.real = std::numeric_limits<double>::quiet_NaN();
    main.code = {input, instruction(Op::Eq, 3, {0, 0}), instruction(Op::Ne, 4, {0, 0}),
        instruction(Op::Print, -1, {3}), instruction(Op::Print, -1, {4}), instruction(Op::Return)};
    program.functions = {main};
    require(execute(program) == "false\ntrue\n", "NaN comparison agrees with LLVM ordered equality and unordered inequality");

    main.code = {input, named(Op::ExternalCall, 1, {0}, "unavailable_external"), instruction(Op::Return)};
    program.functions = {main};
    program.externals = {External{"unavailable_external", {F64}, F64, false}};
    bool rejected = false;
    try { interpret(program); }
    catch (const Error& error) { rejected = std::string(error.what()).find("cannot execute extern") != std::string::npos; }
    require(rejected, "unknown external never called through an assumed function-pointer signature");
}

void testRecursionGuard() {
    Program program;
    auto main = function("main", {}, Unit, {I64, I64});
    main.code = {number(0, 2048), named(Op::Call, 1, {0}, "countdown"), instruction(Op::Return)};
    auto countdown = function("countdown", {I64}, I64, {I64, I64, Bool, I64, I64, I64});
    countdown.code = {number(1, 0), instruction(Op::Eq, 2, {0, 1}), jump(Op::JumpFalse, 4, {2}),
        instruction(Op::Return, -1, {1}), number(3, 1), instruction(Op::Sub, 4, {0, 3}),
        named(Op::Call, 5, {4}, "countdown"), instruction(Op::Return, -1, {5})};
    program.functions = {main, countdown};
    bool rejected = false;
    try { interpret(program); }
    catch (const Error& error) { rejected = std::string(error.what()).find("recursion limit") != std::string::npos; }
    require(rejected, "deep recursion reports a controlled error");
    pf_collect();
    require(pf_heap_objects() == 0, "error unwinding releases interpreter roots");
    main.code[0].integer = 10;
    program.functions[0] = main;
    require(execute(program).empty(), "ordinary recursion works after a rejected deep call");
}

void testUnsafeMemoryAndCString() {
    Program program;
    const Type integerPointer{TypeKind::Pointer, {I64}}, nativePointer{TypeKind::Pointer, {Unit}};
    auto main = function("main", {}, Unit, {I64, integerPointer, I64, I64, I64, String, nativePointer, nativePointer, I64});
    main.code = {number(0, 2), instruction(Op::RawAlloc, 1, {0}), number(2, 1), number(3, 42),
        instruction(Op::RawStore, -1, {1, 2, 3}), instruction(Op::RawLoad, 4, {1, 2}),
        instruction(Op::Print, -1, {4}), instruction(Op::RawFree, -1, {1}), text(5, "hello"),
        instruction(Op::Pin, 6, {5}), instruction(Op::CString, 7, {5}), instruction(Op::Collect),
        named(Op::ExternalCall, 8, {7}, "strlen"), instruction(Op::Print, -1, {8}),
        instruction(Op::Unpin, -1, {6}), instruction(Op::Return)};
    program.functions = {main};
    program.externals = {External{"strlen", {nativePointer}, I64, false}};
    require(execute(program) == "42\n5\n", "unsafe raw memory, pinning, C string boundary and audited strlen");
}

void testRecursiveClosure() {
    Program program;
    const auto closureType = Type::function({I64}, I64);
    auto main = function("main", {}, Unit, {Type::cell(closureType), closureType, I64, I64});
    main.code = {instruction(Op::MakeCell, 0), named(Op::MakeClosure, 1, {0}, "recursive"),
        instruction(Op::StoreCell, -1, {0, 1}), number(2, 10), instruction(Op::CallIndirect, 3, {1, 2}),
        instruction(Op::Print, -1, {3}), instruction(Op::Collect), instruction(Op::Return)};
    auto recursive = function("recursive", {I64}, I64,
        {I64, Type::cell(closureType), closureType, I64, Bool, I64, I64, I64});
    auto capture = instruction(Op::Capture, 1);
    capture.integer = 0;
    recursive.code = {capture, instruction(Op::LoadCell, 2, {1}), number(3, 0),
        instruction(Op::Eq, 4, {0, 3}), jump(Op::JumpFalse, 6, {4}), instruction(Op::Return, -1, {3}),
        number(5, 1), instruction(Op::Sub, 6, {0, 5}), instruction(Op::CallIndirect, 7, {2, 6}),
        instruction(Op::Return, -1, {7})};
    program.functions = {main, recursive};
    require(execute(program) == "0\n", "recursive nested closure shares its initialized self cell and GC reclaims the cycle");
}
void testGenericResultError() {
    Program program;
    const auto payload = Type::array(I64);
    const Type resultType{TypeKind::Result, {payload, String}};
    auto main = function("main", {}, Unit, {Bool, payload, String, resultType, String});
    main.code = {instruction(Op::ConstBool, 0), instruction(Op::Zero, 1), text(2, "unavailable"),
        instruction(Op::MakeTuple, 3, {0, 1, 2}), instruction(Op::Collect),
        instruction(Op::ResultError, 4, {3}), instruction(Op::Print, -1, {4}), instruction(Op::Return)};
    program.functions = {main};
    require(execute(program) == "unavailable\n", "typed Result placeholder and guarded error extraction support managed payloads");
}
}

int main() {
    testFunctionsAndBranches();
    testClosuresAndGC();
    testArraysAndResults();
    testFloatingAndAuditedExterns();
    testRecursionGuard();
    testUnsafeMemoryAndCString();
    testRecursiveClosure();
    testGenericResultError();
    std::cout << "interpreter tests passed\n";
}
