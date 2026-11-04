#include "pikaflow/optimizer.hpp"
#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <numeric>
#include <tuple>

namespace pika {
namespace {
enum class Knowledge { Unseen, Constant, Varying };
struct Fact {
    Knowledge knowledge = Knowledge::Unseen;
    int64_t value = 0;
    static Fact constant(int64_t value) { return {Knowledge::Constant, value}; }
    static Fact varying() { return {Knowledge::Varying, 0}; }
};
using State = std::vector<Fact>;
struct Block {
    size_t begin, end;
    State incoming;
    bool reachable = false, queued = false;
};

bool join(Fact& destination, const Fact& source) {
    if(source.knowledge == Knowledge::Unseen || destination.knowledge == Knowledge::Varying)
        return false;
    if(destination.knowledge == Knowledge::Unseen) { destination = source; return true; }
    if(source.knowledge == Knowledge::Varying || destination.value != source.value) {
        destination = Fact::varying(); return true;
    }
    return false;
}

bool checkedBinary(Op op, int64_t a, int64_t b, int64_t& result) {
    constexpr auto minimum = std::numeric_limits<int64_t>::min();
    constexpr auto maximum = std::numeric_limits<int64_t>::max();
    switch(op) {
    case Op::Add:
        if((b > 0 && a > maximum - b) || (b < 0 && a < minimum - b)) return false;
        result = a + b; return true;
    case Op::Sub:
        if((b > 0 && a < minimum + b) || (b < 0 && a > maximum + b)) return false;
        result = a - b; return true;
    case Op::Mul: {
        const bool negative = (a < 0) != (b < 0);
        const auto magnitudeA = a < 0 ? uint64_t(0) - uint64_t(a) : uint64_t(a);
        const auto magnitudeB = b < 0 ? uint64_t(0) - uint64_t(b) : uint64_t(b);
        const auto limit = negative ? uint64_t(1) << 63 : uint64_t(maximum);
        if(magnitudeA && magnitudeB > limit / magnitudeA) return false;
        const auto product = magnitudeA * magnitudeB;
        result = negative ? (product == (uint64_t(1) << 63) ? minimum : -int64_t(product))
                          : int64_t(product);
        return true;
    }
    case Op::Div: case Op::Mod:
        if(!b || (a == minimum && b == -1)) return false;
        result = op == Op::Div ? a / b : a % b; return true;
    default: return false;
    }
}

Fact evaluate(const Function& function, const Instruction& ins, const State& state) {
    if(ins.dst < 0) return Fact::varying();
    const auto& destination = function.registers[size_t(ins.dst)];
    if(destination != I64 && destination != Bool) return Fact::varying();
    if(ins.op == Op::ConstI64) return Fact::constant(ins.integer);
    if(ins.op == Op::ConstBool) return Fact::constant(ins.integer != 0);
    if(ins.op == Op::Zero) return Fact::constant(0);
    if(ins.op == Op::Move) return state[size_t(ins.args[0])];
    bool supported = ins.op == Op::Neg || ins.op == Op::Not ||
        (ins.op >= Op::Add && ins.op <= Op::Mod) || (ins.op >= Op::Eq && ins.op <= Op::Ge);
    if(!supported) return Fact::varying();
    for(int argument : ins.args)
        if(function.registers[size_t(argument)] != I64 && function.registers[size_t(argument)] != Bool)
            return Fact::varying();
    bool unseen = false;
    for(int argument : ins.args) {
        auto knowledge = state[size_t(argument)].knowledge;
        if(knowledge == Knowledge::Varying) return Fact::varying();
        unseen |= knowledge == Knowledge::Unseen;
    }
    if(unseen) return {};
    const auto a = state[size_t(ins.args[0])].value;
    if(ins.op == Op::Not) return Fact::constant(!a);
    if(ins.op == Op::Neg)
        return a == std::numeric_limits<int64_t>::min() ? Fact::varying() : Fact::constant(-a);
    const auto b = state[size_t(ins.args[1])].value;
    switch(ins.op) {
    case Op::Eq: return Fact::constant(a == b);
    case Op::Ne: return Fact::constant(a != b);
    case Op::Lt: return Fact::constant(a < b);
    case Op::Le: return Fact::constant(a <= b);
    case Op::Gt: return Fact::constant(a > b);
    case Op::Ge: return Fact::constant(a >= b);
    default: {
        int64_t result;
        return checkedBinary(ins.op, a, b, result) ? Fact::constant(result) : Fact::varying();
    }
    }
}

void transfer(const Function& function, const Instruction& ins, State& state) {
    if(ins.dst >= 0) state[size_t(ins.dst)] = evaluate(function, ins, state);
}

std::vector<bool> blockLeaders(const Function& function) {
    const auto count = function.code.size();
    std::vector<bool> leaders(count, false);
    leaders[0] = true;
    for(size_t pc = 0; pc < count; ++pc) {
        const auto op = function.code[pc].op;
        if(op == Op::Jump || op == Op::JumpFalse) leaders[size_t(function.code[pc].target)] = true;
        if((op == Op::Jump || op == Op::JumpFalse || op == Op::Return || op == Op::Panic) && pc + 1 < count)
            leaders[pc + 1] = true;
    }
    return leaders;
}

bool scalarExpression(const Function& function, const Instruction& ins) {
    switch(ins.op) {
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: case Op::Neg:
        return function.registers[size_t(ins.dst)] == I64;
    case Op::Not: return true;
    case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: {
        const auto& operand = function.registers[size_t(ins.args[0])];
        return operand == I64 || operand == Bool;
    }
    default: return false;
    }
}

void reuseExpressions(Function& function, OptimizationStats& statistics) {
    const auto leaders = blockLeaders(function);
    std::vector<size_t> values(function.registers.size());
    using Key = std::tuple<Op, size_t, size_t>;
    struct Available { int reg; size_t value; };
    std::map<Key, Available> expressions;
    size_t next = values.size();
    for(size_t pc = 0; pc < function.code.size(); ++pc) {
        if(leaders[pc]) {
            expressions.clear();
            // Incoming registers are independent on every edge, even after a scanned Move.
            std::iota(values.begin(), values.end(), size_t(0));
            next = values.size();
        }
        auto& ins = function.code[pc];
        if(ins.dst < 0) continue;
        const auto destination = size_t(ins.dst);
        if(scalarExpression(function, ins)) {
            const Key key{ins.op, values[size_t(ins.args[0])],
                          ins.args.size() == 2 ? values[size_t(ins.args[1])] : 0};
            auto found = expressions.find(key);
            if(found != expressions.end() && values[size_t(found->second.reg)] == found->second.value) {
                const auto representative = found->second.reg;
                ins.op = Op::Move;
                ins.args = {representative};
                ins.text.clear(); ins.integer = 0; ins.real = 0; ins.target = -1;
                values[destination] = found->second.value;
                ++statistics.commonExpressionsReused;
            } else {
                values[destination] = next++;
                if(expressions.size() >= 4096) expressions.clear();
                expressions[key] = {ins.dst, values[destination]};
            }
        } else if(ins.op == Op::Move &&
                  (function.registers[destination] == I64 || function.registers[destination] == Bool)) {
            values[destination] = values[size_t(ins.args[0])];
        } else values[destination] = next++;
    }
}

bool discardable(const Function& function, const Instruction& ins) {
    if(ins.dst < 0 || function.localNames.count(ins.dst)) return false;
    const auto& type = function.registers[size_t(ins.dst)];
    if(type != I64 && type != Bool) return false;
    switch(ins.op) {
    case Op::ConstI64: case Op::ConstBool: case Op::Zero: case Op::Move: case Op::Not:
        return true;
    case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
        return scalarExpression(function, ins);
    default: return false;
    }
}

bool removeDeadResults(Function& function, OptimizationStats& statistics) {
    constexpr size_t maximumFacts = 1000000, maximumWork = 8000000;
    const auto registers = function.registers.size(), count = function.code.size();
    const auto leaders = blockLeaders(function);
    struct LiveBlock {
        size_t begin, end;
        std::vector<size_t> successors, predecessors;
        std::vector<unsigned char> incoming;
        bool queued = true;
    };
    std::vector<LiveBlock> blocks;
    std::vector<size_t> containing(count);
    for(size_t pc = 0; pc < count; ++pc) {
        if(leaders[pc]) {
            if(!blocks.empty()) blocks.back().end = pc;
            blocks.push_back({pc, count, {}, {}, {}, true});
        }
        containing[pc] = blocks.size() - 1;
    }
    if(registers && blocks.size() > maximumFacts / registers) return false;
    std::deque<size_t> queue;
    for(size_t index = blocks.size(); index-- > 0;) {
        auto& block = blocks[index];
        block.incoming.resize(registers);
        const auto& last = function.code[block.end - 1];
        if(last.op == Op::Jump || last.op == Op::JumpFalse)
            block.successors.push_back(containing[size_t(last.target)]);
        if(last.op != Op::Jump && last.op != Op::Return && last.op != Op::Panic && block.end < count)
            block.successors.push_back(containing[block.end]);
        for(auto successor : block.successors) blocks[successor].predecessors.push_back(index);
        queue.push_back(index);
    }
    size_t work = 0;
    auto spend = [&](size_t amount) {
        if(amount > maximumWork - work) return false;
        work += amount;
        return true;
    };
    auto outgoing = [&](const LiveBlock& block, std::vector<unsigned char>& live) {
        if(!spend(registers)) return false;
        live.assign(registers, 0);
        for(auto successor : block.successors) {
            if(!spend(registers)) return false;
            for(size_t reg = 0; reg < registers; ++reg) live[reg] |= blocks[successor].incoming[reg];
        }
        return true;
    };
    auto reverse = [&](const LiveBlock& block, std::vector<unsigned char>& live,
                       std::vector<bool>* removed) {
        for(size_t pc = block.end; pc-- > block.begin;) {
            const auto& ins = function.code[pc];
            if(!spend(1)) return false;
            if(ins.dst >= 0 && !live[size_t(ins.dst)] && discardable(function, ins)) {
                if(removed) (*removed)[pc] = true;
                continue;
            }
            if(ins.dst >= 0) live[size_t(ins.dst)] = 0;
            if(!spend(ins.args.size())) return false;
            for(int argument : ins.args) live[size_t(argument)] = 1;
        }
        return true;
    };
    while(!queue.empty()) {
        const auto index = queue.front(); queue.pop_front();
        auto& block = blocks[index]; block.queued = false;
        std::vector<unsigned char> live;
        if(!outgoing(block, live) || !reverse(block, live, nullptr) || !spend(registers)) return false;
        if(live != block.incoming) {
            block.incoming = std::move(live);
            for(auto predecessor : block.predecessors) if(!blocks[predecessor].queued) {
                blocks[predecessor].queued = true;
                queue.push_back(predecessor);
            }
        }
    }
    std::vector<bool> removed(count);
    for(const auto& block : blocks) {
        std::vector<unsigned char> live;
        if(!outgoing(block, live) || !reverse(block, live, &removed)) return false;
    }
    std::vector<int> relocated(count);
    std::vector<Instruction> retained;
    for(size_t pc = 0; pc < count; ++pc) {
        relocated[pc] = int(retained.size());
        if(!removed[pc]) retained.push_back(std::move(function.code[pc]));
    }
    for(auto& ins : retained)
        if(ins.op == Op::Jump || ins.op == Op::JumpFalse) ins.target = relocated[size_t(ins.target)];
    statistics.deadResultsRemoved += count - retained.size();
    function.code = std::move(retained);
    return true;
}

bool optimizeFunction(Function& function, OptimizationStats& statistics) {
    constexpr size_t maximumFacts = 1000000, maximumWork = 8000000;
    const auto count = function.code.size(), registers = function.registers.size();
    const auto leaders = blockLeaders(function);
    std::vector<Block> blocks;
    std::vector<size_t> containing(count);
    for(size_t pc = 0; pc < count; ++pc) {
        if(leaders[pc]) {
            if(!blocks.empty()) blocks.back().end = pc;
            blocks.push_back({pc, count, {}, false, false});
        }
        containing[pc] = blocks.size() - 1;
    }
    if(registers && blocks.size() > maximumFacts / registers) return false;
    for(auto& block : blocks) block.incoming.resize(registers);
    for(size_t n = 0; n < function.parameters.size(); ++n) blocks[0].incoming[n] = Fact::varying();
    blocks[0].reachable = blocks[0].queued = true;
    std::deque<size_t> queue{0};
    size_t work = 0;
    auto propagate = [&](size_t next, const State& state) {
        auto& block = blocks[containing[next]];
        bool changed = !block.reachable;
        if(!block.reachable) block.incoming = state;
        else for(size_t reg = 0; reg < registers; ++reg) changed |= join(block.incoming[reg], state[reg]);
        block.reachable = true;
        if(changed && !block.queued) { block.queued = true; queue.push_back(containing[next]); }
        work += registers;
    };
    while(!queue.empty()) {
        auto index = queue.front(); queue.pop_front();
        auto& block = blocks[index]; block.queued = false;
        State state = block.incoming;
        work += registers + block.end - block.begin;
        if(work > maximumWork) return false;
        for(size_t pc = block.begin; pc < block.end; ++pc) transfer(function, function.code[pc], state);
        const auto& last = function.code[block.end - 1];
        if(last.op == Op::Return || last.op == Op::Panic) continue;
        if(last.op == Op::Jump) propagate(size_t(last.target), state);
        else if(last.op == Op::JumpFalse) {
            const auto condition = state[size_t(last.args[0])];
            if(condition.knowledge == Knowledge::Constant)
                propagate(condition.value ? block.end : size_t(last.target), state);
            else if(condition.knowledge == Knowledge::Varying) {
                propagate(size_t(last.target), state);
                if(block.end < count) propagate(block.end, state);
            }
        } else if(block.end < count) propagate(block.end, state);
    }
    if(work > maximumWork) return false;
    // An incomplete fact must never make executable code disappear.
    for(const auto& block : blocks) if(block.reachable) {
        const auto& last = function.code[block.end - 1];
        if(last.op == Op::JumpFalse) {
            auto state = block.incoming;
            for(size_t pc = block.begin; pc < block.end; ++pc) transfer(function, function.code[pc], state);
            if(state[size_t(last.args[0])].knowledge == Knowledge::Unseen) return false;
        }
    }
    for(const auto& block : blocks) if(block.reachable) {
        State state = block.incoming;
        for(size_t pc = block.begin; pc < block.end; ++pc) {
            auto& ins = function.code[pc];
            if(ins.op == Op::JumpFalse && state[size_t(ins.args[0])].knowledge == Knowledge::Constant) {
                ins.target = state[size_t(ins.args[0])].value ? int(pc + 1) : ins.target;
                ins.op = Op::Jump; ins.args.clear(); ++statistics.branchesFolded;
            }
            if(ins.dst >= 0) {
                const auto value = evaluate(function, ins, state);
                state[size_t(ins.dst)] = value;
                if(value.knowledge == Knowledge::Constant && ins.op != Op::ConstI64 && ins.op != Op::ConstBool) {
                    ins.op = function.registers[size_t(ins.dst)] == Bool ? Op::ConstBool : Op::ConstI64;
                    ins.args.clear(); ins.text.clear(); ins.integer = value.value; ins.real = 0; ins.target = -1;
                    ++statistics.constantsFolded;
                }
            }
        }
    }
    std::vector<bool> reachable(count, false);
    std::vector<size_t> pending{0}; reachable[0] = true;
    auto visit = [&](size_t pc) { if(!reachable[pc]) { reachable[pc] = true; pending.push_back(pc); } };
    while(!pending.empty()) {
        auto pc = pending.back(); pending.pop_back();
        const auto& ins = function.code[pc];
        if(ins.op == Op::Return || ins.op == Op::Panic) continue;
        if(ins.op == Op::Jump || ins.op == Op::JumpFalse) visit(size_t(ins.target));
        if(ins.op != Op::Jump && pc + 1 < count) visit(pc + 1);
    }
    std::vector<int> relocated(count, -1);
    std::vector<Instruction> retained;
    for(size_t pc = 0; pc < count; ++pc) if(reachable[pc]) {
        relocated[pc] = int(retained.size()); retained.push_back(std::move(function.code[pc]));
    }
    for(auto& ins : retained)
        if(ins.op == Op::Jump || ins.op == Op::JumpFalse) ins.target = relocated[size_t(ins.target)];
    statistics.unreachableRemoved += count - retained.size();
    function.code = std::move(retained);
    return true;
}
} // namespace

Program optimizeIR(const Program& program, OptimizationStats* result) {
    verify(program);
    Program optimized = program;
    OptimizationStats statistics;
    for(auto& function : optimized.functions) {
        if(!optimizeFunction(function, statistics)) { ++statistics.functionsSkipped; continue; }
        reuseExpressions(function, statistics);
        if(!removeDeadResults(function, statistics)) ++statistics.livenessSkipped;
    }
    verify(optimized);
    if(result) *result = statistics;
    return optimized;
}
} // namespace pika
