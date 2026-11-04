#pragma once
#include "pikaflow/common.hpp"
#include "pikaflow/frontend.hpp"
#include <map>

namespace pika {
enum class Op {
    ConstI64,
    ConstF64,
    ConstBool,
    ConstString,
    Move,
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Neg,
    Not,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    Print,
    Call,
    CallIndirect,
    MakeClosure,
    Capture,
    MakeCell,
    LoadCell,
    StoreCell,
    MakeArray,
    MakeTuple,
    Index,
    SetIndex,
    Length,
    Convert,
    Jump,
    JumpFalse,
    Return,
    Collect,
    ExternalCall,
    Panic,
    Unwrap,
    RawAlloc,
    RawFree,
    RawLoad,
    RawStore,
    CString,
    Pin,
    Unpin,
    Zero,
    ResultError,
    CCallback,
    Spawn,
    Join,
    RawOffset
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
    explicit Instruction(Op opcode) : op(opcode) {}
};
struct Function {
    std::string name;
    std::vector<std::string> parameterNames;
    std::vector<Type> parameters;
    Type result;
    std::vector<Type> registers;
    std::vector<Instruction> code;
    Location location;
    std::map<int, std::string> localNames;
};
struct CABIType {
    unsigned bits = 0;
    bool isSigned = true;
    std::vector<CABIType> fields;
    bool operator==(const CABIType &other) const {
        return bits == other.bits && isSigned == other.isSigned && fields == other.fields;
    }
    bool operator!=(const CABIType &other) const { return !(*this == other); }
};
struct External {
    std::string name;
    std::vector<Type> parameters;
    Type result;
    bool variadic = false;
    std::string symbol{};
    std::vector<CABIType> abiParameters{};
    CABIType abiResult{};
};
struct Program {
    std::vector<Function> functions;
    std::vector<External> externals;
    std::string entry = "main";
    std::string sourceFile;
    std::map<std::string, Type> nominalLayouts{};
};
Program compile(const SyntaxTree &tree);
void verify(const Program &program);
std::string dumpIR(const Program &program);
int interpret(const Program &program);
} // namespace pika
