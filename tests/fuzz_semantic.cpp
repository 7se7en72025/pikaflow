#include "pikaflow/optimizer.hpp"
#include "pikaflow/runtime.h"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
constexpr size_t maximumInput = 8;
constexpr size_t maximumSource = 4096;

struct Model {
    std::string source;
    std::string expected;
};

Model generate(const uint8_t* data, size_t size) {
    auto byte = [&](size_t index) -> uint8_t { return index < size ? data[index] : 0; };
    const unsigned family = byte(0) % 5;
    // All modeled operations stay below 2^35, so the oracle never overflows.
    const int64_t a = 4294967296LL + (int64_t(byte(1)) - 128) * 1000000;
    const int64_t b = (int64_t(byte(2)) - 128) * 101;
    const int64_t c = int64_t(byte(3)) - 128;
    const unsigned count = byte(4) % 9;
    const bool flag = (byte(5) & 1) != 0;
    const unsigned index = byte(6) % 3;
    const int64_t factor = 1 + byte(7) % 3;
    std::ostringstream source, expected;
    if (family == 0) {
        const int64_t first = a + b;
        const int64_t after = a + c + b;
        expected << first << '\n' << first << '\n' << after << '\n' << first * factor << '\n' << first * 2 << '\n';
        source << "word sum_twice(i64,i64)->i64:\n    over over + rot rot + +\nfun main()->unit:\n"
               << "    var value=" << a << "\n"
               << "    let adjustment=" << b << "\n"
               << "    let first=value+adjustment\n"
               << "    let repeated=value+adjustment\n"
               << "    (" << a << " + " << b << ") * " << factor << "\n"
               << "    value += " << c << "\n"
               << "    let after=value+adjustment\n"
               << "    print(first)\n    print(repeated)\n    print(after)\n"
               << "    print((" << a << " + " << b << ") * " << factor << ")\n"
               << "    let stacked=stack(" << a << ',' << b << ")->i64:\n        sum_twice\n"
               << "    print(stacked)\n";
    } else if (family == 1) {
        int64_t result = a;
        for (unsigned i = 0; i < count; ++i) result += i % 2 == 0 ? b : -c;
        result += flag ? c : -b;
        for (unsigned i = 0; i < count; ++i) result += int64_t(i) * c;
        expected << result << '\n' << count << '\n' << (result < a ? "true" : "false") << '\n';
        source << "fun main()->unit:\n"
               << "    var result=" << a << "\n    var i=0\n"
               << "    while i < " << count << ":\n"
               << "        if i % 2 == 0:\n            result += " << b << "\n"
               << "        else:\n            result -= " << c << "\n"
               << "        i += 1\n"
               << "    if " << (flag ? "true" : "false") << ":\n"
               << "        result += " << c << "\n"
               << "    else:\n        result -= " << b << "\n"
               << "    for j in range(" << count << "):\n        result += j * " << c << "\n"
               << "    print(result)\n    print(i)\n    print(result < " << a << ")\n";
    } else if (family == 2) {
        std::array<int64_t, 3> values{{a, b, c}};
        values[index] += b;
        const int64_t first = values[index];
        values[(index + 1) % 3] = first - c;
        if (flag) values[(index + 2) % 3] *= factor;
        else values[(index + 2) % 3] -= b;
        for (unsigned i = 0; i < count; ++i) values[index] += i;
        expected << '[' << values[0] << ", " << values[1] << ", " << values[2] << "]\n"
                 << first << '\n' << values[0] + values[1] + values[2] << '\n';
        source << "fun main()->unit:\n"
               << "    let values=[" << a << ',' << b << ',' << c << "]\n    let shared=values\n"
               << "    shared[" << index << "] += " << b << "\n"
               << "    let first=values[" << index << "]\n    gc()\n"
               << "    values[" << (index + 1) % 3 << "] = first - " << c << "\n"
               << "    if " << (flag ? "true" : "false") << ":\n"
               << "        shared[" << (index + 2) % 3 << "] *= " << factor << "\n"
               << "    else:\n        shared[" << (index + 2) % 3 << "] -= " << b << "\n"
               << "    for i in range(" << count << "):\n        values[" << index << "] += i\n"
               << "    print(values)\n    print(first)\n    print(values[0]+values[1]+values[2])\n";
    } else if (family == 3) {
        const int64_t first = a + c;
        int64_t other = b;
        for (unsigned i = 0; i < count; ++i) other += int64_t(i) + c;
        const int64_t result = flag ? first * factor + other : first - other;
        expected << result << '\n' << other << '\n' << first << '\n';
        source << "type Stored[T]=Cell[T]\nrecord Cell[T]:\n    value:T\n    other:T\n"
               << "fun main()->unit:\n"
               << "    let left:Stored[i64]=Cell(other:" << b << ",value:" << a << ")\n"
               << "    let shared=left\n    shared.value += " << c << "\n"
               << "    let first=left.value\n    gc()\n"
               << "    for i in range(" << count << "):\n        left.other += i + " << c << "\n"
               << "    if " << (flag ? "true" : "false") << ":\n"
               << "        shared.value=first * " << factor << " + left.other\n"
               << "    else:\n        shared.value=first-left.other\n"
               << "    print(left.value)\n    print(shared.other)\n    print(first)\n";
    } else {
        int64_t value = a;
        expected << value << '\n' << value << '\n' << value << '\n';
        value += b;
        expected << value << '\n' << value << '\n' << value << '\n';
        for (unsigned i = 0; i < count; ++i) {
            expected << value << '\n';
            value += c + int64_t(i);
            expected << value << '\n';
        }
        value += flag ? c * factor : -c;
        expected << value << '\n' << value << '\n';
        source << "fun invoke(callback:fn(i64)->i64,amount:i64)->i64:\n    return callback(amount)\n"
               << "fun main()->unit:\n    var value=" << a << "\n"
               << "    fun read()->i64:\n        return value\n"
               << "    fun update(amount:i64)->i64:\n        value += amount\n        gc()\n        return value\n"
               << "    let callback=update\n    let first=value\n    let repeated=value\n"
               << "    let observed=read()\n    let changed=invoke(callback," << b << ")\n"
               << "    let after=value\n    gc()\n    let reloaded=value\n"
               << "    print(first)\n    print(repeated)\n    print(observed)\n"
               << "    print(changed)\n    print(after)\n    print(reloaded)\n"
               << "    for i in range(" << count << "):\n        let before=value\n"
               << "        let current=callback(" << c << "+i)\n        print(before)\n        print(current)\n"
               << "    if " << (flag ? "true" : "false") << ":\n        invoke(callback," << c * factor << ")\n"
               << "    else:\n        invoke(callback," << -c << ")\n"
               << "    print(value)\n    print(read())\n";
    }
    Model result{source.str(), expected.str()};
    if (result.source.size() > maximumSource) throw std::runtime_error("semantic generator exceeded its source budget");
    return result;
}

