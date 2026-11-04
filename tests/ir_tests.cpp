#include "pikaflow/ir.hpp"
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using namespace pika;
Instruction instruction(Op op, int dst = -1, std::vector<int> args = {}) {
    Instruction value{op};
    value.dst = dst;
    value.args = std::move(args);
    value.location = {"ir-test", 1, 1};
    return value;
}
Program valid() {
    Program program;
    program.sourceFile = "ir-test";
    Function function;
    function.name = "main";
    function.result = I64;
    function.registers = {I64, I64, Bool};
    function.code = {instruction(Op::ConstI64, 0), instruction(Op::Return, -1, {0})};
    program.functions.push_back(function);
    return program;
}
Program recordProgram() {
    auto program = valid();
    Type point{TypeKind::Tuple, {}, "Point"};
    program.nominalLayouts["Point"] = Type{TypeKind::Tuple, {I64, I64, Bool}, "Point"};
    program.nominalRecords.insert("Point");
    Function function;
    function.name = "update_record";
    function.parameters = {I64};
    function.result = I64;
    function.registers = {I64, I64, I64, Bool, point, I64};
    function.code = {instruction(Op::ConstI64, 1), instruction(Op::ConstI64, 2),
                     instruction(Op::ConstBool, 3), instruction(Op::ConstI64, 5),
                     instruction(Op::MakeTuple, 4, {1, 2, 3}),
                     instruction(Op::SetIndex, -1, {4, 5, 2}), instruction(Op::Return, -1, {2})};
    function.code[1].integer = 42;
    function.code[2].integer = 1;
    function.code[3].integer = 1;
    program.functions.push_back(std::move(function));
    return program;
}
void rejects(const char *label, const std::function<void(Program &)> &corrupt) {
    auto program = valid();
    corrupt(program);
    try {
        verify(program);
    } catch (const Error &) {
        return;
    }
    throw std::runtime_error(std::string("IR verifier accepted ") + label);
}
} // namespace
int main() {
    try {
        verify(valid());
        verify(recordProgram());
        rejects("record without canonical layout", [](Program &p) { p.nominalRecords.insert("missing"); });
        rejects("record tag mutation", [](Program &p) {
            p = recordProgram(); p.functions[1].code[3].integer = 0;
        });
        rejects("record out-of-range field", [](Program &p) {
            p = recordProgram(); p.functions[1].code[3].integer = 3;
        });
        rejects("record field type mismatch", [](Program &p) {
            p = recordProgram(); p.functions[1].code[3].integer = 2;
        });
        rejects("dynamic record field", [](Program &p) {
            p = recordProgram(); p.functions[1].code[5].args[1] = 0;
        });
        rejects("unregistered nominal mutation", [](Program &p) {
            p = recordProgram(); p.nominalRecords.clear();
        });
        rejects("nonzero record constructor tag", [](Program &p) {
            p = recordProgram(); p.functions[1].code[0].integer = 1;
        });
        rejects("record tag supplied by parameter", [](Program &p) {
            p = recordProgram(); p.functions[1].code[4].args[0] = 0;
        });
        rejects("out-of-range destination", [](Program &p) { p.functions[0].code[0].dst = 99; });
        rejects("out-of-range operand", [](Program &p) { p.functions[0].code[1].args = {99}; });
        rejects("uninitialized operand", [](Program &p) { p.functions[0].code[1].args = {1}; });
        rejects("incorrect return type", [](Program &p) { p.functions[0].result = Bool; });
        rejects("unknown callee", [](Program &p) {
            auto &f = p.functions[0];
            f.code[0] = instruction(Op::Call, 0);
            f.code[0].text = "missing";
        });
        rejects("invalid branch target", [](Program &p) {
            auto &f = p.functions[0];
            f.code[0] = instruction(Op::Jump);
            f.code[0].target = 99;
        });
        rejects("nonboolean branch", [](Program &p) {
            auto &f = p.functions[0];
            auto branch = instruction(Op::JumpFalse, -1, {0});
            branch.target = 2;
            f.code.insert(f.code.begin() + 1, branch);
        });
        rejects("fallthrough", [](Program &p) { p.functions[0].code.pop_back(); });
        rejects("missing entry", [](Program &p) { p.entry = "absent"; });
        rejects("duplicate function", [](Program &p) { p.functions.push_back(p.functions[0]); });
        rejects("path-dependent initialization", [](Program &p) {
            auto &f = p.functions[0];
            f.code = {instruction(Op::ConstBool, 2), instruction(Op::JumpFalse, -1, {2}),
                      instruction(Op::ConstI64, 0), instruction(Op::Return, -1, {0})};
            f.code[1].target = 3;
        });
        rejects("wrong array index base", [](Program &p) {
            auto &f = p.functions[0];
            f.code.insert(f.code.begin() + 1, instruction(Op::Index, 1, {0, 0}));
        });
        for (bool branch : {false, true}) {
            rejects("parameter is not a constant tuple index", [branch](Program &p) {
                Function f;
                f.name = "tuple_reader";
                f.result = I64;
                Type tuple{TypeKind::Tuple, {I64, String}};
                f.parameters = {tuple, I64, Bool};
                f.registers = {tuple, I64, Bool, I64};
                f.code = {instruction(Op::Index, 3, {0, 1}),
                          instruction(Op::ConstI64, 1), instruction(Op::Return, -1, {3})};
                if (branch) {
                    auto jump = instruction(Op::JumpFalse, -1, {2});
                    jump.target = 3;
                    f.code.insert(f.code.begin() + 1, jump);
                }
                p.functions.push_back(std::move(f));
            });
        }
        rejects("reassigned tuple index is not constant", [](Program &p) {
            auto &f = p.functions[0];
            f.registers.push_back(Type{TypeKind::Tuple, {I64, String}});
            auto second = instruction(Op::ConstI64, 1);
            second.integer = 1;
            f.code = {instruction(Op::Zero, 3), instruction(Op::ConstI64, 1),
                      instruction(Op::Index, 0, {3, 1}), second,
                      instruction(Op::Return, -1, {0})};
        });
        rejects("malformed cell type", [](Program &p) {
            auto &f = p.functions[0];
            f.registers[0] = {TypeKind::Cell};
        });
        rejects("closure signature mismatch", [](Program &p) {
            auto &f = p.functions[0];
            f.registers[1] = Type::function({I64}, I64);
            auto closure = instruction(Op::MakeClosure, 1);
            closure.text = "main";
            f.code.insert(f.code.begin() + 1, closure);
        });
        rejects("forward closure capture destination", [](Program &p) {
            auto &main = p.functions[0];
            main.registers.push_back(Type::cell(I64));
            main.registers.push_back(Type::function({}, I64));
            main.code = {instruction(Op::ConstI64, 0), instruction(Op::MakeCell, 3, {0}),
                         instruction(Op::MakeClosure, 4, {3}), instruction(Op::Return, -1, {0})};
            main.code[2].text = "captured";
            Function target;
            target.name = "captured";
            target.result = I64;
            target.registers = {Type::cell(I64), I64};
            target.code = {instruction(Op::Capture, 99), instruction(Op::Return, -1, {1})};
            p.functions.push_back(target);
        });
        rejects("managed external argument",
                [](Program &p) { p.externals.push_back({"bad", {String}, Unit, false}); });
        rejects("duplicate external name", [](Program &p) {
            p.externals.push_back({"sqrt", {F64}, F64, false});
            p.externals.push_back(p.externals.back());
        });
        rejects("cell store destination", [](Program &p) {
            auto &f = p.functions[0];
            f.registers.push_back(Type::cell(I64));
            f.code = {instruction(Op::ConstI64, 0), instruction(Op::MakeCell, 3, {0}),
                      instruction(Op::StoreCell, 1, {3, 0}), instruction(Op::Return, -1, {1})};
        });
        rejects("array store destination", [](Program &p) {
            auto &f = p.functions[0];
            f.registers.push_back(Type::array(I64));
            f.code = {instruction(Op::ConstI64, 0), instruction(Op::MakeArray, 3, {0}),
                      instruction(Op::SetIndex, 1, {3, 0, 0}), instruction(Op::Return, -1, {1})};
        });
        rejects("jump destination", [](Program &p) {
            auto &f = p.functions[0];
            auto branch = instruction(Op::Jump, 1);
            branch.target = 2;
            f.code = {instruction(Op::ConstI64, 0), branch, instruction(Op::Return, -1, {1})};
        });
        rejects("conditional jump destination", [](Program &p) {
            auto &f = p.functions[0];
            auto branch = instruction(Op::JumpFalse, 0, {2});
            branch.target = 2;
            f.code = {instruction(Op::ConstBool, 2), branch, instruction(Op::Return, -1, {0})};
        });
        rejects("captured direct callee", [](Program &p) {
            auto &f = p.functions[0];
            auto call = instruction(Op::Call, 0);
            call.text = "captured";
            f.code = {call, instruction(Op::Return, -1, {0})};
            Function captured;
            captured.name = "captured";
            captured.result = I64;
            captured.registers = {Type::cell(I64), I64};
            auto capture = instruction(Op::Capture, 0);
            capture.integer = 0;
            captured.code = {capture, instruction(Op::LoadCell, 1, {0}),
                             instruction(Op::Return, -1, {1})};
            p.functions.push_back(std::move(captured));
        });
        rejects("unresolved generic IR type", [](Program &p) {
            p.functions[0].registers[0] = Type{TypeKind::Named};
        });
        rejects("malformed task IR type", [](Program &p) {
            p.functions[0].registers[0] = Type{TypeKind::Task};
        });
        rejects("spawn scalar operand", [](Program &p) {
            auto &f = p.functions[0];
            f.code.insert(f.code.begin() + 1, instruction(Op::Spawn, 1, {0}));
        });
        rejects("join scalar operand", [](Program &p) {
            auto &f = p.functions[0];
            f.code.insert(f.code.begin() + 1, instruction(Op::Join, 1, {0}));
        });
        rejects("callback unknown function", [](Program &p) {
            auto &f = p.functions[0];
            f.registers[1] = Type{TypeKind::Pointer, {Unit}};
            auto callback = instruction(Op::CCallback, 1);
            callback.text = "missing";
            f.code.insert(f.code.begin() + 1, callback);
        });
        rejects("nominal ADT C record", [](Program &p) {
            Type nominal{TypeKind::Tuple, {I64}};
            nominal.name = "Option[i64]";
            p.externals.push_back({"bad", {nominal}, Unit, false});
        });
        rejects("raw offset scalar operand", [](Program &p) {
            auto &f = p.functions[0];
            f.code.insert(f.code.begin() + 1, instruction(Op::RawOffset, 1, {0, 0}));
        });
        rejects("unregistered nominal reference", [](Program &p) {
            p.functions[0].registers.push_back(Type{TypeKind::Tuple, {}, "Missing"});
        });
        rejects("C ABI unsupported integer width", [](Program &p) {
            External external{"bad", {I64}, I64, false};
            external.abiParameters = {{24, true, {}}};
            p.externals.push_back(std::move(external));
        });
        rejects("C ABI descriptors mismatch parameters", [](Program &p) {
            External external{"bad", {I64}, I64, false};
            external.abiParameters = {{8, true, {}}, {16, true, {}}};
            p.externals.push_back(std::move(external));
        });
        rejects("C ABI integer descriptor on float", [](Program &p) {
            External external{"bad", {F64}, I64, false};
            external.abiParameters = {{32, true, {}}};
            p.externals.push_back(std::move(external));
        });
        rejects("C ABI aliases disagree on integer signedness", [](Program &p) {
            External left{"left", {I64}, I64, false, "shared"};
            External right{"right", {I64}, I64, false, "shared"};
            left.abiParameters = {{8, true, {}}};
            right.abiParameters = {{8, false, {}}};
            p.externals = {left, right};
        });
        {
            auto compatible = valid();
            External left{"left", {I64}, I64, false, "shared"};
            External right{"right", {I64}, I64, false, "shared"};
            right.abiParameters = {{64, false, {}}};
            right.abiResult = {64, true, {}};
            compatible.externals = {left, right};
            verify(compatible);
            if (dumpIR(compatible).find("i64 @c_u64") == std::string::npos)
                throw std::runtime_error("IR dump lost C integer wire type");
        }
        rejects("invalid nominal registry tag", [](Program &p) {
            p.nominalLayouts["Node"] = Type{TypeKind::Tuple, {Bool}, "Node"};
        });
        rejects("nominal registry identity mismatch", [](Program &p) {
            p.nominalLayouts["Node"] = Type{TypeKind::Tuple, {I64}, "Other"};
        });
        rejects("nominal use changes canonical layout", [](Program &p) {
            p.nominalLayouts["Node"] = Type{TypeKind::Tuple, {I64}, "Node"};
            p.functions[0].registers.push_back(Type{TypeKind::Tuple, {I64, Bool}, "Node"});
        });
        {
            auto deepProgram = valid();
            Type deep = I64;
            for (size_t i = 0; i < 50000; ++i) {
                Type parent{TypeKind::Array};
                parent.args.push_back(std::move(deep));
                deep = std::move(parent);
            }
            Type layout{TypeKind::Tuple};
            layout.name = "Deep";
            layout.args.push_back(I64);
            layout.args.push_back(std::move(deep));
            deepProgram.nominalLayouts.emplace("Deep", std::move(layout));
            bool rejected = false;
            try { verify(deepProgram); } catch (const Error &) { rejected = true; }
            // Dispose the deliberately invalid type without recursing in vector destructors.
            auto &field = deepProgram.nominalLayouts.at("Deep").args[1];
            while (!field.args.empty()) {
                Type next = std::move(field.args[0]);
                field.args.clear();
                field = std::move(next);
            }
            if (!rejected) throw std::runtime_error("deep nominal type bypassed IR depth limit");
        }
        {
            auto recursive = valid();
            Type node{TypeKind::Tuple, {}, "Node"};
            recursive.nominalLayouts["Node"] = Type{TypeKind::Tuple, {I64, node}, "Node"};
            auto &f = recursive.functions[0];
            f.registers.push_back(node);
            f.code = {instruction(Op::ConstI64, 0), instruction(Op::Zero, 3),
                      instruction(Op::MakeTuple, 3, {0, 3}), instruction(Op::Return, -1, {0})};
            verify(recursive);
            f.code[2].args = {0, 0};
            bool rejected = false;
            try { verify(recursive); } catch (const Error &) { rejected = true; }
            if (!rejected) throw std::runtime_error("recursive nominal field accepted wrong type");
        }
        rejects("definite assignment resource bound", [](Program &p) {
            auto &f = p.functions[0];
            f.registers.assign(8193, I64);
            f.code.assign(8193, instruction(Op::ConstI64, 0));
            f.code.back() = instruction(Op::Return, -1, {0});
        });
        std::cout << "Typed IR verification tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
