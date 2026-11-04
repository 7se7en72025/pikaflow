#include "pikaflow/runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
constexpr uint32_t UnitTag = 0, IntTag = 1, FloatTag = 2, BoolTag = 3,
    StringTag = 4, ArrayTag = 5, TupleTag = 6, ClosureTag = 7, CellTag = 8,
    PointerTag = 9, ResultTag = 10;

struct Object {
    uint32_t kind;
    uint32_t elementTag = UnitTag;
    bool marked = false;
    std::string bytes;
    std::vector<PFValue> values;
    void* code = nullptr;
    explicit Object(uint32_t k) : kind(k) {}
};

struct RootFrame {
    void** slots;
    int64_t count;
    RootFrame* previous;
};

struct FreeMemory { void operator()(void* pointer) const { std::free(pointer); } };
struct RawAllocation {
    std::unique_ptr<void, FreeMemory> storage;
    size_t bytes;
    uint32_t tag;
};
struct Pin { Object* object; };

struct Heap {
    std::unordered_map<void*, std::unique_ptr<Object>> objects;
    std::unordered_map<void*, RawAllocation> raw;
    std::unordered_map<void*, std::unique_ptr<Pin>> pins;
    RootFrame* roots = nullptr;
    size_t threshold = 64;
    ~Heap() {
        while (roots) {
            auto* previous = roots->previous;
            delete roots;
            roots = previous;
        }
    }
};

thread_local Heap heap;
struct SourceLocation {
    std::string file;
    int32_t line = 1, column = 1;
};
thread_local SourceLocation sourceLocation;

[[noreturn]] void fail(const char* message) {
    pf_fail(message, nullptr, 0);
    std::abort();
}

bool managed(uint32_t tag) { return (tag >= StringTag && tag <= CellTag) || tag == ResultTag; }

uint32_t objectKind(uint32_t tag) { return tag == ResultTag ? TupleTag : tag; }

Object* object(void* reference) {
    auto found = heap.objects.find(reference);
    if (found == heap.objects.end()) fail("invalid or expired managed reference");
    return found->second.get();
}

Object* object(void* reference, uint32_t kind) {
    auto* result = object(reference);
    if (result->kind != kind) fail("runtime object has an incompatible type");
    return result;
}

PFValue value(uint64_t bits, uint32_t tag) {
    if (tag > ResultTag) fail("invalid runtime value tag");
    if (tag == BoolTag && bits > 1) fail("invalid boolean representation");
    if (managed(tag) && bits) {
        auto* reference = reinterpret_cast<void*>(static_cast<uintptr_t>(bits));
        if (object(reference)->kind != objectKind(tag)) fail("managed reference has an incompatible type tag");
    }
    return {bits, tag, 0};
}

int64_t integer(uint64_t bits) {
    int64_t result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

uint64_t bits(void* reference) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(reference));
}

Object* allocate(uint32_t kind, int64_t count = 0) {
    if (count < 0) fail("negative allocation size");
    try {
        if (heap.objects.size() >= heap.threshold) pf_collect();
        auto result = std::make_unique<Object>(kind);
        if (static_cast<uint64_t>(count) > result->values.max_size())
            fail("allocation size is too large");
        result->values.resize(static_cast<size_t>(count), PFValue{0, UnitTag, 0});
        auto* pointer = result.get();
        heap.objects.emplace(pointer, std::move(result));
        return pointer;
    } catch (const std::bad_alloc&) {
        fail("out of memory");
    } catch (const std::length_error&) {
        fail("allocation size is too large");
    }
}

size_t checkedIndex(Object* reference, int64_t index) {
    const auto size = reference->kind == StringTag ? reference->bytes.size() : reference->values.size();
    if (index < 0 || static_cast<uint64_t>(index) >= size)
        fail("index is out of bounds");
    return static_cast<size_t>(index);
}

void requireSequence(Object* reference) {
    if (reference->kind != StringTag && reference->kind != ArrayTag && reference->kind != TupleTag)
        fail("expected an array, tuple, or string");
}