struct Capture {
    std::ostringstream output;
    std::streambuf* previous = std::cout.rdbuf();
    std::ios::iostate state = std::cout.rdstate();
    Capture() { std::cout.rdbuf(output.rdbuf()); }
    ~Capture() { std::cout.rdbuf(previous); std::cout.clear(state); }
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
};

struct Outcome {
    int status = -1;
    std::string output;
};

Outcome execute(const pika::Program& program) {
    pf_collect();
    const auto baseline = pf_heap_objects();
    Outcome result;
    try {
        Capture capture;
        result.status = pika::interpret(program);
        result.output = capture.output.str();
    } catch (...) {
        pf_collect();
        throw;
    }
    pf_collect();
    if (pf_heap_objects() != baseline) throw std::runtime_error("runtime roots or managed values leaked across a semantic input");
    return result;
}

bool matches(const Outcome& actual, const std::string& expected) {
    return actual.status == 0 && actual.output == expected;
}

[[noreturn]] void failure(const Model& model, const char* phase, const std::string& detail) {
    std::cerr << "semantic fuzzer failure in " << phase << ": " << detail
              << "\n--- source ---\n" << model.source << "--- expected full output ---\n"
              << model.expected;
    std::abort();
}

void check(const Model& model, const pika::Program& program, const char* phase) {
    const auto actual = execute(program);
    if (!matches(actual, model.expected))
        failure(model, phase, "status=" + std::to_string(actual.status) + "\n--- actual full output ---\n" + actual.output);
}

void exercise(const Model& model) {
    try {
        const auto syntax = pika::parse(pika::lex(model.source, "<semantic-fuzz>"), "<semantic-fuzz>");
        const auto original = pika::compile(syntax);
        pika::verify(original);
        check(model, original, "original");
        const auto optimized = pika::optimizeIR(original);
        pika::verify(optimized);
        check(model, optimized, "optimized");
        const auto repeated = pika::optimizeIR(optimized);
        pika::verify(repeated);
        check(model, repeated, "optimized twice");
    } catch (const std::exception& error) {
        failure(model, "unexpected exception", error.what());
    }
}

