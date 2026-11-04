# Implementation status

PikaFlow is being implemented against the approved hybrid-language design.
An item is complete only after its implementation and relevant checks pass.

## Approved target

- C++ compiler; indentation-based structured syntax plus explicit stack blocks.
- Static types, inferred local types, typed shared operand-stack contracts.
- Functions, recursion, closures, pipelines, arrays, tuples, strings, modules.
- Checked arithmetic, recoverable `Result` values, explicit unsafe C boundary.
- Custom typed IR, reference interpreter, LLVM IR and native object generation,
  optimization, ORC JIT, source debug information.
- Precise non-moving mark/sweep runtime, roots for live managed values.
- Windows x64 and Linux x64, executable regression tests and IR verification.

## Work in progress

The repository currently contains shared compiler interfaces. Frontend, semantic
analysis, runtime, interpreter, LLVM backend, CLI, examples, and verification are
under active implementation. No language feature is yet marked complete.

## Later roadmap

Generics, formatter, language server, package manager, and concurrency follow
the core compiler. Concurrent collection and a borrow checker are not part of
the chosen memory model.