size_t rawWidth(uint32_t tag) {
    switch (tag) {
    case IntTag: case FloatTag: return sizeof(uint64_t);
    case BoolTag: return sizeof(uint8_t);
    case PointerTag: return sizeof(void*);
    default: fail("raw memory supports only i64, f64, bool, or native pointers");
    }
}

unsigned char* rawAddress(void* pointer, int64_t index, uint32_t tag) {
    if (!pointer) fail("null raw pointer");
    if (index < 0) fail("negative raw memory index");
    const auto width = rawWidth(tag);
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    const auto maximum = std::numeric_limits<uintptr_t>::max();
    if (static_cast<uint64_t>(index) > (maximum - address) / width)
        fail("raw pointer offset overflow");
    for (const auto& item : heap.raw) {
        const auto base = reinterpret_cast<uintptr_t>(item.first);
        const auto& allocation = item.second;
        if (address >= base && address - base <= allocation.bytes) {
            const auto offset = static_cast<size_t>(address - base);
            if (tag != allocation.tag) fail("raw memory element type mismatch");
            if (offset % width) fail("misaligned offset into owned raw memory");
            if (static_cast<uint64_t>(index) >= (allocation.bytes - offset) / width)
                fail("raw memory index is out of bounds");
            return static_cast<unsigned char*>(allocation.storage.get()) + offset + static_cast<size_t>(index) * width;
        }
    }
    // Foreign pointer validity and lifetime are the caller's unsafe obligation.
    return reinterpret_cast<unsigned char*>(address + static_cast<uintptr_t>(index) * width);
}

void render(std::ostream& output, PFValue item, std::unordered_set<Object*>& active, bool nested) {
    switch (item.tag) {
    case UnitTag: output << "unit"; return;
    case IntTag: output << integer(item.bits); return;
    case FloatTag: {
        double number;
        std::memcpy(&number, &item.bits, sizeof(number));
        output << std::setprecision(std::numeric_limits<double>::max_digits10) << number;
        return;
    }
    case BoolTag: output << (item.bits ? "true" : "false"); return;
    case PointerTag: output << "0x" << std::hex << item.bits << std::dec; return;
    default: break;
    }
    if (!item.bits) { output << "null"; return; }
    auto* reference = object(reinterpret_cast<void*>(static_cast<uintptr_t>(item.bits)), objectKind(item.tag));
    if (reference->kind == StringTag) {
        if (nested) output << std::quoted(reference->bytes);
        else output.write(reference->bytes.data(), static_cast<std::streamsize>(reference->bytes.size()));
        return;
    }
    if (reference->kind == ClosureTag) { output << "<function>"; return; }
    if (!active.insert(reference).second) { output << "<cycle>"; return; }
    if (reference->kind == CellTag) {
        output << "<cell ";
        render(output, reference->values[0], active, true);
        output << '>';
    } else {
        output << (reference->kind == TupleTag ? '(' : '[');
        for (size_t index = 0; index < reference->values.size(); ++index) {
            if (index) output << ", ";
            render(output, reference->values[index], active, true);
        }
        if (reference->kind == TupleTag && reference->values.size() == 1) output << ',';
        output << (reference->kind == TupleTag ? ')' : ']');
    }
    active.erase(reference);
}
}

