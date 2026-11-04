#include "pikaflow/runtime.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

namespace {
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(2); }
}
uint64_t bits(void* pointer) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pointer)); }
uint64_t floatBits(double number) { uint64_t result; std::memcpy(&result, &number, sizeof(result)); return result; }
void testArithmetic() {
    const auto low = std::numeric_limits<int64_t>::min(), high = std::numeric_limits<int64_t>::max();
    require(pf_checked_add(high, -1) == high - 1, "addition near upper boundary");
    require(pf_checked_add(low, 0) == low, "addition at lower boundary");
    require(pf_checked_sub(low, -1) == low + 1, "subtraction near lower boundary");
    require(pf_checked_sub(-1, high) == low, "subtraction reaching lower boundary");
    require(pf_checked_mul(low, 1) == low, "multiplication at lower boundary");
    require(pf_checked_mul(-3037000499LL, -3037000499LL) == 9223372030926249001LL, "negative multiplication near upper boundary");
    require(pf_checked_mul(high, 0) == 0, "multiplication by zero");
    require(pf_checked_div(-7, 3) == -2, "division truncates toward zero");
    require(pf_checked_mod(-7, 3) == -1, "remainder follows numerator sign");
    require(pf_checked_neg(high) == -high, "valid negation");
    require(pf_float_to_int(-3.9) == -3, "float conversion truncates toward zero");
    require(pf_float_to_int(-9223372036854775808.0) == low, "float conversion permits lower endpoint");
}
void testHeap() {
    pf_collect();
    require(pf_heap_objects() == 0, "fresh heap empty");
    void* roots[4]{};
    auto* frame = pf_roots_push(roots, 4);
    roots[0] = pf_string("hello", 5);
    roots[1] = pf_array_new(2, 4);
    pf_set_index(roots[1], 0, bits(roots[0]), 4);
    pf_set_index(roots[1], 1, bits(roots[0]), 4);
    require(pf_length(roots[0]) == 5 && pf_index(roots[0], 1) == 'e', "string length and byte indexing");
    roots[0] = nullptr;
    pf_collect();
    require(pf_heap_objects() == 2, "array keeps referenced string alive");
    require(pf_index_tag(roots[1], 0) == 4, "array element tag");
    auto* retained = reinterpret_cast<void*>(static_cast<uintptr_t>(pf_index(roots[1], 0)));
    require(pf_length(retained) == 5, "traced string remains usable");
    roots[2] = pf_cell_new(42, 1);
    roots[3] = pf_closure_new(reinterpret_cast<void*>(uintptr_t(1)), 1);
    pf_closure_set(roots[3], 0, roots[2]);
    roots[2] = nullptr;
    pf_collect();
    pf_cell_set(pf_closure_get(roots[3], 0), 43, 1);
    require(pf_cell_get(pf_closure_get(roots[3], 0)) == 43, "closure keeps mutable capture alive");
    require(pf_closure_code(roots[3]) == reinterpret_cast<void*>(uintptr_t(1)), "closure retains implementation address");
    roots[0] = pf_tuple_new(2);
    pf_set_index(roots[0], 0, bits(roots[0]), 6);
    pf_set_index(roots[0], 1, bits(roots[3]), 7);
    roots[3] = nullptr;
    pf_collect();
    require(pf_heap_objects() == 5, "cycle and nested closure reachable");
    roots[0] = nullptr;
    roots[1] = nullptr;
    pf_collect();
    require(pf_heap_objects() == 0, "unreachable cycles collected");
    roots[0] = pf_string("\xC3\xA9", 2);
    require(pf_length(roots[0]) == 2 && pf_index(roots[0], 0) == 0xC3, "UTF8 indexes are unsigned bytes");
    roots[1] = pf_string("\xC3\xA9", 2);
    require(pf_string_equal(roots[0], roots[1]) == 1, "string equality compares contents");
    auto* inner = pf_roots_push(nullptr, 0);
    pf_roots_pop(inner);
    for (int index = 0; index < 200; ++index) pf_string("temporary", 9);
    require(pf_length(roots[0]) == 2, "automatic collections preserve caller roots");
    pf_roots_pop(frame);
    pf_collect();
    require(pf_heap_objects() == 0, "popped frames no longer retain objects");
}
void testPrinting() {
    void* roots[2]{};
    auto* frame = pf_roots_push(roots, 2);
    roots[0] = pf_tuple_new(2);
    pf_set_index(roots[0], 0, 42, 1);
    pf_set_index(roots[0], 1, 1, 3);
    roots[1] = pf_tuple_new(1);
    pf_set_index(roots[1], 0, bits(roots[1]), 6);
    std::ostringstream output;
    auto* original = std::cout.rdbuf(output.rdbuf());
    pf_print(42, 1);
    pf_print(floatBits(2.5), 2);
    pf_print(1, 3);
    pf_print(bits(roots[0]), 6);
    pf_print(bits(roots[1]), 6);
    std::cout.rdbuf(original);
    require(output.str() == "42\n2.5\ntrue\n(42, true)\n(<cycle>,)\n", "stable scalar, tuple and cyclic printing");
    pf_roots_pop(frame);
    pf_collect();
}
void testResults() {
    void* roots[3]{};
    auto* frame = pf_roots_push(roots, 3);
    roots[0] = pf_tuple_new(3);
    roots[1] = pf_string("success", 7);
    pf_set_index(roots[0], 0, 1, 3);
    pf_set_index(roots[0], 1, bits(roots[1]), 4);
    pf_set_index(roots[0], 2, 0, 4);
    roots[2] = pf_cell_new(bits(roots[0]), 10);
    roots[0] = roots[1] = nullptr;
    pf_collect();
    auto* result = reinterpret_cast<void*>(static_cast<uintptr_t>(pf_cell_get(roots[2])));
    auto* returned = reinterpret_cast<void*>(static_cast<uintptr_t>(pf_result_value(result)));
    require(pf_length(returned) == 7, "Result tag traces its tuple and returned managed value");
    pf_roots_pop(frame);
    pf_collect();
    require(pf_heap_objects() == 0, "Result graph is reclaimed");
}
}

