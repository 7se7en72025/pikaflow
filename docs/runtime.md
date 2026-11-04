# Runtime and garbage collection

The runtime implements a precise, non-moving, mark-and-sweep collector. Heap
objects are strings, arrays, tuples, mutable cells, and closures. Result values
are tuples containing a success boolean, a payload, and an error value. Both
payload and error types may contain managed references. Extracting the wrong
variant fails; unwrapping a string error reports that message, while other error
types produce a generic diagnostic.

All allocations belong to the current thread's runtime heap. Structured tasks
use real threads with isolated heaps and copied capture/result graphs.

## Compiler and interpreter contract

- `pf_roots_push(slots, count)` registers an array of **actual object pointers**.
  A slot contains an object pointer, not the address of a local variable.
- The array remains alive and at the same address until `pf_roots_pop(frame)`.
  Frames must be removed in reverse registration order.
- Callers update slots whenever managed virtual registers change. Parameters,
  captured environments, and intermediate managed values must stay rooted
  across any call that can allocate.
- Allocation may collect **before** constructing the new object. A newly
  returned object must be rooted before the caller makes another allocation.
- Managed tags 4 through 8, tag 10, and task tag 12 are traced. Native pointer tag 9 is not.
  Native code retaining an object must retain an explicit registered root.

Array, tuple, cell, and closure references are traversed during collection.
Unreachable cycles are reclaimed. The tracing worklist is iterative. Strings
are immutable UTF-8 byte sequences; length and indexing count bytes, and string
indexing returns an unsigned byte integer.

Printing also traverses managed graphs iteratively. Active ancestors print as
`<cycle>`; a shared child reached along separate paths is printed separately.
Nested strings are quoted, while a top-level string prints its bytes directly.
The 0.3.1 regression renders 50,000 nested tuples containing a shared cyclic
leaf, checks the exact output, then collects the entire graph. This removes
native-stack growth from graph depth; output size and heap memory remain finite
resource requirements.

`pf_cell_address` exposes a mutable cell's value storage for generated debug
information. Its address remains stable while the cell stays reachable. The
address itself is not a root and must not outlive the cell.

## Failure behavior

Invalid bounds, incompatible runtime tags, expired managed references, integer
overflow, division by zero, failed Result unwrapping, and invalid GC frame order
produce a diagnostic on standard error and terminate with exit code 1. Errors
do not unwind C++ exceptions through JIT-generated code. Integer division
truncates toward zero. Converting a finite, in-range floating-point value to
`i64` also truncates toward zero.

`pf_c_integer` validates explicitly declared C integer argument widths before the
backend truncates their `i64` carriers. Signed and unsigned 8/16/32-bit ranges
are checked; 64-bit declarations preserve every carrier bit, so a `c_u64`
value with its high bit set is represented by a negative language `i64`.
The reference interpreter rejects narrow C width metadata instead of calling
an audited foreign function through a mismatched signature. Explicit 64-bit
carriers remain compatible with its audited integer calls.

Generated code and the interpreter call `pf_source_location` before operations
that may fail. The runtime owns a thread-local copy of the filename, line, and
column, so diagnostics remain valid after a JIT module is unloaded. Errors use
`file:line:column: runtime error: message` when source information is available.

The native runtime test executable checks arithmetic boundaries, reachability,
cycles, closures, UTF-8 indexing, Result references, and printable values.
`tests/runtime_failures.py` checks fatal failures in subprocesses.

Focused 0.3.1 runtime and IR tests also passed Clang 18 ASan/UBSan with leak
detection. The [focused record](validation/focused-sanitizers-0.3.1-2026-11-05.json)
retains its original dirty-worktree provenance; four recorded source hashes
match the frozen `d77a3b5` blobs. This is focused sanitizer evidence, not a
whole-compiler sanitizer or successful TSan claim.

The reference interpreter keeps calls in explicit heap-allocated VM frames, so
language recursion does not consume the Windows native call stack. It reports
a controlled error at 1024 active VM frames and releases registered roots in
reverse order when evaluation fails.

## Unsafe native memory and C strings

`pf_raw_alloc` returns zero-initialized manual storage for `i64`, `f64`, `bool`,
or native pointer elements. `pf_raw_load` and `pf_raw_store` check the bounds,
element type, and interior offset when the pointer belongs to an allocation
known to this runtime. `pf_raw_free` accepts only an allocation's original base
pointer and rejects unknown or already freed allocations. Manual memory is not
scanned by the garbage collector. Runtime-owned raw blocks must be released with
`pf_raw_free`, not a foreign `free`; foreign allocations must be released by
their matching foreign allocator API.