extern "C" {
void pf_fail(const char* message, const char* file, int32_t line) {
    const bool explicitLocation = file && *file;
    const char* filename = explicitLocation ? file : sourceLocation.file.c_str();
    const int32_t actualLine = explicitLocation ? line : sourceLocation.line;
    const int32_t actualColumn = explicitLocation && (sourceLocation.file != file || sourceLocation.line != line)
        ? 1 : sourceLocation.column;
    if (*filename) std::fprintf(stderr, "%s:%d:%d: runtime error: %s\n", filename,
        actualLine > 0 ? actualLine : 1, actualColumn > 0 ? actualColumn : 1, message ? message : "unknown failure");
    else std::fprintf(stderr, "runtime error: %s\n", message ? message : "unknown failure");
    std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

void pf_source_location(const char* file, int32_t line, int32_t column) {
    const char* filename = file ? file : "";
    try {
        if (sourceLocation.file != filename) sourceLocation.file = filename;
    } catch (const std::bad_alloc&) {
        pf_fail("out of memory recording source location", filename, line);
    }
    sourceLocation.line = line > 0 ? line : 1;
    sourceLocation.column = column > 0 ? column : 1;
}

void pf_panic(void* message) { fail(object(message, StringTag)->bytes.c_str()); }

uint64_t pf_result_value(void* reference) {
    auto* result = object(reference, TupleTag);
    if (result->values.size() != 3 || result->values[0].tag != BoolTag)
        fail("invalid Result representation");
    if (!result->values[0].bits) {
        const auto& error = result->values[2];
        if (error.tag == StringTag && error.bits)
            pf_panic(reinterpret_cast<void*>(static_cast<uintptr_t>(error.bits)));
        fail("cannot unwrap an error Result");
    }
    return result->values[1].bits;
}

uint64_t pf_result_error(void* reference) {
    auto* result = object(reference, TupleTag);
    if (result->values.size() != 3 || result->values[0].tag != BoolTag)
        fail("invalid Result representation");
    if (result->values[0].bits) fail("cannot extract an error from a successful Result");
    return result->values[2].bits;
}

void pf_print(uint64_t payload, uint32_t tag) {
    try {
        std::ostringstream output;
        std::unordered_set<Object*> active;
        render(output, value(payload, tag), active, false);
        output << '\n';
        std::cout << output.str();
        if (!std::cout) fail("failed to write standard output");
    } catch (const std::bad_alloc&) {
        fail("out of memory while printing");
    }
}

void* pf_string(const char* bytes, int64_t length) {
    if (length < 0 || (!bytes && length != 0)) fail("invalid string buffer");
    auto* result = allocate(StringTag);
    try {
        if (static_cast<uint64_t>(length) > result->bytes.max_size()) fail("string is too large");
        if (length) result->bytes.assign(bytes, static_cast<size_t>(length));
    } catch (const std::bad_alloc&) { fail("out of memory"); }
    catch (const std::length_error&) { fail("string is too large"); }
    return result;
}

void* pf_array_new(int64_t count, uint32_t elementTag) {
    value(0, elementTag);
    auto* result = allocate(ArrayTag, count);
    result->elementTag = elementTag;
    for (auto& item : result->values) item.tag = elementTag;
    return result;
}

void* pf_tuple_new(int64_t count) { return allocate(TupleTag, count); }

int64_t pf_length(void* reference) {
    auto* result = object(reference);
    requireSequence(result);
    return static_cast<int64_t>(result->kind == StringTag ? result->bytes.size() : result->values.size());
}

uint64_t pf_index(void* reference, int64_t index) {
    auto* result = object(reference);
    requireSequence(result);
    const auto position = checkedIndex(result, index);
    return result->kind == StringTag
        ? static_cast<unsigned char>(result->bytes[position]) : result->values[position].bits;
}

uint32_t pf_index_tag(void* reference, int64_t index) {
    auto* result = object(reference);
    requireSequence(result);
    const auto position = checkedIndex(result, index);
    return result->kind == StringTag ? IntTag : result->values[position].tag;
}

void pf_set_index(void* reference, int64_t index, uint64_t payload, uint32_t tag) {
    auto* result = object(reference);
    if (result->kind != ArrayTag && result->kind != TupleTag) fail("only arrays and tuples can store indexed values");
    const auto position = checkedIndex(result, index);
    if (result->kind == ArrayTag && tag != result->elementTag) fail("array element has an incompatible type");
    result->values[position] = value(payload, tag);
}

void* pf_cell_new(uint64_t payload, uint32_t tag) {
    // Validate before a possible collection; managed inputs must be rooted by the caller.
    const auto initial = value(payload, tag);
    auto* result = allocate(CellTag, 1);
    result->values[0] = initial;
    return result;
}

uint64_t pf_cell_get(void* reference) { return object(reference, CellTag)->values[0].bits; }

void* pf_cell_address(void* reference) { return &object(reference, CellTag)->values[0].bits; }

void pf_cell_set(void* reference, uint64_t payload, uint32_t tag) {
    auto* result = object(reference, CellTag);
    if (tag != result->values[0].tag) fail("cell assignment has an incompatible type");
    result->values[0] = value(payload, tag);
}

void* pf_closure_new(void* code, int64_t count) {
    if (!code) fail("function has no implementation");
    auto* result = allocate(ClosureTag, count);
    result->code = code;
    for (auto& item : result->values) item.tag = CellTag;
    return result;
}

void pf_closure_set(void* reference, int64_t index, void* cell) {
    auto* result = object(reference, ClosureTag);
    const auto position = checkedIndex(result, index);
    result->values[position] = value(bits(cell), CellTag);
}

void* pf_closure_get(void* reference, int64_t index) {
    auto* result = object(reference, ClosureTag);
    const auto position = checkedIndex(result, index);
    auto* cell = reinterpret_cast<void*>(static_cast<uintptr_t>(result->values[position].bits));
    if (!cell) fail("function capture is uninitialized");
    return cell;
}

void* pf_closure_code(void* reference) { return object(reference, ClosureTag)->code; }

void* pf_roots_push(void** slots, int64_t count) {
    if (count < 0 || (count && !slots)) fail("invalid GC root frame");
    for (int64_t index = 0; index < count; ++index) if (slots[index]) object(slots[index]);
    try {
        auto* frame = new RootFrame{slots, count, heap.roots};
        heap.roots = frame;
        return frame;
    } catch (const std::bad_alloc&) { fail("out of memory registering GC roots"); }
}

void pf_roots_pop(void* frame) {
    if (!frame || frame != heap.roots) fail("GC root frames must be removed in reverse order");
    auto* current = heap.roots;
    heap.roots = current->previous;
    delete current;
}

void pf_collect(void) {
    try {
        std::vector<Object*> pending;
        auto mark = [&](void* reference) {
            if (!reference) return;
            auto* target = object(reference);
            if (!target->marked) {
                target->marked = true;
                pending.push_back(target);
            }
        };
        for (auto* frame = heap.roots; frame; frame = frame->previous)
            for (int64_t index = 0; index < frame->count; ++index) mark(frame->slots[index]);
        for (const auto& pin : heap.pins) mark(pin.second->object);
        while (!pending.empty()) {
            auto* current = pending.back();
            pending.pop_back();
            for (const auto& item : current->values)
                if (managed(item.tag) && item.bits)
                    mark(reinterpret_cast<void*>(static_cast<uintptr_t>(item.bits)));
        }
        for (auto iterator = heap.objects.begin(); iterator != heap.objects.end();) {
            if (!iterator->second->marked) iterator = heap.objects.erase(iterator);
            else { iterator->second->marked = false; ++iterator; }
        }
        const auto live = heap.objects.size();
        heap.threshold = live > std::numeric_limits<size_t>::max() / 2
            ? std::numeric_limits<size_t>::max() : std::max<size_t>(64, live * 2);
    } catch (const std::bad_alloc&) { fail("out of memory during garbage collection"); }
}

int64_t pf_heap_objects(void) { return static_cast<int64_t>(heap.objects.size()); }

int64_t pf_checked_add(int64_t a, int64_t b) {
    const auto low = std::numeric_limits<int64_t>::min(), high = std::numeric_limits<int64_t>::max();
    if ((b > 0 && a > high - b) || (b < 0 && a < low - b)) fail("integer addition overflow");
    return a + b;
}

int64_t pf_checked_sub(int64_t a, int64_t b) {
    const auto low = std::numeric_limits<int64_t>::min(), high = std::numeric_limits<int64_t>::max();
    if ((b > 0 && a < low + b) || (b < 0 && a > high + b)) fail("integer subtraction overflow");
    return a - b;
}

int64_t pf_checked_mul(int64_t a, int64_t b) {
    const auto low = std::numeric_limits<int64_t>::min(), high = std::numeric_limits<int64_t>::max();
    if (a > 0) {
        if ((b > 0 && a > high / b) || (b < 0 && b < low / a)) fail("integer multiplication overflow");
    } else if (a < 0) {
        if ((b > 0 && a < low / b) || (b < 0 && a < high / b)) fail("integer multiplication overflow");
    }
    return a * b;
}

int64_t pf_checked_div(int64_t a, int64_t b) {
    if (!b) fail("integer division by zero");
    if (a == std::numeric_limits<int64_t>::min() && b == -1) fail("integer division overflow");
    return a / b;
}

int64_t pf_checked_mod(int64_t a, int64_t b) {
    if (!b) fail("integer remainder by zero");
    if (a == std::numeric_limits<int64_t>::min() && b == -1) fail("integer remainder overflow");
    return a % b;
}

int64_t pf_checked_neg(int64_t number) {
    if (number == std::numeric_limits<int64_t>::min()) fail("integer negation overflow");
    return -number;
}

int64_t pf_float_to_int(double number) {
    constexpr double lower = -9223372036854775808.0;
    constexpr double upper = 9223372036854775808.0;
    if (!std::isfinite(number) || number < lower || number >= upper) fail("floating-point value is outside i64 range");
    return static_cast<int64_t>(number);
}

int32_t pf_string_equal(void* a, void* b) {
    return object(a, StringTag)->bytes == object(b, StringTag)->bytes ? 1 : 0;
}

void* pf_raw_alloc(int64_t count, uint32_t elementTag) {
    if (count < 0) fail("negative raw allocation size");
    const auto width = rawWidth(elementTag);
    if (static_cast<uint64_t>(count) > std::numeric_limits<size_t>::max() / width)
        fail("raw allocation size is too large");
    const auto bytes = static_cast<size_t>(count) * width;
    RawAllocation allocation{std::unique_ptr<void, FreeMemory>(std::calloc(bytes ? bytes : 1, 1)), bytes, elementTag};
    if (!allocation.storage) fail("out of memory allocating raw storage");
    void* result = allocation.storage.get();
    try { heap.raw.emplace(result, std::move(allocation)); }
    catch (const std::bad_alloc&) { fail("out of memory tracking raw allocation"); }
    return result;
}

void pf_raw_free(void* pointer) {
    auto found = heap.raw.find(pointer);
    if (found == heap.raw.end()) fail("raw pointer was not allocated here or has already been freed");
    heap.raw.erase(found);
}

uint64_t pf_raw_load(void* pointer, int64_t index, uint32_t elementTag) {
    const auto width = rawWidth(elementTag);
    const auto* address = rawAddress(pointer, index, elementTag);
    uint64_t result = 0;
    std::memcpy(&result, address, width);
    if (elementTag == BoolTag && result > 1) fail("invalid boolean representation in raw memory");
    return result;
}

void pf_raw_store(void* pointer, int64_t index, uint64_t payload, uint32_t elementTag) {
    const auto width = rawWidth(elementTag);
    if (elementTag == BoolTag && payload > 1) fail("invalid boolean representation in raw memory");
    std::memcpy(rawAddress(pointer, index, elementTag), &payload, width);
}

void* pf_cstr(void* reference) {
    return const_cast<char*>(object(reference, StringTag)->bytes.c_str());
}

void* pf_pin(void* reference) {
    auto* target = object(reference);
    try {
        auto pin = std::make_unique<Pin>(Pin{target});
        auto* handle = pin.get();
        heap.pins.emplace(handle, std::move(pin));
        return handle;
    } catch (const std::bad_alloc&) { fail("out of memory registering a persistent root"); }
}

void pf_unpin(void* handle) {
    auto found = heap.pins.find(handle);
    if (found == heap.pins.end()) fail("invalid or released persistent root handle");
    heap.pins.erase(found);
}
}
