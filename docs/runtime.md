# Runtime and garbage collection

The runtime implements a precise, non-moving, mark-and-sweep collector. Heap
objects are strings, arrays, tuples, mutable cells, and closures. Result values
are tuples containing a success boolean, a payload, and an error string.

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
