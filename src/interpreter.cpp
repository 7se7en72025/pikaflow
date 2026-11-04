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

struct Frame {
    const Function& function;
    std::vector<PFValue> registers;
    Roots roots;
    struct Tasks {
        void* scope = pf_tasks_enter();
        ~Tasks() { pf_tasks_leave(scope); }
    } tasks;
    void* environment;
    int returnDestination;
    size_t pc = 0;
    Frame(const Function& source, const std::vector<PFValue>& arguments, void* closure, int destination)
        : function(source), registers(source.registers.size(), PFValue{0, 0, 0}),
          roots(registers.size() + 1), environment(closure), returnDestination(destination) {
        roots.slots.back() = environment;
        for (size_t index = 0; index < arguments.size(); ++index) set(static_cast<int>(index), arguments[index]);
    }
    void set(int destination, PFValue value) {
        if (destination < 0) return;
        const auto& type = function.registers.at(static_cast<size_t>(destination));
        value.tag = static_cast<uint32_t>(type.kind);
        registers.at(static_cast<size_t>(destination)) = value;
        roots.slots.at(static_cast<size_t>(destination)) = type.managed() ? reference(value) : nullptr;
    }
};

struct Frames {
    std::vector<std::unique_ptr<Frame>> values;
    // Explicit reverse destruction preserves the runtime's LIFO root-frame contract.
    ~Frames() { while (!values.empty()) values.pop_back(); }
};

class Interpreter {
    const Program& program;
    std::unordered_map<std::string, size_t> functions;
    std::unordered_map<std::string, const External*> externals;

    static uint64_t taskEntry(void* closure, void* context) {
        const auto& source = *static_cast<const Program*>(context);
        const auto encoded = reinterpret_cast<uintptr_t>(pf_closure_code(closure));
        if (!encoded || encoded > source.functions.size())
            throw Error({source.sourceFile, 1, 1}, "invalid task function handle");
        return Interpreter(source).call(static_cast<size_t>(encoded - 1), {}, closure).bits;
    }

    PFValue external(const Instruction& instruction, const std::vector<PFValue>& arguments) {
        const auto& declaration = *externals.at(instruction.text);
        const auto& symbol = declaration.symbol.empty() ? declaration.name : declaration.symbol;
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
            if (symbol == operation.name && matches(1, F64, F64))
                return fromFloat(operation.call(floating(arguments[0])));
        if (matches(2, F64, F64)) {
            const auto a = floating(arguments[0]), b = floating(arguments[1]);
            if (symbol == "pow") return fromFloat(std::pow(a, b));
            if (symbol == "atan2") return fromFloat(std::atan2(a, b));
            if (symbol == "fmod") return fromFloat(std::fmod(a, b));
        }
        if (symbol == "llabs" && matches(1, I64, I64)) {
            const auto value = integer(arguments[0]);
            return fromInteger(value < 0 ? pf_checked_neg(value) : value);
        }
        const Type nativePointer{TypeKind::Pointer, {Unit}};
        if (symbol == "strlen" && sizeof(size_t) == sizeof(int64_t) && matches(1, nativePointer, I64)) {
            const auto* bytes = static_cast<const char*>(reference(arguments[0]));
            if (!bytes) pf_fail("null C string pointer", instruction.location.file.c_str(), static_cast<int32_t>(instruction.location.line));
            const auto count = std::strlen(bytes);
            if (count > static_cast<size_t>(std::numeric_limits<int64_t>::max()))
                throw Error(instruction.location, "C string length does not fit i64");
            return fromInteger(static_cast<int64_t>(count));
        }
        throw Error(instruction.location,
            "reference interpreter cannot execute extern '" + instruction.text +
            "' with this signature; use native compilation or JIT for other C functions");
    }