void selfCheck() {
    const std::array<std::string, 5> zero{{
        "4294967296\n4294967296\n4294967296\n4294967296\n8589934592\n",
        "4294967296\n0\nfalse\n",
        "[4294967296, 4294967296, 0]\n4294967296\n8589934592\n",
        "4294967296\n0\n4294967296\n",
        "4294967296\n4294967296\n4294967296\n4294967296\n4294967296\n4294967296\n4294967296\n4294967296\n"}};
    const std::array<std::string, 5> branch{{
        "4295967397\n4295967397\n4295967399\n12887902191\n8591934794\n",
        "4295967399\n2\nfalse\n",
        "[12887901888, 203, 200]\n202\n12887902291\n",
        "12887902000\n106\n4295967298\n",
        "4295967296\n4295967296\n4295967296\n4295967397\n4295967397\n4295967397\n"
        "4295967397\n4295967399\n4295967399\n4295967402\n4295967408\n4295967408\n"}};
    for (uint8_t family = 0; family < 5; ++family) {
        std::array<uint8_t, 8> data{{family,128,128,128,0,0,0,0}};
        auto model = generate(data.data(), data.size());
        if (model.expected != zero[family]) failure(model, "zero golden oracle", zero[family]);
        exercise(model);
        data = {{family,129,129,130,2,1,1,2}};
        model = generate(data.data(), data.size());
        if (model.expected != branch[family]) failure(model, "branch golden oracle", branch[family]);
        exercise(model);
    }
    const std::array<uint8_t, 8> less{{1,128,127,129,3,0,0,0}};
    const auto model = generate(less.data(), less.size());
    if (model.expected != "4294967197\n3\ntrue\n") failure(model, "comparison golden oracle", "wrong independent loop expectation");
    exercise(model);
    const std::array<uint8_t, 8> closureNegative{{4,128,127,129,3,0,0,0}};
    const auto closure = generate(closureNegative.data(), closureNegative.size());
    const std::string closureExpected = "4294967296\n4294967296\n4294967296\n"
        "4294967195\n4294967195\n4294967195\n4294967195\n4294967196\n"
        "4294967196\n4294967198\n4294967198\n4294967201\n4294967200\n4294967200\n";
    if (closure.expected != closureExpected) failure(closure, "closure golden oracle", closureExpected);
    exercise(closure);

    const std::string source = "fun main()->unit:\n    print(4294967296)\n";
    auto valid = pika::compile(pika::parse(pika::lex(source, "<oracle-check>"), "<oracle-check>"));
    const auto expected = execute(valid);
    if (!matches(expected, "4294967296\n")) throw std::runtime_error("full-width output self-check failed");
    bool changed = false;
    for (auto& function : valid.functions) for (auto& instruction : function.code)
        if (instruction.op == pika::Op::ConstI64 && instruction.integer == 4294967296LL) {
            instruction.integer = 0;
            changed = true;
        }
    pika::verify(valid);
    const auto mutated = execute(valid);
    if (!changed || mutated.status != expected.status || matches(mutated, "4294967296\n"))
        throw std::runtime_error("full-output oracle failed to reject a valid IR mutation with equal unit status");

    const std::array<uint8_t, 8> arithmetic{{0,128,128,128,0,0,0,0}};
    const auto generated = generate(arithmetic.data(), arithmetic.size());
    const auto original = pika::compile(pika::parse(pika::lex(generated.source, "<pass-check>"), "<pass-check>"));
    pika::OptimizationStats statistics;
    (void)pika::optimizeIR(original, &statistics);
    if (!statistics.constantsFolded || !statistics.deadResultsRemoved || !statistics.commonExpressionsReused ||
            !statistics.cellLoadsForwarded)
        throw std::runtime_error("semantic smoke fixture did not exercise constant folding, CSE, dead-result removal and cell forwarding");
}
}

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    try { selfCheck(); }
    catch (const std::exception& error) {
        std::cerr << "semantic fuzzer initialization failed: " << error.what() << '\n';
        std::abort();
    }
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > maximumInput) return 0;
    exercise(generate(data, size));
    return 0;
}
