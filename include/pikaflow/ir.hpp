#pragma once
#include "pikaflow/common.hpp"
#include "pikaflow/frontend.hpp"

namespace pika {
enum class Op {
    ConstI64, ConstF64, ConstBool, ConstString, Move,
    Add, Sub, Mul, Div, Mod, Neg, Not, Eq, Ne, Lt, Le, Gt, Ge,
    Print, Call, CallIndirect, MakeClosure, Capture, MakeCell, LoadCell, StoreCell,
    MakeArray, MakeTuple, Index, SetIndex, Length, Convert,
    Jump, JumpFalse, Return, Collect, ExternalCall, Panic, Unwrap
};
struct Instruction {
    Op op;
    int dst = -1;
    std::vector<int> args;
    std::string text;
    int64_t integer = 0;
    double real = 0;
    int target = -1;
    Location location;
};
struct Function {
    std::string name;
    std::vector<std::string> parameterNames;
    std::vector<Type> parameters;
    Type result;
    std::vector<Type> registers;
    std::vector<Instruction> code;
    Location location;
};
struct External {
    std::string name;
    std::vector<Type> parameters;
    Type result;
    bool variadic = false;
};
struct Program {
    std::vector<Function> functions;
    std::vector<External> externals;
    std::string entry = "main";
    std::string sourceFile;
};
Program compile(const SyntaxTree& tree);
void verify(const Program& program);
std::string dumpIR(const Program& program);
int interpret(const Program& program);
}
