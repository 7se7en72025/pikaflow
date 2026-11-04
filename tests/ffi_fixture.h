#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
#ifdef PF_BUILD_FIXTURE
#define PF_FIXTURE_API __declspec(dllexport)
#else
#define PF_FIXTURE_API __declspec(dllimport)
#endif
#else
#define PF_FIXTURE_API __attribute__((visibility("default")))
#endif

typedef struct PFPair { int64_t a, b; } PFPair;
typedef struct PFMixed { double x; int64_t y; } PFMixed;
typedef struct PFPadded { bool first; double value; bool last; } PFPadded;
typedef struct PFTiny { bool a, b, c; } PFTiny;
typedef struct PFDoubles { double a, b; } PFDoubles;
typedef struct PFBig { int64_t a, b, c; } PFBig;
typedef struct PFPointer { int64_t* value; bool enabled; } PFPointer;
typedef struct PFOne { int64_t value; } PFOne;
typedef struct PFSmall { bool first, last; } PFSmall;

PF_FIXTURE_API PFPair pf_ffi_pair(PFPair value, int64_t delta);
PF_FIXTURE_API PFMixed pf_ffi_mixed(PFMixed value, double delta);
PF_FIXTURE_API int64_t pf_ffi_padded(PFPadded value);
PF_FIXTURE_API int64_t pf_ffi_tiny(PFTiny value);
PF_FIXTURE_API PFDoubles pf_ffi_doubles(PFDoubles value, double delta);
PF_FIXTURE_API PFBig pf_ffi_big(PFBig value, int64_t delta);
PF_FIXTURE_API int64_t pf_ffi_pressure(int64_t a, int64_t b, int64_t c,
    int64_t d, int64_t e, PFPair pair, int64_t tail);
PF_FIXTURE_API double pf_ffi_sse_pressure(double a, double b, double c,
    double d, double e, double f, double g, PFDoubles pair, double tail);
PF_FIXTURE_API int64_t pf_ffi_varargs(int64_t marker, ...);
PF_FIXTURE_API int64_t pf_ffi_pointer(PFPointer value);
PF_FIXTURE_API PFOne pf_ffi_one(PFOne value);
PF_FIXTURE_API PFSmall pf_ffi_small(PFSmall value);
PF_FIXTURE_API int64_t pf_ffi_small_score(PFSmall value);
