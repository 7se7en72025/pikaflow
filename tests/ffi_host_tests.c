#include "ffi_fixture.h"
#include <stdio.h>

#define REQUIRE(condition) do { if (!(condition)) { fprintf(stderr, "C ABI reference failed at line %d\n", __LINE__); return 1; } } while (0)
int main(void) {
    PFPair pair = {10, 20}; PFPair p = pf_ffi_pair(pair, 6);
    REQUIRE(p.a + p.b == 42);
    PFMixed mixed = {10.0, 20}; PFMixed m = pf_ffi_mixed(mixed, 12.0);
    REQUIRE((int64_t)m.x + m.y == 42);
    PFPadded padded = {true, 40.0, true}; REQUIRE(pf_ffi_padded(padded) == 42);
    PFTiny tiny = {true, true, true}; REQUIRE(pf_ffi_tiny(tiny) == 42);
    PFDoubles doubles = {10.0, 20.0}; PFDoubles d = pf_ffi_doubles(doubles, 6.0);
    REQUIRE(d.a + d.b == 42.0);
    PFBig big = {1, 2, 3}; PFBig b = pf_ffi_big(big, 12); REQUIRE(b.a + b.b + b.c == 42);
    PFPair pressure = {10, 11}; REQUIRE(pf_ffi_pressure(1,2,3,4,5,pressure,6) == 42);
    PFDoubles sse = {20.0, 22.0}; REQUIRE(pf_ffi_sse_pressure(0,0,0,0,0,0,0,sse,0) == 42.0);
    REQUIRE(pf_ffi_varargs(1, 1, 10.0, pair) == 42);
    int64_t forty_two = 42; PFPointer pointer = {&forty_two, true}; REQUIRE(pf_ffi_pointer(pointer) == 42);
    PFOne one = {20}; REQUIRE(pf_ffi_one(one).value == 42);
    PFSmall small = {false, true}; REQUIRE(pf_ffi_small_score(pf_ffi_small(small)) == 42);
    puts("C ABI reference fixture passed"); return 0;
}
