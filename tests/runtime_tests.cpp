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
    require(pf_c_integer(static_cast<uint64_t>(int64_t(-128)), 8, 1) == static_cast<uint64_t>(int64_t(-128)),
        "signed C integer lower endpoint preserves carrier bits");
    require(pf_c_integer(127, 8, 1) == 127 && pf_c_integer(32767, 16, 1) == 32767 &&
        pf_c_integer(2147483647, 32, 1) == 2147483647, "signed C integer upper endpoints");
    require(pf_c_integer(255, 8, 0) == 255 && pf_c_integer(65535, 16, 0) == 65535 &&
        pf_c_integer(4294967295ULL, 32, 0) == 4294967295ULL, "unsigned C integer upper endpoints");
    require(pf_c_integer(std::numeric_limits<uint64_t>::max(), 64, 0) == std::numeric_limits<uint64_t>::max() &&
        pf_c_integer(static_cast<uint64_t>(low), 64, 1) == static_cast<uint64_t>(low),
        "64-bit C carriers preserve the complete unsigned and signed bit ranges");
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
    auto* address = static_cast<uint64_t*>(pf_cell_address(roots[2]));
    require(*address == 42, "debugger cell address points to its actual value storage");
    roots[3] = pf_closure_new(reinterpret_cast<void*>(uintptr_t(1)), 1);
    pf_closure_set(roots[3], 0, roots[2]);
    roots[2] = nullptr;
    pf_collect();
    pf_cell_set(pf_closure_get(roots[3], 0), 43, 1);
    require(pf_cell_get(pf_closure_get(roots[3], 0)) == 43, "closure keeps mutable capture alive");
    require(pf_cell_address(pf_closure_get(roots[3], 0)) == address && *address == 43,
        "debugger cell address remains stable through collection and observes updates");
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
    roots[0] = pf_tuple_new(3);
    pf_set_index(roots[0], 0, 0, 3);
    pf_set_index(roots[0], 1, 0, 4);
    pf_set_index(roots[0], 2, 404, 1);
    require(pf_result_error(roots[0]) == 404, "Result errors support non-string payloads");
    pf_roots_pop(frame);
    pf_collect();
    require(pf_heap_objects() == 0, "Result graph is reclaimed");
}
void testRawMemoryAndPins() {
    auto* integers = pf_raw_alloc(2, 1);
    require(pf_raw_load(integers, 0, 1) == 0, "owned raw storage is zero initialized");
    pf_raw_store(integers, 1, 42, 1);
    require(pf_raw_load(integers, 1, 1) == 42, "raw integer round trip");
    auto* interior = static_cast<unsigned char*>(integers) + sizeof(int64_t);
    require(pf_raw_load(interior, 0, 1) == 42, "known interior pointers retain bounds and type validation");
    require(pf_raw_offset(integers, 1, 1) == interior, "typed pointer offset addresses owned elements");
    auto* end = pf_raw_offset(integers, 2, 1);
    require(pf_raw_offset(end, -2, 1) == integers, "one-past owned pointer can retreat to its allocation");
    pf_raw_free(integers);
    auto* floats = pf_raw_alloc(1, 2);
    pf_raw_store(floats, 0, floatBits(1.5), 2);
    require(pf_raw_load(floats, 0, 2) == floatBits(1.5), "raw float bit representation preserved");
    pf_raw_free(floats);
    auto* booleans = pf_raw_alloc(3, 3);
    pf_raw_store(booleans, 1, 1, 3);
    require(pf_raw_load(booleans, 0, 3) == 0 && pf_raw_load(booleans, 1, 3) == 1 && pf_raw_load(booleans, 2, 3) == 0,
        "raw boolean stores address one byte without corrupting neighbors");
    pf_raw_free(booleans);
    int64_t foreign[2] = {7, 9};
    require(pf_raw_load(foreign, 1, 1) == 9, "valid foreign native memory can be read explicitly");
    pf_raw_store(foreign, 0, 12, 1);
    require(foreign[0] == 12, "valid foreign native memory can be written explicitly");
    require(pf_raw_offset(foreign + 1, -1, 1) == foreign, "foreign typed pointers permit signed element offsets");
    const char foreignString[] = "abc";
    require(pf_raw_offset(const_cast<char*>(foreignString), 1, 0) == foreignString + 1,
        "opaque foreign pointers use byte offsets");
    auto* pointers = pf_raw_alloc(1, 9);
    pf_raw_store(pointers, 0, bits(foreign), 9);
    require(pf_raw_load(pointers, 0, 9) == bits(foreign), "raw native pointer width round trip");
    pf_raw_free(pointers);
    pf_raw_free(pf_raw_alloc(0, 1));

    void* roots[1]{};
    auto* frame = pf_roots_push(roots, 1);
    roots[0] = pf_string("A\0B", 3);
    const auto* bytes = static_cast<const char*>(pf_cstr(roots[0]));
    auto* first = pf_pin(roots[0]);
    auto* second = pf_pin(roots[0]);
    require(first != second && first != roots[0], "persistent root handles have independent identity");
    roots[0] = nullptr;
    pf_collect();
    require(pf_heap_objects() == 1 && bytes[0] == 'A' && bytes[1] == '\0' && bytes[2] == 'B' && bytes[3] == '\0',
        "pinned C string survives collection and preserves embedded NUL bytes");
    pf_unpin(first);
    pf_collect();
    require(pf_heap_objects() == 1, "remaining pin retains the managed object");
    pf_unpin(second);
    pf_collect();
    require(pf_heap_objects() == 0, "released final pin permits collection");
    pf_roots_pop(frame);
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
        else if (name == "c-int-signed-high") pf_c_integer(128, 8, 1);
        else if (name == "c-int-signed-low") pf_c_integer(static_cast<uint64_t>(int64_t(-129)), 8, 1);
        else if (name == "c-int-unsigned-high") pf_c_integer(256, 8, 0);
        else if (name == "c-int-unsigned-negative") pf_c_integer(static_cast<uint64_t>(int64_t(-1)), 32, 0);
        else if (name == "c-int-width") pf_c_integer(0, 7, 1);
        else if (name == "c-int-signedness") pf_c_integer(0, 8, 2);
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
        else if (name == "raw-bounds") pf_raw_load(pf_raw_alloc(1, 1), 1, 1);
        else if (name == "raw-interior-bounds") {
            auto* memory = static_cast<unsigned char*>(pf_raw_alloc(2, 1));
            pf_raw_load(memory + sizeof(int64_t), 1, 1);
        }
        else if (name == "raw-double-free") { auto* pointer = pf_raw_alloc(1, 1); pf_raw_free(pointer); pf_raw_free(pointer); }
        else if (name == "raw-type") pf_raw_load(pf_raw_alloc(1, 1), 0, 2);
        else if (name == "raw-negative") pf_raw_alloc(-1, 1);
        else if (name == "raw-size-overflow") pf_raw_alloc(high, 1);
        else if (name == "raw-null") pf_raw_load(nullptr, 0, 1);
        else if (name == "raw-boolean") pf_raw_store(pf_raw_alloc(1, 3), 0, 2, 3);
        else if (name == "raw-offset-overflow") pf_raw_load(reinterpret_cast<void*>(std::numeric_limits<uintptr_t>::max() - 3), 1, 1);
        else if (name == "raw-managed-type") pf_raw_alloc(1, 4);
        else if (name == "offset-owned-bounds") pf_raw_offset(pf_raw_alloc(2, 1), 3, 1);
        else if (name == "offset-before-owned") pf_raw_offset(pf_raw_alloc(2, 1), -1, 1);
        else if (name == "offset-onepast-load") pf_raw_load(pf_raw_offset(pf_raw_alloc(2, 1), 2, 1), 0, 1);
        else if (name == "offset-null") pf_raw_offset(nullptr, 0, 1);
        else if (name == "offset-underflow") pf_raw_offset(reinterpret_cast<void*>(uintptr_t(1)), -1, 1);
        else if (name == "offset-overflow") pf_raw_offset(reinterpret_cast<void*>(std::numeric_limits<uintptr_t>::max() - 3), 1, 1);
        else if (name == "offset-minimum") pf_raw_offset(reinterpret_cast<void*>(uintptr_t(1)), low, 1);
        else if (name == "invalid-pin") pf_unpin(reinterpret_cast<void*>(uintptr_t(1)));
        else if (name == "released-pin") { auto* pin = pf_pin(pf_string("hello", 5)); pf_unpin(pin); pf_unpin(pin); }
        else if (name == "result-nonstring") {
            void* roots[1]{pf_tuple_new(3)};
            pf_roots_push(roots, 1);
            pf_set_index(roots[0], 0, 0, 3);
            pf_set_index(roots[0], 1, 0, 5);
            pf_set_index(roots[0], 2, 404, 1);
            pf_result_value(roots[0]);
        }
        else if (name == "result-success-error") {
            void* roots[1]{pf_tuple_new(3)};
            pf_roots_push(roots, 1);
            pf_set_index(roots[0], 0, 1, 3);
            pf_result_error(roots[0]);
        }
        else if (name == "source-location") {
            pf_source_location("example.pf", 12, 7);
            pf_checked_div(1, 0);
        }
        else if (name == "source-location-copy") {
            std::string filename = "copied.pf";
            pf_source_location(filename.c_str(), 8, 3);
            filename.assign("overwritten.pf");
            pf_checked_div(1, 0);
        }
        else if (name == "source-location-explicit") {
            pf_source_location("old.pf", 1, 9);
            pf_fail("explicit failure", "new.pf", 5);
        }
        else { std::cerr << "unknown error case\n"; return 2; }
        std::cerr << "error case unexpectedly succeeded\n";
        return 2;
    }
    testArithmetic();
    testHeap();
    testPrinting();
    testResults();
    testRawMemoryAndPins();
    std::cout << "runtime tests passed\n";
}
