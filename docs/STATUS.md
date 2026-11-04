# Implementation status

PikaFlow 0.1 has an implemented and tested core compiler. The initial Windows x64
and Linux x86-64 native/JIT integration runs each passed all 13 CTest suites,
including 216 compiler checks and 52 generated C ABI execution comparisons per
host. This establishes the tested first-release core, not completion of the full
approved roadmap.

## Approved target

- C++ compiler; indentation-based structured syntax plus explicit stack blocks.
- Static types, inferred local types, typed shared operand-stack contracts.
- Functions, recursion, closures, pipelines, arrays, tuples, strings, modules.
- Checked arithmetic, recoverable `Result` values, explicit unsafe C boundary.
- Custom typed IR, reference interpreter, LLVM IR and native object generation,
  optimization, ORC JIT, source debug information.
- Precise non-moving mark/sweep runtime, roots for live managed values.
- Windows x64 and Linux x64, executable regression tests and IR verification.

## Implemented and checked core

The repository contains the indentation frontend, static semantic analysis,
structured/typed-stack compiler, typed IR verifier, reference interpreter, traced
runtime, LLVM native/JIT backend, CLI, source standard library and executable
examples. Closures, mutation, Result/destructuring patterns, module privacy,
checked arithmetic and explicit unsafe memory/foreign calls have executable
regression coverage. The LLVM backend includes optimization, native assembly and
object emission, linking and source debug metadata.

The C boundary supports a bounded set of natural-layout flat records on Windows
x64 and Linux x86-64, alongside scalar/pointer calls and supported varargs.
Nested/packed records, unions, bitfields, C++ object ABI and callbacks remain
outside that subset. CodeView line/local records were inspected on Windows;
interactive graphical-debugger stepping and usability are not established.

A bounded frontend libFuzzer campaign completed 64,258 executions under ASan/UBSan
without a reported failure. Native benchmark outputs were verified before three
timed repetitions per configuration; [the raw report](benchmarks/windows-2026-11-04.json)
does not establish a performance advantage.

The recorded implementation is captured by core commit `92e0e38` and backend
commit `8a96379`. Tests ran against captured worktree snapshots during integration;
archive/binary hashes, exact toolchains, local log paths and limits are in the
[validation record](validation.md). See [feature status](FEATURES.md) for the
boundary of each implemented feature and remaining work.

## Later roadmap

Generics, formatter, language server, package manager, and concurrency follow
the core compiler. Concurrent collection and a borrow checker are not part of
the chosen memory model.