    PFValue call(size_t functionIndex, const std::vector<PFValue>& arguments, void* environment) {
        Frames frames;
        frames.values.push_back(std::make_unique<Frame>(program.functions.at(functionIndex), arguments, environment, -1));
        while (!frames.values.empty()) {
            auto& frame = *frames.values.back();
            const auto& function = frame.function;
            auto& registers = frame.registers;
            auto& pc = frame.pc;
            if (pc >= function.code.size())
                throw Error(function.location, "reference interpreter reached the end of a function without returning");
            auto set = [&](int destination, PFValue value) { frame.set(destination, value); };
            const auto& instruction = function.code[pc];
            pf_source_location(instruction.location.file.c_str(), static_cast<int32_t>(instruction.location.line),
                static_cast<int32_t>(instruction.location.column));
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
                if (frames.values.size() >= 1024)
                    throw Error(instruction.location, "reference interpreter recursion limit (1024 frames) exceeded");
                auto next = std::make_unique<Frame>(program.functions.at(target), parameters, closure, instruction.dst);
                ++pc;
                frames.values.push_back(std::move(next));
                continue;
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
                if (!frame.environment) throw Error(instruction.location, "capturing function requires a closure environment");
                result = fromReference(pf_closure_get(frame.environment, instruction.integer), destinationType());
                break;
            case Op::MakeCell:
                result = fromReference(instruction.args.empty()
                    ? pf_cell_new(0, static_cast<uint32_t>(destinationType().args.at(0).kind))
                    : pf_cell_new(arg(0).bits, arg(0).tag), destinationType());
                break;
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
            case Op::Return: {
                const auto returned = instruction.args.empty() ? PFValue{0, 0, 0} : arg(0);
                const int destination = frame.returnDestination;
                frames.values.pop_back();
                if (frames.values.empty()) return returned;
                frames.values.back()->set(destination, returned);
                continue;
            }
            case Op::Collect: pf_collect(); assigned = false; break;
            case Op::ExternalCall: {
                std::vector<PFValue> parameters;
                for (size_t index = 0; index < instruction.args.size(); ++index) parameters.push_back(arg(index));
                result = external(instruction, parameters);
                break;
            }
            case Op::Panic: pf_panic(reference(arg(0))); assigned = false; break;
            case Op::Unwrap: result = {pf_result_value(reference(arg(0))), static_cast<uint32_t>(destinationType().kind), 0}; break;
            case Op::RawAlloc:
                result = fromReference(pf_raw_alloc(integer(arg(0)), static_cast<uint32_t>(destinationType().args.at(0).kind)), destinationType());
                break;
            case Op::RawFree: pf_raw_free(reference(arg(0))); assigned = false; break;
            case Op::RawLoad:
                result = {pf_raw_load(reference(arg(0)), integer(arg(1)), static_cast<uint32_t>(argType(0).args.at(0).kind)),
                    static_cast<uint32_t>(destinationType().kind), 0};
                break;
            case Op::RawStore:
                pf_raw_store(reference(arg(0)), integer(arg(1)), arg(2).bits, static_cast<uint32_t>(argType(0).args.at(0).kind));
                assigned = false;
                break;
            case Op::RawOffset:
                result = fromReference(pf_raw_offset(reference(arg(0)), integer(arg(1)),
                    static_cast<uint32_t>(argType(0).args.at(0).kind)), destinationType());
                break;
            case Op::CString: result = fromReference(pf_cstr(reference(arg(0))), destinationType()); break;
            case Op::Pin: result = fromReference(pf_pin(reference(arg(0))), destinationType()); break;
            case Op::Unpin: pf_unpin(reference(arg(0))); assigned = false; break;
            case Op::Zero: result = {0, static_cast<uint32_t>(destinationType().kind), 0}; break;
            case Op::ResultError:
                result = {pf_result_error(reference(arg(0))), static_cast<uint32_t>(destinationType().kind), 0};
                break;
            case Op::Spawn:
                result = fromReference(pf_task_spawn(reference(arg(0)), &Interpreter::taskEntry,
                    const_cast<Program*>(&program), static_cast<uint32_t>(destinationType().args.at(0).kind)), destinationType());
                break;
            case Op::Join:
                result = {pf_task_join(reference(arg(0))),
                    instruction.dst < 0 ? 0 : static_cast<uint32_t>(destinationType().kind), 0};
                break;
            case Op::CCallback:
                throw Error(instruction.location, "C callbacks require native compilation or JIT");
            }
            if (assigned && instruction.dst >= 0) set(instruction.dst, result);
            ++pc;
        }
        throw Error({program.sourceFile, 1, 1}, "reference interpreter has no active entry frame");
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
        const auto result = call(index, {}, nullptr);
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