Foreign pointers have no runtime allocation metadata. Their validity, bounds,
aliasing, and lifetime remain the caller's unsafe responsibility. Stale pointers
cannot reliably be distinguished from foreign or subsequently reused addresses;
the runtime does not claim use-after-free protection.

`offset(pointer, elements)` / `pf_raw_offset` computes a signed element offset.
For owned blocks, the runtime checks the element type, alignment, and resulting
bounds, allowing a one-past pointer for arithmetic but rejecting its dereference.
Opaque `ptr[unit]` foreign pointers use byte offsets. Address arithmetic overflow
is rejected even for foreign pointers; foreign allocation bounds remain unsafe.

`pf_cstr` exposes a managed string's read-only NUL-terminated bytes. Embedded NUL
bytes are preserved, but C string functions observe the usual first-NUL
termination. The original string must stay rooted while native code uses those
bytes; the returned native pointer is not a GC root.

`pf_pin` creates a distinct opaque persistent-root handle. The collector retains
the managed object and its reachable children until every corresponding handle
has been released with `pf_unpin`. Handles are native pointers, not managed
objects; passing a released or unrecognized handle to `pf_unpin` is an error.

## Structured concurrency

`spawn(worker)` accepts a zero-argument function returning `T` and produces
`task[T]`. `join(task)` blocks until that worker finishes and returns its value.
Repeated joins return the same cached value, including the same managed object
identity. Unit-returning workers are supported.

Spawning copies the complete reachable closure graph before the new thread
starts. This preserves aliases and cycles within the copy, including recursive
closures. A worker mutating a captured array or cell changes only its snapshot;
the parent can keep mutating its own copy without a data race. Returning a managed
value copies its reachable graph back on the first join. Each collector touches
only its own thread's objects and roots. Snapshot traversal is iterative.

Native pointer values and existing task handles cannot cross this boundary,
even when nested in arrays, tuples, Results, or captured cells. The compiler
rejects statically visible violations; the runtime checks the actual graph.
This includes pin handles and C string pointers. Code addresses inside closures
refer to immutable executable code and are retained as implementation metadata.
Calling foreign code within a worker still carries the explicit `unsafe`
obligations of that foreign API, including any foreign shared-state rules.

Every language function enters a task scope. Leaving it joins all tasks spawned
in that function before releasing its GC roots, including discarded task handles
and interpreter error unwinding. A returned task handle is already complete when
its spawning function returns. Workers may spawn their own nested tasks. There
are at most 64 outstanding workers across the process; joining releases a slot,
so sequentially spawning and joining many jobs does not exhaust the limit.

The native C runtime interface uses `PFTaskEntry`, a `uint64_t(void*, void*)`
trampoline, to pack a typed worker result into its existing value representation.
Native and JIT backends emit this trampoline; the interpreter supplies one using
its immutable Program. `pf_tasks_enter`/`pf_tasks_leave` must nest in LIFO order.
The C embedding context parameter is trusted host data; language captures always
go through snapshot validation.

Runtime errors in any worker retain the existing process-fatal behavior: a
source diagnostic is printed and the process exits with code 1. They are not
recoverable task values, and exceptions do not unwind through JIT frames. The
OS reclaims process resources on this path. Normal returns and interpreter
exceptions join workers and clean up their heaps. There is no cancellation,
detachment, shared managed heap, scheduler, or implicit thread pool.

`tests/task_tests.cpp` proves simultaneous execution with a two-worker barrier,
isolated mutations, GC during concurrent allocation, graph alias/cycle transfer,
repeat joins, scope cleanup, nested workers, and slot reuse. Fatal transfers,
worker failures, invalid scopes, and the thread limit are checked with subprocess
timeouts in `tests/task_failures.py`.

The focused task runtime suite passed GNU C++ 13.3 AddressSanitizer and
UndefinedBehaviorSanitizer on Ubuntu 24.04/WSL with `-O1 -g
-fsanitize=address,undefined -fno-omit-frame-pointer -pthread`, including leak
detection. The ThreadSanitizer binary compiled, but this host could not start its
runtime (`FATAL: ThreadSanitizer: unexpected memory mapping`); this is not a
race-clean TSan result. Local command flags, source hashes, and outcomes are
recorded in the ignored `out/task-validation/sanitizers.json` artifact.
