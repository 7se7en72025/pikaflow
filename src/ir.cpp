#include "pikaflow/ir.hpp"
#include <algorithm>
#include <deque>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace pika {
namespace {
const char *name(Op op) {
    static const char *names[] = {"i64",         "f64",      "bool",
                                  "string",      "move",     "add",
                                  "sub",         "mul",      "div",
                                  "mod",         "neg",      "not",
                                  "eq",          "ne",       "lt",
                                  "le",          "gt",       "ge",
                                  "print",       "call",     "call.indirect",
                                  "closure",     "capture",  "cell",
                                  "cell.get",    "cell.set", "array",
                                  "tuple",       "index",    "index.set",
                                  "length",      "convert",  "jump",
                                  "jump.false",  "return",   "gc",
                                  "extern.call", "panic",    "unwrap",
                                  "raw.alloc",   "raw.free", "raw.load",
                                  "raw.store",   "cstr",     "pin",
                                  "unpin",       "zero",     "result.error"};
    return names[static_cast<unsigned>(op)];
}
} // namespace
void verify(const Program &program) {
    auto validType = [](const Type &initial) {
        std::vector<std::pair<const Type *, unsigned>> work{{&initial, 0}};
        while (!work.empty()) {
            auto item = work.back();
            work.pop_back();
            const auto &t = *item.first;
            if (item.second > 64)
                return false;
            switch (t.kind) {
            case TypeKind::Unit:
            case TypeKind::I64:
            case TypeKind::F64:
            case TypeKind::Bool:
            case TypeKind::String:
                if (!t.args.empty())
                    return false;
                break;
            case TypeKind::Array:
            case TypeKind::Cell:
                if (t.args.size() != 1 || t.args[0] == Unit)
                    return false;
                break;
            case TypeKind::Pointer:
                if (t.args.size() != 1)
                    return false;
                break;
            case TypeKind::Result:
                if (t.args.size() != 2 || t.args[0] == Unit || t.args[1] == Unit)
                    return false;
                break;
            case TypeKind::Tuple:
                for (auto &arg : t.args)
                    if (arg == Unit)
                        return false;
                break;
            case TypeKind::Function:
                if (t.args.empty())
                    return false;
                for (size_t i = 0; i + 1 < t.args.size(); ++i)
                    if (t.args[i] == Unit)
                        return false;
                break;
            default:
                return false;
            }
            for (const auto &arg : t.args)
                work.push_back({&arg, item.second + 1});
        }
        return true;
    };
    std::map<std::string, const Function *> functions;
    for (const auto &function : program.functions) {
        if (!functions.emplace(function.name, &function).second)
            throw Error(function.location, "duplicate IR function");
    }
    if (!functions.count(program.entry))
        throw Error({program.sourceFile, 1, 1}, "missing IR entry function");
    for (const auto &f : program.functions) {
        if (!validType(f.result))
            throw Error(f.location, "invalid IR result type");
        for (const auto &t : f.registers)
            if (!validType(t) || t == Unit)
                throw Error(f.location, "invalid IR register type");
        for (const auto &t : f.parameters)
            if (!validType(t) || t == Unit)
                throw Error(f.location, "invalid IR parameter type");
        for (const auto &local : f.localNames)
            if (local.first < 0 || static_cast<size_t>(local.first) >= f.registers.size() ||
                f.registers[local.first].kind != TypeKind::Cell || local.second.empty())
                throw Error(f.location, "invalid IR source variable metadata");
        if (f.code.empty())
            throw Error(f.location, "empty IR function");
        if (f.registers.size() < f.parameters.size())
            throw Error(f.location, "missing IR parameter registers");
        for (size_t i = 0; i < f.parameters.size(); ++i)
            if (f.registers[i] != f.parameters[i])
                throw Error(f.location, "IR parameter type mismatch");
        auto check = [&](bool valid, const Instruction &ins, const std::string &message) {
            if (!valid)
                throw Error(ins.location, "invalid typed IR: " + message);
        };
        std::map<int, int64_t> constants;
        std::set<int> nonconstant;
        for (const auto &ins : f.code)
            if (ins.dst >= 0) {
                if (ins.op != Op::ConstI64)
                    nonconstant.insert(ins.dst);
                else if (constants.count(ins.dst) && constants[ins.dst] != ins.integer)
                    nonconstant.insert(ins.dst);
                else
                    constants[ins.dst] = ins.integer;
            }
        for (const auto &ins : f.code) {
            check(static_cast<unsigned>(ins.op) <= static_cast<unsigned>(Op::ResultError), ins,
                  "unknown opcode");
            check(ins.dst >= -1 &&
                      (ins.dst < 0 || static_cast<size_t>(ins.dst) < f.registers.size()),
                  ins, "destination out of range");
            for (int arg : ins.args)
                check(arg >= 0 && static_cast<size_t>(arg) < f.registers.size(), ins,
                      "operand out of range");
            auto arity = [&](size_t count) {
                check(ins.args.size() == count, ins,
                      "wrong operand count for " + std::string(name(ins.op)));
            };
            auto dst = [&]() -> Type {
                check(ins.dst >= 0, ins, "instruction requires destination");
                return f.registers[ins.dst];
            };
            auto arg = [&](size_t i) -> Type { return f.registers[ins.args.at(i)]; };
            switch (ins.op) {
            case Op::ConstI64:
                arity(0);
                check(dst() == I64, ins, "integer type");
                break;
            case Op::ConstF64:
                arity(0);
                check(dst() == F64, ins, "float type");
                break;
            case Op::ConstBool:
                arity(0);
                check(dst() == Bool, ins, "boolean type");
                break;
            case Op::ConstString:
                arity(0);
                check(dst() == String, ins, "string type");
                break;
            case Op::Move:
                arity(1);
                check(dst() == arg(0), ins, "move type mismatch");
                break;
            case Op::Add:
            case Op::Sub:
            case Op::Mul:
            case Op::Div:
            case Op::Mod:
                arity(2);
                check(dst() == arg(0) && arg(0) == arg(1) && arg(0).numeric(), ins,
                      "arithmetic type mismatch");
                break;
            case Op::Neg:
                arity(1);
                check(dst() == arg(0) && arg(0).numeric(), ins, "negation type mismatch");
                break;
            case Op::Not:
                arity(1);
                check(dst() == Bool && arg(0) == Bool, ins, "not type mismatch");
                break;
            case Op::Eq:
            case Op::Ne:
            case Op::Lt:
            case Op::Le:
            case Op::Gt:
            case Op::Ge:
                arity(2);
                check(dst() == Bool && arg(0) == arg(1), ins, "comparison type mismatch");
                check(arg(0).numeric() || ((ins.op == Op::Eq || ins.op == Op::Ne) &&
                                           (arg(0) == Bool || arg(0) == String ||
                                            arg(0).kind == TypeKind::Pointer)),
                      ins, "unsupported comparison type");
                break;
            case Op::Jump:
                arity(0);
                [[fallthrough]];
            case Op::JumpFalse:
                check(ins.dst < 0, ins, "branch cannot define a register");
                if (ins.op == Op::JumpFalse) {
                    arity(1);
                    check(arg(0) == Bool, ins, "branch requires bool");
                }
                check(ins.target >= 0 && static_cast<size_t>(ins.target) < f.code.size(), ins,
                      "branch target out of range");
                break;
            case Op::Call: {
                auto target = functions.find(ins.text);
                check(target != functions.end(), ins, "unknown function");
                const auto &callee = *target->second;
                for (const auto &operation : callee.code)
                    check(operation.op != Op::Capture, ins,
                          "captured functions require indirect closure calls");
                check(ins.args.size() == callee.parameters.size(), ins, "call arity");
                for (size_t i = 0; i < ins.args.size(); ++i)
                    check(arg(i) == callee.parameters[i], ins, "call argument type");
                check((callee.result == Unit && ins.dst < 0) ||
                          (ins.dst >= 0 && dst() == callee.result),
                      ins, "call return type");
                break;
            }
            case Op::Return:
                check(ins.dst < 0, ins, "return destination");
                check((f.result == Unit && ins.args.empty()) ||
                          (ins.args.size() == 1 && arg(0) == f.result),
                      ins, "return type");
                break;
            case Op::MakeCell:
                if (ins.args.empty())
                    check(dst().kind == TypeKind::Cell && dst().args[0].kind == TypeKind::Function,
                          ins, "empty cell requires function type");
                else {
                    arity(1);
                    check(dst() == Type::cell(arg(0)), ins, "cell type");
                }
                break;
            case Op::LoadCell:
                arity(1);
                check(arg(0).kind == TypeKind::Cell && arg(0).args[0] == dst(), ins,
                      "cell load type");
                break;
            case Op::StoreCell:
                arity(2);
                check(ins.dst < 0, ins, "cell store cannot define a register");
                check(arg(0) == Type::cell(arg(1)), ins, "cell store type");
                break;
            case Op::Capture:
                arity(0);
                check(dst().kind == TypeKind::Cell && ins.integer >= 0, ins, "capture type");
                break;
            case Op::MakeClosure: {
                check(dst().kind == TypeKind::Function && functions.count(ins.text), ins,
                      "closure target");
                const auto &target = *functions.at(ins.text);
                check(dst() == Type::function(target.parameters, target.result), ins,
                      "closure signature");
                for (size_t i = 0; i < ins.args.size(); ++i)
                    check(arg(i).kind == TypeKind::Cell, ins, "capture must be a cell");
                for (const auto &capture : target.code)
                    if (capture.op == Op::Capture) {
                        check(capture.integer >= 0 &&
                                  static_cast<size_t>(capture.integer) < ins.args.size(),
                              ins, "capture index outside closure environment");
                        check(target.registers.at(capture.dst) ==
                                  arg(static_cast<size_t>(capture.integer)),
                              ins, "capture environment type mismatch");
                    }
                break;
            }
            case Op::CallIndirect: {
                check(!ins.args.empty(), ins, "missing closure");
                Type t = arg(0);
                check(t.kind == TypeKind::Function && t.args.size() == ins.args.size(), ins,
                      "closure arity");
                for (size_t i = 1; i < ins.args.size(); ++i)
                    check(arg(i) == t.args[i - 1], ins, "closure argument");
                check((t.args.back() == Unit && ins.dst < 0) ||
                          (ins.dst >= 0 && dst() == t.args.back()),
                      ins, "closure result");
                break;
            }
            case Op::MakeArray:
                check(dst().kind == TypeKind::Array && dst().args.size() == 1, ins, "array type");
                for (size_t i = 0; i < ins.args.size(); ++i)
                    check(arg(i) == dst().args[0], ins, "array element");
                break;
            case Op::MakeTuple: {
                Type t = dst();
                check(t.kind == TypeKind::Tuple || t.kind == TypeKind::Result, ins, "tuple type");
                if (t.kind == TypeKind::Tuple) {
                    check(ins.args.size() == t.args.size(), ins, "tuple arity");
                    for (size_t i = 0; i < ins.args.size(); ++i)
                        check(arg(i) == t.args[i], ins, "tuple element");
                } else {
                    arity(3);
                    check(arg(0) == Bool && arg(1) == t.args[0] && arg(2) == t.args[1], ins,
                          "Result representation");
                }
                break;
            }
            case Op::Index: {
                arity(2);
                check(arg(1) == I64, ins, "index type");
                Type base = arg(0);
                if (base.kind == TypeKind::Array)
                    check(dst() == base.args[0], ins, "array index result type");
                else if (base == String)
                    check(dst() == I64, ins, "string index result type");
                else if (base.kind == TypeKind::Tuple || base.kind == TypeKind::Result) {
                    std::vector<Type> fields =
                        base.kind == TypeKind::Tuple
                            ? base.args
                            : std::vector<Type>{Bool, base.args[0], base.args[1]};
                    check(constants.count(ins.args[1]) && !nonconstant.count(ins.args[1]), ins,
                          "heterogeneous index must be constant");
                    int64_t index = constants.at(ins.args[1]);
                    check(index >= 0 && static_cast<size_t>(index) < fields.size(), ins,
                          "tuple index bounds");
                    check(dst() == fields[static_cast<size_t>(index)], ins,
                          "tuple index result type");
                } else
                    check(false, ins, "non-indexable base type");
                break;
            }
            case Op::SetIndex:
                arity(3);
                check(ins.dst < 0, ins, "array store cannot define a register");
                check(arg(0).kind == TypeKind::Array && arg(1) == I64 && arg(0).args[0] == arg(2),
                      ins, "array store type");
                break;
            case Op::Length:
                arity(1);
                check(dst() == I64 && (arg(0) == String || arg(0).kind == TypeKind::Array ||
                                       arg(0).kind == TypeKind::Tuple),
                      ins, "length operand/result");
                break;
            case Op::Convert:
                arity(1);
                check(dst().numeric() && arg(0).numeric(), ins, "conversion type");
                break;
            case Op::Print:
                arity(1);
                check(ins.dst < 0, ins, "print destination");
                break;
            case Op::Collect:
                arity(0);
                check(ins.dst < 0, ins, "collect destination");
                break;
            case Op::Panic:
                arity(1);
                check(arg(0) == String && ins.dst < 0, ins, "panic type");
                break;
            case Op::Unwrap:
                arity(1);
                check(arg(0).kind == TypeKind::Result && dst() == arg(0).args[0], ins,
                      "unwrap type");
                break;
            case Op::ResultError:
                arity(1);
                check(arg(0).kind == TypeKind::Result && dst() == arg(0).args[1], ins,
                      "Result error type");
                break;
            case Op::Zero:
                arity(0);
                check(dst() != Unit, ins, "zero placeholder cannot be unit");
                break;
            case Op::RawAlloc:
                arity(1);
                check(arg(0) == I64 && dst().kind == TypeKind::Pointer &&
                          (dst().args[0] == I64 || dst().args[0] == F64 || dst().args[0] == Bool),
                      ins, "raw allocation type");
                break;
            case Op::RawFree:
            case Op::Unpin:
                arity(1);
                check(arg(0).kind == TypeKind::Pointer && ins.dst < 0, ins, "pointer release type");
                break;
            case Op::RawLoad:
                arity(2);
                check(arg(0).kind == TypeKind::Pointer && arg(1) == I64 && dst() == arg(0).args[0],
                      ins, "raw load type");
                break;
            case Op::RawStore:
                arity(3);
                check(ins.dst < 0, ins, "raw store cannot define a register");
                check(arg(0).kind == TypeKind::Pointer && arg(1) == I64 && arg(2) == arg(0).args[0],
                      ins, "raw store type");
                break;
            case Op::CString:
                arity(1);
                check(arg(0) == String && dst() == Type{TypeKind::Pointer, {Unit}}, ins,
                      "C string type");
                break;
            case Op::Pin:
                arity(1);
                check(arg(0).managed() && dst() == Type{TypeKind::Pointer, {Unit}}, ins,
                      "pin type");
                break;
            case Op::ExternalCall: {
                auto ext = std::find_if(program.externals.begin(), program.externals.end(),
                                        [&](const External &e) { return e.name == ins.text; });
                check(ext != program.externals.end(), ins, "unknown external");
                check(ins.args.size() >= ext->parameters.size() &&
                          (ext->variadic || ins.args.size() == ext->parameters.size()),
                      ins, "extern arity");
                for (size_t i = 0; i < ext->parameters.size(); ++i)
                    check(arg(i) == ext->parameters[i], ins, "extern argument type");
                check((ext->result == Unit && ins.dst < 0) ||
                          (ins.dst >= 0 && dst() == ext->result),
                      ins, "extern return type");
                break;
            }
            }
        }
        // Definite assignment uses intersection at joins, including loop backedges.
        std::vector<std::vector<bool>> incoming(f.code.size(),
                                                std::vector<bool>(f.registers.size(), true));
        std::vector<bool> reachable(f.code.size(), false), queued(f.code.size(), false);
        incoming[0].assign(f.registers.size(), false);
        for (size_t i = 0; i < f.parameters.size(); ++i)
            incoming[0][i] = true;
        reachable[0] = queued[0] = true;
        std::deque<size_t> work{0};
        auto successors = [&](size_t pc) {
            std::vector<size_t> result;
            const auto &ins = f.code[pc];
            if (ins.op == Op::Return || ins.op == Op::Panic)
                return result;
            if (ins.op == Op::Jump || ins.op == Op::JumpFalse)
                result.push_back(static_cast<size_t>(ins.target));
            if (ins.op != Op::Jump && pc + 1 < f.code.size())
                result.push_back(pc + 1);
            return result;
        };
        while (!work.empty()) {
            size_t pc = work.front();
            work.pop_front();
            queued[pc] = false;
            auto state = incoming[pc];
            const auto &ins = f.code[pc];
            if (ins.dst >= 0)
                state[ins.dst] = true;
            for (size_t next : successors(pc)) {
                bool changed = !reachable[next];
                if (!reachable[next])
                    incoming[next] = state;
                else
                    for (size_t i = 0; i < state.size(); ++i) {
                        bool value = incoming[next][i] && state[i];
                        if (value != incoming[next][i]) {
                            incoming[next][i] = value;
                            changed = true;
                        }
                    }
                reachable[next] = true;
                if (changed && !queued[next]) {
                    queued[next] = true;
                    work.push_back(next);
                }
            }
        }
        for (size_t pc = 0; pc < f.code.size(); ++pc)
            if (reachable[pc]) {
                for (int arg : f.code[pc].args)
                    if (!incoming[pc][arg])
                        throw Error(
                            f.code[pc].location,
                            "invalid typed IR: operand may be uninitialized at instruction " +
                                std::to_string(pc));
                if (pc + 1 == f.code.size() && f.code[pc].op != Op::Return &&
                    f.code[pc].op != Op::Panic && f.code[pc].op != Op::Jump)
                    throw Error(f.code[pc].location, "invalid typed IR: function falls through");
            }
    }
}
std::string dumpIR(const Program &program) {
    std::ostringstream out;
    for (const auto &f : program.functions) {
        out << "fun " << f.name << "(";
        for (size_t i = 0; i < f.parameters.size(); ++i) {
            if (i)
                out << ", ";
            out << "r" << i << ":" << f.parameters[i].str();
        }
        out << ") -> " << f.result.str() << "\n";
        for (size_t pc = 0; pc < f.code.size(); ++pc) {
            const auto &ins = f.code[pc];
            out << "  " << std::setw(4) << pc << "  ";
            if (ins.dst >= 0)
                out << "r" << ins.dst << ":" << f.registers[ins.dst].str() << " = ";
            out << name(ins.op);
            if (!ins.text.empty())
                out << " " << std::quoted(ins.text);
            if (ins.op == Op::ConstI64 || ins.op == Op::ConstBool || ins.op == Op::Capture)
                out << " " << ins.integer;
            if (ins.op == Op::ConstF64)
                out << " " << std::setprecision(17) << ins.real;
            for (int arg : ins.args)
                out << " r" << arg;
            if (ins.target >= 0)
                out << " -> " << ins.target;
            out << "\n";
        }
    }
    return out.str();
}
} // namespace pika
