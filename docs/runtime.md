# Runtime and garbage collection

The runtime implements a precise, non-moving, mark-and-sweep collector. Heap
objects are strings, arrays, tuples, mutable cells, and closures. Result values
are tuples containing a success boolean, a payload, and an error value. Both
payload and error types may contain managed references. Extracting the wrong
variant fails; unwrapping a string error reports that message, while other error
types produce a generic diagnostic.

All allocations belong to the current thread's runtime heap. The runtime does
not provide cross-thread object sharing or a concurrency API.

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
- Managed tags 4 through 8 and tag 10 are traced. Native pointer tag 9 is not.
  Native code retaining an object must retain an explicit registered root.

Array, tuple, cell, and closure references are traversed during collection.
Unreachable cycles are reclaimed. The tracing worklist is iterative. Strings
are immutable UTF-8 byte sequences; length and indexing count bytes, and string
indexing returns an unsigned byte integer.

## Failure behavior

Invalid bounds, incompatible runtime tags, expired managed references, integer
overflow, division by zero, failed Result unwrapping, and invalid GC frame order
produce a diagnostic on standard error and terminate with exit code 1. Errors
do not unwind C++ exceptions through JIT-generated code. Integer division
truncates toward zero. Converting a finite, in-range floating-point value to
`i64` also truncates toward zero.

The native runtime test executable checks arithmetic boundaries, reachability,
cycles, closures, UTF-8 indexing, Result references, and printable values.
`tests/runtime_failures.py` checks fatal failures in subprocesses.

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
scanned by the garbage collector.

Foreign pointers have no runtime allocation metadata. Their validity, bounds,
aliasing, and lifetime remain the caller's unsafe responsibility. Stale pointers
cannot reliably be distinguished from foreign or subsequently reused addresses;
the runtime does not claim use-after-free protection.

`pf_cstr` exposes a managed string's read-only NUL-terminated bytes. Embedded NUL
bytes are preserved, but C string functions observe the usual first-NUL
termination. The original string must stay rooted while native code uses those
bytes; the returned native pointer is not a GC root.

`pf_pin` creates a distinct opaque persistent-root handle. The collector retains
the managed object and its reachable children until every corresponding handle
has been released with `pf_unpin`. Handles are native pointers, not managed
objects; passing a released or unrecognized handle to `pf_unpin` is an error.
