#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Tags match pika::TypeKind. Managed references use tags 4 through 8 and 10.
typedef struct PFValue { uint64_t bits; uint32_t tag; uint32_t reserved; } PFValue;
void pf_print(uint64_t bits, uint32_t tag);
void pf_fail(const char* message, const char* file, int32_t line);
void pf_panic(void* message);
uint64_t pf_result_value(void* result);
uint64_t pf_result_error(void* result);
void* pf_string(const char* bytes, int64_t length);
void* pf_array_new(int64_t count, uint32_t element_tag);
void* pf_tuple_new(int64_t count);
int64_t pf_length(void* object);
uint64_t pf_index(void* object, int64_t index);
uint32_t pf_index_tag(void* object, int64_t index);
void pf_set_index(void* object, int64_t index, uint64_t bits, uint32_t tag);
void* pf_cell_new(uint64_t bits, uint32_t tag);
uint64_t pf_cell_get(void* cell);
void pf_cell_set(void* cell, uint64_t bits, uint32_t tag);
void* pf_closure_new(void* code, int64_t capture_count);
void pf_closure_set(void* closure, int64_t index, void* cell);
void* pf_closure_get(void* closure, int64_t index);
void* pf_closure_code(void* closure);
void* pf_roots_push(void** slots, int64_t count);
void pf_roots_pop(void* frame);
void pf_collect(void);
int64_t pf_heap_objects(void);
int64_t pf_checked_add(int64_t a, int64_t b);
int64_t pf_checked_sub(int64_t a, int64_t b);
int64_t pf_checked_mul(int64_t a, int64_t b);
int64_t pf_checked_div(int64_t a, int64_t b);
int64_t pf_checked_mod(int64_t a, int64_t b);
int64_t pf_checked_neg(int64_t value);
int64_t pf_float_to_int(double value);
int32_t pf_string_equal(void* a, void* b);
void* pf_raw_alloc(int64_t count, uint32_t element_tag);
void pf_raw_free(void* pointer);
uint64_t pf_raw_load(void* pointer, int64_t index, uint32_t element_tag);
void pf_raw_store(void* pointer, int64_t index, uint64_t bits, uint32_t element_tag);
void* pf_cstr(void* managed_string);
void* pf_pin(void* managed_object);
void pf_unpin(void* handle);
#ifdef __cplusplus
}
#endif
