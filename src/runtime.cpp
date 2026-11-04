#include "pikaflow/runtime.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
constexpr uint32_t UnitTag = 0, IntTag = 1, FloatTag = 2, BoolTag = 3,
    StringTag = 4, ArrayTag = 5, TupleTag = 6, ClosureTag = 7, CellTag = 8,
    PointerTag = 9, ResultTag = 10, TaskTag = 12;

struct TaskState;

struct Object {
    uint32_t kind;
    uint32_t elementTag = UnitTag;
    bool marked = false;
    std::string bytes;
    std::vector<PFValue> values;
    void* code = nullptr;
    std::shared_ptr<TaskState> task;
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
std::mutex outputMutex;

struct TaskState {
    std::thread worker;
    std::unordered_map<void*, std::unique_ptr<Object>> objects;
    PFValue result{0, UnitTag, 0};
    bool imported = false;
    bool counted = false;
    void join();
    ~TaskState() { join(); }
};
struct TaskScope {
    TaskScope* previous;
    std::vector<std::shared_ptr<TaskState>> tasks;
};
thread_local TaskScope* taskScope = nullptr;
std::atomic<unsigned> outstandingTasks{0};
constexpr unsigned MaximumTasks = 64;

[[noreturn]] void fail(const char* message) {
    pf_fail(message, nullptr, 0);
}

void TaskState::join() {
    if (worker.joinable()) {
        try { worker.join(); }
        catch (const std::system_error&) { fail("failed to join task thread"); }
    }
    if (counted) { --outstandingTasks; counted = false; }
}

bool managed(uint32_t tag) { return (tag >= StringTag && tag <= CellTag) || tag == ResultTag || tag == TaskTag; }

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
    if (tag > TaskTag || tag == 11) fail("invalid runtime value tag");
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

using Objects = std::unordered_map<void*, std::unique_ptr<Object>>;

PFValue snapshot(PFValue root, const Objects& source, Objects& destination) {
    std::unordered_map<Object*, Object*> copies;
    std::vector<std::pair<Object*, Object*>> pending;
    auto copyValue = [&](PFValue input) {
        if (input.tag == PointerTag || input.tag == TaskTag)
            fail("task snapshots cannot contain native pointers or task handles");
        if (!managed(input.tag) || !input.bits) return input;
        auto found = source.find(reinterpret_cast<void*>(static_cast<uintptr_t>(input.bits)));
        if (found == source.end() || found->second->kind != objectKind(input.tag))
            fail("invalid managed reference in task snapshot");
        auto* original = found->second.get();
        if (original->kind == ArrayTag && (original->elementTag == PointerTag || original->elementTag == TaskTag))
            fail("task snapshots cannot contain native pointers or task handles");
        auto known = copies.find(original);
        Object* copy;
        if (known != copies.end()) copy = known->second;
        else {
            auto storage = std::make_unique<Object>(original->kind);
            storage->elementTag = original->elementTag;
            storage->bytes = original->bytes;
            storage->values = original->values;
            storage->code = original->code;
            copy = storage.get();
            copies.emplace(original, copy);
            destination.emplace(copy, std::move(storage));
            pending.emplace_back(original, copy);
        }
        input.bits = bits(copy);
        return input;
    };
    const auto result = copyValue(root);
    for (size_t index = 0; index < pending.size(); ++index) {
        auto* copy = pending[index].second;
        for (auto& item : copy->values) item = copyValue(item);
    }
    return result;
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
    struct Frame {
        PFValue item;
        bool nested;
        Object* reference = nullptr;
        size_t next = 0;
    };
    std::vector<Frame> frames{{item, nested}};
    while (!frames.empty()) {
        auto& frame = frames.back();
        if (frame.reference) {
            auto* reference = frame.reference;
            if (frame.next < reference->values.size()) {
                if (frame.next && reference->kind != CellTag) output << ", ";
                auto child = reference->values[frame.next++];
                frames.push_back({child, true});
                continue;
            }
            if (reference->kind == CellTag) output << '>';
            else {
                if (reference->kind == TupleTag && reference->values.size() == 1) output << ',';
                output << (reference->kind == TupleTag ? ')' : ']');
            }
            active.erase(reference);
            frames.pop_back();
            continue;
        }
        const auto current = frame.item;
        switch (current.tag) {
        case UnitTag: output << "unit"; frames.pop_back(); continue;
        case IntTag: output << integer(current.bits); frames.pop_back(); continue;
        case FloatTag: {
            double number;
            std::memcpy(&number, &current.bits, sizeof(number));
            output << std::setprecision(std::numeric_limits<double>::max_digits10) << number;
            frames.pop_back(); continue;
        }
        case BoolTag: output << (current.bits ? "true" : "false"); frames.pop_back(); continue;
        case PointerTag: output << "0x" << std::hex << current.bits << std::dec; frames.pop_back(); continue;
        default: break;
        }
        if (!current.bits) { output << "null"; frames.pop_back(); continue; }
        auto* reference = object(reinterpret_cast<void*>(static_cast<uintptr_t>(current.bits)), objectKind(current.tag));
        if (reference->kind == StringTag) {
            if (frame.nested) output << std::quoted(reference->bytes);
            else output.write(reference->bytes.data(), static_cast<std::streamsize>(reference->bytes.size()));
            frames.pop_back(); continue;
        }
        if (reference->kind == ClosureTag) { output << "<function>"; frames.pop_back(); continue; }
        if (reference->kind == TaskTag) { output << "<task>"; frames.pop_back(); continue; }
        if (!active.insert(reference).second) { output << "<cycle>"; frames.pop_back(); continue; }
        output << (reference->kind == CellTag ? "<cell " : reference->kind == TupleTag ? "(" : "[");
        frame.reference = reference;
    }
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
        std::lock_guard<std::mutex> lock(outputMutex);
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

uint64_t pf_c_integer(uint64_t payload, uint32_t width, uint32_t isSigned) {
    if ((width != 8 && width != 16 && width != 32 && width != 64) || isSigned > 1)
        fail("invalid C integer width or signedness");
    // The i64 carrier preserves every bit of a 64-bit C integer, including u64.
    if (width == 64) return payload;
    if (isSigned) {
        const auto limit = int64_t(1) << (width - 1);
        const auto number = integer(payload);
        if (number < -limit || number >= limit) fail("C integer argument is outside its declared signed width");
    } else if (payload > (uint64_t(1) << width) - 1)
        fail("C integer argument is outside its declared unsigned width");
    return payload;
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

void* pf_raw_offset(void* pointer, int64_t elements, uint32_t elementTag) {
    if (!pointer) fail("null raw pointer");
    const auto width = elementTag == UnitTag ? size_t(1) : rawWidth(elementTag);
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    const auto maximum = std::numeric_limits<uintptr_t>::max();
    const auto magnitude = elements < 0 ? static_cast<uint64_t>(-(elements + 1)) + 1 : static_cast<uint64_t>(elements);
    if (magnitude > maximum / width) fail("raw pointer offset overflow");
    const auto distance = static_cast<uintptr_t>(magnitude) * width;
    if ((elements < 0 && distance > address) || (elements >= 0 && distance > maximum - address))
        fail("raw pointer offset overflow");
    const auto destination = elements < 0 ? address - distance : address + distance;
    for (const auto& item : heap.raw) {
        const auto base = reinterpret_cast<uintptr_t>(item.first);
        const auto& allocation = item.second;
        if (address >= base && address - base <= allocation.bytes) {
            if (elementTag != allocation.tag) fail("raw memory element type mismatch");
            if ((address - base) % width) fail("misaligned offset into owned raw memory");
            if (destination < base || destination - base > allocation.bytes)
                fail("raw pointer offset is out of bounds");
            return reinterpret_cast<void*>(destination);
        }
    }
    // Foreign allocation bounds and lifetimes remain the caller's unsafe obligation.
    return reinterpret_cast<void*>(destination);
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

void* pf_tasks_enter(void) {
    try {
        auto* scope = new TaskScope{taskScope, {}};
        taskScope = scope;
        return scope;
    } catch (const std::bad_alloc&) { fail("out of memory entering task scope"); }
}

void pf_tasks_leave(void* handle) {
    if (!handle || handle != taskScope) fail("task scopes must be left in reverse order");
    auto* scope = taskScope;
    for (const auto& task : scope->tasks) task->join();
    taskScope = scope->previous;
    delete scope;
}

void* pf_task_spawn(void* closure, PFTaskEntry entry, void* context, uint32_t resultTag) {
    object(closure, ClosureTag);
    if (!taskScope) fail("spawn requires an active structured task scope");
    if (!entry) fail("task has no worker entry point");
    if (resultTag == PointerTag || resultTag == TaskTag || resultTag == 11 || resultTag > TaskTag)
        fail("task result type cannot contain native pointers or task handles");
    try {
        auto state = std::make_shared<TaskState>();
        const auto environment = snapshot({bits(closure), ClosureTag, 0}, heap.objects, state->objects);
        auto* result = allocate(TaskTag, 1);
        result->task = state;
        result->values[0] = {0, resultTag, 0};
        auto& tasks = taskScope->tasks;
        tasks.erase(std::remove_if(tasks.begin(), tasks.end(), [](const auto& task) {
            return !task->counted;
        }), tasks.end());
        tasks.push_back(state);
        if (outstandingTasks.fetch_add(1) >= MaximumTasks) {
            --outstandingTasks;
            fail("structured concurrency limit (64 outstanding tasks) exceeded");
        }
        state->counted = true;
        // Each worker exclusively owns its heap; no parent GC state is accessed.
        state->worker = std::thread([worker = state.get(), environment, entry, context, resultTag] {
            try {
                heap.objects = std::move(worker->objects);
                auto* closureRoot = reinterpret_cast<void*>(static_cast<uintptr_t>(environment.bits));
                auto* frame = pf_roots_push(&closureRoot, 1);
                auto* scope = pf_tasks_enter();
                const auto payload = entry(closureRoot, context);
                pf_tasks_leave(scope);
                pf_roots_pop(frame);
                worker->result = value(payload, resultTag);
                void* resultRoot = managed(resultTag)
                    ? reinterpret_cast<void*>(static_cast<uintptr_t>(payload)) : nullptr;
                frame = pf_roots_push(&resultRoot, 1);
                pf_collect();
                pf_roots_pop(frame);
                worker->objects = std::move(heap.objects);
            } catch (const std::exception& error) {
                pf_fail(error.what(), nullptr, 0);
            } catch (...) { fail("unhandled task failure"); }
        });
        return result;
    } catch (const std::bad_alloc&) { fail("out of memory spawning task"); }
    catch (const std::system_error&) { fail("failed to start task thread"); }
}

uint64_t pf_task_join(void* reference) {
    auto* task = object(reference, TaskTag);
    auto& state = *task->task;
    state.join();
    if (!state.imported) {
        try { task->values[0] = snapshot(state.result, state.objects, heap.objects); }
        catch (const std::bad_alloc&) { fail("out of memory importing task result"); }
        state.objects.clear();
        state.imported = true;
    }
    return task->values[0].bits;
}
}
