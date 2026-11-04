# Implementation status

PikaFlow 0.2 extends the tested core with generic specialization, recursive ADTs,
guarded/exhaustive patterns, Result propagation, isolated threaded tasks, nested
C records/scalar callbacks/integer-width annotations, and working formatter,
editor and local-package tools. Clean release commit `39fcac3` passed **19/19
Windows native**, **15/15 Windows reference** and **20/20 Linux native** suites.
Both native hosts passed 216 compiler checks, 133 language-extension checks,
120 C ABI executions plus 40 located range failures, and 50 task executions.
Linux also passed the real GDB test. Exact toolchain/source/binary identities,
the Windows-only skipped Linux tools case, and complete counts are recorded in
[validation.md](validation.md).

## Approved target

- C++ compiler; indentation-based structured syntax plus explicit stack blocks.
- Static types, inferred local types, typed shared operand-stack contracts.
- Functions, recursion, closures, pipelines, arrays, tuples, strings, modules.
- Inferred/explicit generic specialization, recursive nominal enums and guarded
  exhaustive matches.
- Checked arithmetic, recoverable `Result` values, explicit unsafe C boundary.
- Custom typed IR, reference interpreter, LLVM IR and native object generation,
  optimization, ORC JIT, source debug information.
- Precise non-moving mark/sweep runtime, roots for live managed values.
- Isolated real-thread tasks, formatter, scoped LSP support and local package locks.
- Windows x64 and Linux x64, executable regression tests and IR verification.

## Implemented and checked core

The repository contains the indentation frontend, static semantic analysis,
structured/typed-stack compiler, typed IR verifier, reference interpreter, traced
runtime, LLVM native/JIT backend, CLI, source standard library and executable
examples. Closures, mutation, Result/destructuring patterns, module privacy,
checked arithmetic, generics, recursive enum graphs and explicit unsafe memory/foreign calls have executable
regression coverage. The LLVM backend includes optimization, native assembly and
object emission, linking and source debug metadata.

The C boundary supports natural-layout nested records, scalar noncapturing
callbacks, scalar/pointer calls, explicit integer boundary widths and supported
varargs on the x86-64 targets. Packed records, unions, bitfields, C++ object ABI
and managed/captured callbacks remain outside that subset. Headless GDB tests
exercise real breakpoints, stepping, backtraces and mutated/captured scalar
watches; Windows CodeView line/local records are also inspected. Graphical IDE
integration, managed-value visualization and optimized-code debugger quality
remain unverified.

A bounded 0.2 frontend libFuzzer campaign completed 80,430 executions in 49 seconds
under ASan/UBSan without a reported failure. Native benchmark outputs were verified before three
timed repetitions per configuration; [the 0.2 raw report](benchmarks/windows-0.2-2026-11-04.json)
does not establish a performance advantage.

The earlier clean `6ca5992` validation/benchmark/install records establish the
0.1 baseline, not automatic verification of every later extension. The 0.2
language source cases compare interpreter/JIT/native outputs at `-O0`/`-O2`,
alongside independent host-C fixtures, threaded runtime tests, debugger sessions
and protocol/tool subprocess tests. Final source/archive/binary hashes and
host-specific suite results are maintained in [validation.md](validation.md).

## Delivered tooling and task contracts

The formatter preserves real compiler tokens. The language server advertises
only its implemented document-level capabilities. Package management uses local
path dependencies and deterministic integrity locks, without a network registry
or publishing service. Real worker threads copy capture/result graphs between
isolated heaps and join at function-scope exit; they do not share managed mutable
state. Concurrent collection and a borrow checker are not part of this model.

The [integrated showcase](../examples/showcase/main.pf) combines imported generic
recursive trees, exhaustive guarded patterns, Result `?`, a worker with GC,
typed stack operations, a standard-library word and a pipeline. It prints `43`,
`36` and `negative seed` in that order.

See [FEATURES.md](FEATURES.md) for the complete supported-feature boundaries.
