#include "pikaflow/runtime.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(2); }
}
uint64_t bits(void* pointer) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pointer)); }
void* pointer(uint64_t value) { return reinterpret_cast<void*>(static_cast<uintptr_t>(value)); }
void* closure() { return pf_closure_new(reinterpret_cast<void*>(uintptr_t(1)), 0); }
uint64_t unit(void*, void*) { return 0; }

struct Barrier { std::atomic<int> arrived{0}; };
uint64_t concurrent(void*, void* context) {
    auto& barrier = *static_cast<Barrier*>(context);
    ++barrier.arrived;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (barrier.arrived != 2) {
        require(std::chrono::steady_clock::now() < deadline, "two tasks must run simultaneously on real threads");
        std::this_thread::yield();
    }
    // Exercise independent worker GC while both threads allocate.
    for (int index = 0; index < 250; ++index) pf_string("temporary", 9);
    pf_collect();
    return 21;
}

uint64_t isolated(void* environment, void*) {
    auto* pair = pointer(pf_cell_get(pf_closure_get(environment, 0)));
    auto* array = pointer(pf_index(pair, 0));
    require(pf_index(pair, 0) == pf_index(pair, 1), "snapshot preserves shared aliases");
    require(pf_index(array, 0) == 7, "capture snapshot reflects the spawn instant");
    pf_set_index(array, 0, 42, 1);
    void* result = pf_tuple_new(3);
    auto* roots = pf_roots_push(&result, 1);
    pf_set_index(result, 0, bits(array), 5);
    pf_set_index(result, 1, bits(array), 5);
    pf_set_index(result, 2, bits(result), 6);
    pf_collect();
    pf_roots_pop(roots);
    return bits(result);
}

uint64_t finish(void*, void* context) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ++*static_cast<std::atomic<int>*>(context);
    return 0;
}
uint64_t nested(void* environment, void*) {
    auto* task = pf_task_spawn(environment, &unit, nullptr, 0);
    require(pf_task_join(task) == 0, "nested child task returns unit");
    return 99;
}
uint64_t panic(void*, void*) { pf_fail("worker failed", nullptr, 0); }
uint64_t foreignResult(void*, void*) {
    auto* tuple = pf_tuple_new(1);
    pf_set_index(tuple, 0, 0, 9);
    return bits(tuple);
}

void testParallel() {
    auto* scope = pf_tasks_enter();
    void* roots[3]{};
    auto* frame = pf_roots_push(roots, 3);
    roots[0] = closure();
    Barrier barrier;
    roots[1] = pf_task_spawn(roots[0], &concurrent, &barrier, 1);
    roots[2] = pf_task_spawn(roots[0], &concurrent, &barrier, 1);
    pf_collect();
    require(pf_task_join(roots[1]) + pf_task_join(roots[2]) == 42, "parallel task results combine correctly");
    pf_tasks_leave(scope);
    pf_roots_pop(frame);
    pf_collect();
    require(pf_heap_objects() == 0, "joined task heaps and handles are collected");
}

void testIsolation() {
    auto* scope = pf_tasks_enter();
    void* roots[6]{};
    auto* frame = pf_roots_push(roots, 6);
    roots[0] = pf_array_new(1, 1);
    pf_set_index(roots[0], 0, 7, 1);
    roots[1] = pf_tuple_new(2);
    pf_set_index(roots[1], 0, bits(roots[0]), 5);
    pf_set_index(roots[1], 1, bits(roots[0]), 5);
    roots[2] = pf_cell_new(bits(roots[1]), 6);
    roots[3] = pf_closure_new(reinterpret_cast<void*>(uintptr_t(1)), 1);
    pf_closure_set(roots[3], 0, roots[2]);
    roots[4] = pf_task_spawn(roots[3], &isolated, nullptr, 6);
    pf_set_index(roots[0], 0, 8, 1);
    roots[5] = pointer(pf_task_join(roots[4]));
    require(pf_index(roots[0], 0) == 8, "worker mutation never changes parent heap");
    require(pf_index(pointer(pf_index(roots[5], 0)), 0) == 42, "managed task result copied back");
    require(pf_index(roots[5], 0) == pf_index(roots[5], 1), "result preserves aliases");
    require(pf_index(roots[5], 2) == bits(roots[5]), "result preserves cyclic graph");
    const auto result = roots[5];
    roots[5] = nullptr;
    pf_collect();
    require(pointer(pf_task_join(roots[4])) == result, "repeat join retains cached result through GC");
    pf_tasks_leave(scope);
    pf_roots_pop(frame);
    pf_collect();
    require(pf_heap_objects() == 0, "task result cycles are collected after handles expire");
}

