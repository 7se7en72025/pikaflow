#include "ffi_fixture.h"
#include <stdarg.h>

PFPair pf_ffi_pair(PFPair value, int64_t delta) {
    PFPair result = {value.a + delta, value.b + delta}; return result;
}
PFMixed pf_ffi_mixed(PFMixed value, double delta) {
    PFMixed result = {value.x + delta, value.y}; return result;
}
int64_t pf_ffi_padded(PFPadded value) {
    return (int64_t)value.value + (value.first ? 1 : 0) + (value.last ? 1 : 0);
}
int64_t pf_ffi_tiny(PFTiny value) { return value.a * 10 + value.b * 20 + value.c * 12; }
PFDoubles pf_ffi_doubles(PFDoubles value, double delta) {
    PFDoubles result = {value.a + delta, value.b + delta}; return result;
}
PFBig pf_ffi_big(PFBig value, int64_t delta) {
    PFBig result = {value.a + delta, value.b + delta, value.c + delta}; return result;
}
int64_t pf_ffi_pressure(int64_t a, int64_t b, int64_t c,
    int64_t d, int64_t e, PFPair pair, int64_t tail) {
    return a + b + c + d + e + pair.a + pair.b + tail;
}
double pf_ffi_sse_pressure(double a, double b, double c,
    double d, double e, double f, double g, PFDoubles pair, double tail) {
    return a + b + c + d + e + f + g + pair.a + pair.b + tail;
}
int64_t pf_ffi_varargs(int64_t marker, ...) {
    va_list args; va_start(args, marker);
    int flag = va_arg(args, int);
    double number = va_arg(args, double);
    PFPair pair = va_arg(args, PFPair);
    va_end(args);
    return marker + flag + (int64_t)number + pair.a + pair.b;
}
int64_t pf_ffi_pointer(PFPointer value) { return value.enabled ? *value.value : 0; }
PFOne pf_ffi_one(PFOne value) { PFOne result = {value.value + 22}; return result; }
PFSmall pf_ffi_small(PFSmall value) { PFSmall result = {!value.first, !value.last}; return result; }
int64_t pf_ffi_small_score(PFSmall value) { return value.first && !value.last ? 42 : 0; }
PFNested pf_ffi_nested(PFNested value, int64_t delta) {
    value.pair.a += delta; value.pair.b += delta;
    value.mixed.x += delta; value.mixed.y += delta;
    return value;
}
PFNestedDouble pf_ffi_nested_small(PFNestedDouble value, double delta) {
    value.inner.value += delta; return value;
}
int64_t pf_ffi_callback_i64(int64_t (*callback)(int64_t, int64_t)) { return callback(20, 22); }
double pf_ffi_callback_f64(double (*callback)(double, double)) { return callback(10.0, 32.0); }
int64_t pf_ffi_callback_bool(bool (*callback)(bool, bool)) { return callback(true, false) ? 42 : 0; }
void pf_ffi_callback_unit(void (*callback)(void)) { callback(); }
int64_t pf_ffi_callback_pointer(int64_t* (*callback)(int64_t*)) {
    int64_t value = 42; return *callback(&value);
}
