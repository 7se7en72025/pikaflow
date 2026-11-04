#include "pikaflow/ir.hpp"
#include "pikaflow/runtime.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace pika {
namespace {
uint64_t integerBits(int64_t number) {
    uint64_t result;
    std::memcpy(&result, &number, sizeof(result));
    return result;
}
int64_t integer(PFValue value) {
    int64_t result;
    std::memcpy(&result, &value.bits, sizeof(result));
    return result;
}
uint64_t floatingBits(double number) {
    uint64_t result;
    std::memcpy(&result, &number, sizeof(result));
    return result;
}
double floating(PFValue value) {
    double result;
    std::memcpy(&result, &value.bits, sizeof(result));
    return result;
}
void* reference(PFValue value) { return reinterpret_cast<void*>(static_cast<uintptr_t>(value.bits)); }
PFValue fromInteger(int64_t value) { return {integerBits(value), static_cast<uint32_t>(TypeKind::I64), 0}; }
PFValue fromFloat(double value) { return {floatingBits(value), static_cast<uint32_t>(TypeKind::F64), 0}; }
PFValue fromBool(bool value) { return {value ? uint64_t(1) : uint64_t(0), static_cast<uint32_t>(TypeKind::Bool), 0}; }
PFValue fromReference(void* value, const Type& type) {
    return {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value)), static_cast<uint32_t>(type.kind), 0};
}

struct Roots {
    std::vector<void*> slots;
    void* frame;
    explicit Roots(size_t count) : slots(count, nullptr), frame(pf_roots_push(slots.data(), static_cast<int64_t>(count))) {}
    ~Roots() { pf_roots_pop(frame); }
    Roots(const Roots&) = delete;
    Roots& operator=(const Roots&) = delete;
};

class Interpreter {
    const Program& program;
    std::unordered_map<std::string, size_t> functions;
    std::unordered_map<std::string, const External*> externals;

    PFValue external(const Instruction& instruction, const std::vector<PFValue>& arguments) {
        const auto& declaration = *externals.at(instruction.text);
        auto matches = [&](size_t count, const Type& input, const Type& output) {
            if (declaration.variadic || declaration.parameters.size() != count || declaration.result != output) return false;
            for (const auto& type : declaration.parameters) if (type != input) return false;
            return true;
        };
        struct Unary { const char* name; double (*call)(double); };
        static const Unary unary[] = {
            {"sqrt", static_cast<double (*)(double)>(&std::sqrt)},
            {"fabs", static_cast<double (*)(double)>(&std::fabs)},
            {"sin", static_cast<double (*)(double)>(&std::sin)},
            {"cos", static_cast<double (*)(double)>(&std::cos)},
            {"tan", static_cast<double (*)(double)>(&std::tan)},
            {"asin", static_cast<double (*)(double)>(&std::asin)},
            {"acos", static_cast<double (*)(double)>(&std::acos)},
            {"atan", static_cast<double (*)(double)>(&std::atan)},
            {"exp", static_cast<double (*)(double)>(&std::exp)},
            {"exp2", static_cast<double (*)(double)>(&std::exp2)},
            {"log", static_cast<double (*)(double)>(&std::log)},
            {"log2", static_cast<double (*)(double)>(&std::log2)},
            {"log10", static_cast<double (*)(double)>(&std::log10)},
            {"floor", static_cast<double (*)(double)>(&std::floor)},
            {"ceil", static_cast<double (*)(double)>(&std::ceil)},
            {"trunc", static_cast<double (*)(double)>(&std::trunc)},
            {"round", static_cast<double (*)(double)>(&std::round)}
        };
        for (const auto& operation : unary)
            if (instruction.text == operation.name && matches(1, F64, F64))
                return fromFloat(operation.call(floating(arguments[0])));
        if (matches(2, F64, F64)) {
            const auto a = floating(arguments[0]), b = floating(arguments[1]);
            if (instruction.text == "pow") return fromFloat(std::pow(a, b));
            if (instruction.text == "atan2") return fromFloat(std::atan2(a, b));
            if (instruction.text == "fmod") return fromFloat(std::fmod(a, b));
        }
        if (instruction.text == "llabs" && matches(1, I64, I64)) {
            const auto value = integer(arguments[0]);
            return fromInteger(value < 0 ? pf_checked_neg(value) : value);
        }
        throw Error(instruction.location,
            "reference interpreter cannot execute extern '" + instruction.text +
            "' with this signature; use native compilation or JIT for other C functions");
    }