void testLifecycle() {
    auto* scope = pf_tasks_enter();
    void* root = closure();
    auto* frame = pf_roots_push(&root, 1);
    std::atomic<int> finished{0};
    pf_task_spawn(root, &finish, &finished, 0);
    pf_collect(); // Drop the unobserved handle while its owning scope remains active.
    pf_tasks_leave(scope);
    require(finished == 1, "leaving scope joins discarded tasks");
    scope = pf_tasks_enter();
    auto* task = pf_task_spawn(root, &nested, nullptr, 1);
    require(pf_task_join(task) == 99, "worker can create structured nested tasks");
    pf_tasks_leave(scope);
    // Join promptly releases resource slots instead of counting historical spawns.
    scope = pf_tasks_enter();
    for (int index = 0; index < 100; ++index)
        require(pf_task_join(pf_task_spawn(root, &unit, nullptr, 0)) == 0, "joined slots reusable");
    pf_tasks_leave(scope);
    pf_roots_pop(frame);
    pf_collect();
}

void failure(const std::string& name) {
    if (name == "outside_scope") { pf_task_spawn(closure(), &unit, nullptr, 0); return; }
    auto* scope = pf_tasks_enter();
    void* roots[3]{};
    auto* frame = pf_roots_push(roots, 3);
    roots[0] = closure();
    if (name == "null_entry") pf_task_spawn(roots[0], nullptr, nullptr, 0);
    else if (name == "pointer_result") pf_task_spawn(roots[0], &unit, nullptr, 9);
    else if (name == "pointer_capture") {
        roots[1] = pf_cell_new(0, 9);
        roots[2] = pf_closure_new(reinterpret_cast<void*>(uintptr_t(1)), 1);
        pf_closure_set(roots[2], 0, roots[1]);
        pf_task_spawn(roots[2], &unit, nullptr, 0);
    } else if (name == "task_capture") {
        roots[1] = pf_task_spawn(roots[0], &unit, nullptr, 0);
        roots[2] = pf_cell_new(bits(roots[1]), 12);
        roots[0] = pf_closure_new(reinterpret_cast<void*>(uintptr_t(1)), 1);
        pf_closure_set(roots[0], 0, roots[2]);
        pf_task_spawn(roots[0], &unit, nullptr, 0);
    } else if (name == "nested_pointer_result") {
        roots[1] = pf_task_spawn(roots[0], &foreignResult, nullptr, 6);
        pf_task_join(roots[1]);
    } else if (name == "worker_panic") {
        roots[1] = pf_task_spawn(roots[0], &panic, nullptr, 0);
        pf_task_join(roots[1]);
    } else if (name == "limit") {
        for (int index = 0; index < 65; ++index) pf_task_spawn(roots[0], &unit, nullptr, 0);
    } else if (name == "scope_order") {
        pf_tasks_enter();
        pf_tasks_leave(scope);
    } else if (name == "invalid_join") pf_task_join(roots[0]);
    else { std::cerr << "unknown failure case\n"; std::exit(2); }
    pf_roots_pop(frame);
    pf_tasks_leave(scope);
    std::exit(3);
}
}

int main(int argc, char** argv) {
    if (argc > 1) { failure(argv[1]); return 3; }
    testParallel();
    testIsolation();
    testLifecycle();
    std::cout << "Structured concurrency runtime tests passed\n";
}