int main(int argc, char** argv) {
    const auto low = std::numeric_limits<int64_t>::min(), high = std::numeric_limits<int64_t>::max();
    if (argc > 1) {
        const std::string name = argv[1];
        if (name == "add-overflow") pf_checked_add(high, 1);
        else if (name == "sub-overflow") pf_checked_sub(low, 1);
        else if (name == "mul-overflow") pf_checked_mul(low, -1);
        else if (name == "div-zero") pf_checked_div(1, 0);
        else if (name == "div-overflow") pf_checked_div(low, -1);
        else if (name == "mod-overflow") pf_checked_mod(low, -1);
        else if (name == "neg-overflow") pf_checked_neg(low);
        else if (name == "float-overflow") pf_float_to_int(9223372036854775808.0);
        else if (name == "float-nan") pf_float_to_int(std::numeric_limits<double>::quiet_NaN());
        else if (name == "bounds") pf_index(pf_array_new(1, 1), 1);
        else if (name == "negative-index") pf_index(pf_array_new(1, 1), -1);
        else if (name == "type-mismatch") pf_set_index(pf_array_new(1, 1), 0, 1, 3);
        else if (name == "negative-size") pf_array_new(-1, 1);
        else if (name == "invalid-reference") pf_length(reinterpret_cast<void*>(uintptr_t(1)));
        else if (name == "root-order") { auto* frame = pf_roots_push(nullptr, 0); pf_roots_push(nullptr, 0); pf_roots_pop(frame); }
        else if (name == "panic") pf_panic(pf_string("explicit panic", 14));
        else if (name == "result-failure") {
            void* roots[2]{};
            pf_roots_push(roots, 2);
            roots[0] = pf_tuple_new(3);
            roots[1] = pf_string("operation failed", 16);
            pf_set_index(roots[0], 0, 0, 3);
            pf_set_index(roots[0], 1, 0, 1);
            pf_set_index(roots[0], 2, bits(roots[1]), 4);
            pf_result_value(roots[0]);
        }
        else { std::cerr << "unknown error case\n"; return 2; }
        std::cerr << "error case unexpectedly succeeded\n";
        return 2;
    }
    testArithmetic();
    testHeap();
    testPrinting();
    testResults();
    std::cout << "runtime tests passed\n";
}