    PFValue call(size_t functionIndex, const std::vector<PFValue>& arguments, void* environment, unsigned depth) {
        const auto& function = program.functions.at(functionIndex);
        // The reference interpreter uses native recursion, bounded for Windows' default stack.
        if (depth >= 256) throw Error(function.location, "reference interpreter recursion limit (256 frames) exceeded");
        std::vector<PFValue> registers(function.registers.size(), PFValue{0, 0, 0});
        Roots roots(registers.size() + 1);
        roots.slots.back() = environment;
        auto set = [&](int destination, PFValue value) {
            if (destination < 0) return;
            const auto& type = function.registers.at(static_cast<size_t>(destination));
            value.tag = static_cast<uint32_t>(type.kind);
            registers.at(static_cast<size_t>(destination)) = value;
            roots.slots.at(static_cast<size_t>(destination)) = type.managed() ? reference(value) : nullptr;
        };
        for (size_t index = 0; index < arguments.size(); ++index) set(static_cast<int>(index), arguments[index]);

        for (size_t pc = 0; pc < function.code.size();) {
            const auto& instruction = function.code[pc];
            auto arg = [&](size_t index) { return registers.at(static_cast<size_t>(instruction.args.at(index))); };
            auto argType = [&](size_t index) -> const Type& { return function.registers.at(static_cast<size_t>(instruction.args.at(index))); };
            auto destinationType = [&]() -> const Type& { return function.registers.at(static_cast<size_t>(instruction.dst)); };
            PFValue result{0, 0, 0};
            bool assigned = true;
            switch (instruction.op) {
            case Op::ConstI64: result = fromInteger(instruction.integer); break;
            case Op::ConstF64: result = fromFloat(instruction.real); break;
            case Op::ConstBool: result = fromBool(instruction.integer != 0); break;
            case Op::ConstString:
                result = fromReference(pf_string(instruction.text.data(), static_cast<int64_t>(instruction.text.size())), String);
                break;
            case Op::Move: result = arg(0); break;
            case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: {
                if (argType(0) == F64) {
                    const auto a = floating(arg(0)), b = floating(arg(1));
                    switch (instruction.op) {
                    case Op::Add: result = fromFloat(a + b); break;
                    case Op::Sub: result = fromFloat(a - b); break;
                    case Op::Mul: result = fromFloat(a * b); break;
                    case Op::Div: result = fromFloat(a / b); break;
                    default: result = fromFloat(std::fmod(a, b)); break;
                    }
                } else {
                    const auto a = integer(arg(0)), b = integer(arg(1));
                    switch (instruction.op) {
                    case Op::Add: result = fromInteger(pf_checked_add(a, b)); break;
                    case Op::Sub: result = fromInteger(pf_checked_sub(a, b)); break;
                    case Op::Mul: result = fromInteger(pf_checked_mul(a, b)); break;
                    case Op::Div: result = fromInteger(pf_checked_div(a, b)); break;
                    default: result = fromInteger(pf_checked_mod(a, b)); break;
                    }
                }
                break;
            }
            case Op::Neg:
                result = argType(0) == F64 ? fromFloat(-floating(arg(0))) : fromInteger(pf_checked_neg(integer(arg(0))));
                break;
            case Op::Not: result = fromBool(!arg(0).bits); break;
            case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: {
                bool answer = false;
                if (argType(0) == F64) {
                    const auto a = floating(arg(0)), b = floating(arg(1));
                    switch (instruction.op) {
                    case Op::Eq: answer = a == b; break;
                    case Op::Ne: answer = a != b; break;
                    case Op::Lt: answer = a < b; break;
                    case Op::Le: answer = a <= b; break;
                    case Op::Gt: answer = a > b; break;
                    default: answer = a >= b; break;
                    }
                } else if (argType(0) == String) {
                    const bool equal = pf_string_equal(reference(arg(0)), reference(arg(1))) != 0;
                    answer = instruction.op == Op::Eq ? equal : !equal;
                } else {
                    const auto a = integer(arg(0)), b = integer(arg(1));
                    switch (instruction.op) {
                    case Op::Eq: answer = a == b; break;
                    case Op::Ne: answer = a != b; break;
                    case Op::Lt: answer = a < b; break;
                    case Op::Le: answer = a <= b; break;
                    case Op::Gt: answer = a > b; break;
                    default: answer = a >= b; break;
                    }
                }
                result = fromBool(answer);
                break;
            }
            case Op::Print: pf_print(arg(0).bits, arg(0).tag); assigned = false; break;
            case Op::Call: case Op::CallIndirect: {
                std::vector<PFValue> parameters;
                const size_t offset = instruction.op == Op::CallIndirect ? 1 : 0;
                for (size_t index = offset; index < instruction.args.size(); ++index) parameters.push_back(arg(index));
                size_t target = 0;
                void* closure = nullptr;
                if (instruction.op == Op::Call) target = functions.at(instruction.text);
                else {
                    closure = reference(arg(0));
                    const auto encoded = reinterpret_cast<uintptr_t>(pf_closure_code(closure));
                    if (encoded == 0 || encoded > program.functions.size())
                        throw Error(instruction.location, "invalid reference interpreter function handle");
                    target = static_cast<size_t>(encoded - 1);
                }
                result = call(target, parameters, closure, depth + 1);
                break;
            }
            case Op::MakeClosure: {
                auto* closure = pf_closure_new(reinterpret_cast<void*>(static_cast<uintptr_t>(functions.at(instruction.text) + 1)),
                    static_cast<int64_t>(instruction.args.size()));
                result = fromReference(closure, destinationType());
                set(instruction.dst, result);
                for (size_t index = 0; index < instruction.args.size(); ++index)
                    pf_closure_set(closure, static_cast<int64_t>(index), reference(arg(index)));
                break;
            }
            case Op::Capture:
                if (!environment) throw Error(instruction.location, "capturing function requires a closure environment");
                result = fromReference(pf_closure_get(environment, instruction.integer), destinationType());
                break;
            case Op::MakeCell: result = fromReference(pf_cell_new(arg(0).bits, arg(0).tag), destinationType()); break;
            case Op::LoadCell: result = {pf_cell_get(reference(arg(0))), static_cast<uint32_t>(destinationType().kind), 0}; break;
            case Op::StoreCell:
                pf_cell_set(reference(arg(0)), arg(1).bits, arg(1).tag); assigned = false; break;
            case Op::MakeArray: case Op::MakeTuple: {
                const auto count = static_cast<int64_t>(instruction.args.size());
                void* sequence = instruction.op == Op::MakeArray
                    ? pf_array_new(count, static_cast<uint32_t>(destinationType().args.at(0).kind)) : pf_tuple_new(count);
                result = fromReference(sequence, destinationType());
                set(instruction.dst, result);
                for (size_t index = 0; index < instruction.args.size(); ++index)
                    pf_set_index(sequence, static_cast<int64_t>(index), arg(index).bits, arg(index).tag);
                break;
            }
            case Op::Index: {
                const auto index = integer(arg(1));
                auto* sequence = reference(arg(0));
                if (pf_index_tag(sequence, index) != static_cast<uint32_t>(destinationType().kind))
                    throw Error(instruction.location, "indexed value does not match its declared IR type");
                result = {pf_index(sequence, index), static_cast<uint32_t>(destinationType().kind), 0};
                break;
            }
            case Op::SetIndex:
                pf_set_index(reference(arg(0)), integer(arg(1)), arg(2).bits, arg(2).tag); assigned = false; break;
            case Op::Length: result = fromInteger(pf_length(reference(arg(0)))); break;
            case Op::Convert:
                if (destinationType() == argType(0)) result = arg(0);
                else if (destinationType() == F64) result = fromFloat(static_cast<double>(integer(arg(0))));
                else result = fromInteger(pf_float_to_int(floating(arg(0))));
                break;
            case Op::Jump: pc = static_cast<size_t>(instruction.target); continue;
            case Op::JumpFalse:
                if (!arg(0).bits) { pc = static_cast<size_t>(instruction.target); continue; }
                assigned = false;
                break;
            case Op::Return: return instruction.args.empty() ? PFValue{0, 0, 0} : arg(0);
            case Op::Collect: pf_collect(); assigned = false; break;
            case Op::ExternalCall: {
                std::vector<PFValue> parameters;
                for (size_t index = 0; index < instruction.args.size(); ++index) parameters.push_back(arg(index));
                result = external(instruction, parameters);
                break;
            }
            case Op::Panic: pf_panic(reference(arg(0))); assigned = false; break;
            case Op::Unwrap: result = {pf_result_value(reference(arg(0))), static_cast<uint32_t>(destinationType().kind), 0}; break;
            }
            if (assigned && instruction.dst >= 0) set(instruction.dst, result);
            ++pc;
        }
        throw Error(function.location, "reference interpreter reached the end of a function without returning");
    }

public:
    explicit Interpreter(const Program& source) : program(source) {
        for (size_t index = 0; index < program.functions.size(); ++index) functions.emplace(program.functions[index].name, index);
        for (const auto& external : program.externals) externals.emplace(external.name, &external);
    }
    int run() {
        const auto index = functions.at(program.entry);
        const auto& entry = program.functions.at(index);
        if (!entry.parameters.empty() || (entry.result != Unit && entry.result != I64))
            throw Error(entry.location, "entry function must take no arguments and return unit or i64");
        const auto result = call(index, {}, nullptr, 0);
        if (entry.result == Unit) return 0;
        // Match the native i32 entry wrapper's low 32 bits without signed overflow.
        const auto low = static_cast<uint32_t>(result.bits);
        int32_t status;
        std::memcpy(&status, &low, sizeof(status));
        return static_cast<int>(status);
    }
};
}

int interpret(const Program& program) {
    verify(program);
    return Interpreter(program).run();
}
}
