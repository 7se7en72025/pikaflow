# Feature status

PikaFlow 0.3 implements the language, runtime and developer-tool contracts
below. The boundaries are part of the release contract. Integration results are
recorded by exact source/toolchain snapshot in [validation.md](validation.md);
an implemented feature is not a claim of unlimited platform or protocol coverage.

## Available implementation

| Area | Current implementation |
|---|---|
| Frontend | C++17 lexer, indentation parser, source locations, token/AST dumps |
| Language | Static types, locals, explicit signatures, inferred initializers, lexical scopes |
| Values | Checked `i64`, `f64`, booleans, byte strings, arrays, tuples |
| Functions | Named functions, recursion, nested/anonymous closures, function values, inferred/explicit generic monomorphization and typed generic words |
| User-defined types | Nominal generic enums, constructors, recursive/mutually recursive fields, traced cyclic values, verified canonical layout registry |
| Hybrid programming | Stack blocks, typed words, multiple results, shared stack within functions, pipelines |
| Control flow | `if`/`elif`/`else`, `while`, range/sequence `for`, `break`, `continue`, nested patterns, guarded cases and bounded exhaustive enum matching |
| Modules | Direct and named imports with qualified functions/generics/types/constructors/words/externs; relative, `std:` and local `pkg:` paths; privacy and cycle checks |
| Recoverable errors | Typed `Result[T,E]`, constructors/accessors, Result patterns and postfix `?` early error propagation |
| Custom IR | Typed register instructions, control-flow verification, definite-assignment checks, dumps |
| Typed-IR optimization | Verified CFG integer/boolean constant propagation, safe arithmetic/branch folding, unreachable-instruction removal and bounded fallback |
| Reference execution | Typed-IR interpreter using the common runtime |
| Runtime | Printing, bounds checks, checked arithmetic, arrays/tuples/closures, traced non-moving GC |
| Concurrency | Real OS-thread `spawn`/`join`, typed task handles, isolated capture/result graph copies, automatic scoped joins, independent worker heaps |
| LLVM backend | LLVM IR generation/verification, optimization pipelines, a checked-arithmetic identity transform, object emission, native linking, ORC JIT |
| Foreign calls | Scalar/pointer and nested natural C records, explicit 8/16/32/64-bit integer boundary annotations, varargs, scalar noncapturing callbacks and symbol aliases |
| Unsafe memory | Explicit unsafe blocks, typed raw allocation/load/store/free/offset, checked owned-block offsets, C-string exposure and persistent GC roots |
| Source standard library | Recoverable numeric helpers, integer sequences/sorting/search, checked decimal parsing, ASCII classification |
| Developer CLI | `check`, `interpret`, `run`, `emit-ir`, `emit-obj`, `build`, inspection commands and optional compiler-resolved source-binding JSON |
| Formatter | Compiler-token-equivalent formatting with atomic replacement, refusal of invalid source, check mode and idempotence tests |
| Editor integration | Incremental/full LSP sync, compiler-backed unsaved diagnostics, hover/definitions/references/verified rename, formatting, symbols/completion and grouped project contexts |
| Packages | Project initialization, local paths and exact Git pins, deterministic content locks, explicit restore, verified offline builds and source confinement |
| Source debugging | Generated locals/lines plus headless GDB breakpoint, stepping, backtrace and scalar/captured-local watch tests; Windows CodeView inspection |

The end-to-end integration suite in [`tests/test_cli.py`](../tests/test_cli.py)
checks the actual executable and conditionally enables LLVM tests only when the
binary reports `LLVM enabled`. Interpreter tests alone do not validate native
compilation or JIT behavior. See the project README for the latest recorded build
and test results.

## Supported limits and future extensions

| Target | Boundary of current implementation |
|---|---|
| Error recovery | Bounded parser and cross-function semantic recovery with rollback; lexical/declaration/signature phases remain fail-fast, and one function body stops at its first semantic error |
| Generic type system | Module-level specialization with concrete operation checks; no traits, generic local declarations or runtime polymorphic dispatch |
| Pattern exhaustiveness | Finite constructor/tuple/bool/Result combinations are analyzed; unrestricted integer/string/array payloads need a binding or wildcard; guards establish no coverage |
| Raw-pointer programming | `offset` supports typed/byte offsets; foreign-address validity and lifetime remain unsafe caller obligations; no address-of syntax or use-after-free guarantee |
| C ABI interoperability | Natural x86-64 records and scalar callbacks; no packed records, unions, bitfields, C++ objects, captured callbacks or managed/record callback signatures |
| Source debugger experience | Automated GDB and CodeView evidence is target-specific; graphical IDE integration and every debugger are unverified |
| Additional platforms | Windows x64 and Linux x86-64 are the supported targets; other architectures/ABIs are unverified |
| LLVM main compatibility | One LLVM version/revision is tested at a time; ongoing compatibility requires repeated builds and tests |
| Optimization | Typed-IR constant/CFG analysis and LLVM pipelines/checked identities; no heap/interprocedural propagation or floating-point folding in the custom pass |
| Production fuzzing | A bounded Clang ASan/UBSan/libFuzzer frontend campaign passed; long-running campaigns and semantic/runtime fuzzing remain work |
| Formatter | Normalizes whitespace while preserving compiler tokens; no expression/comment reflow |
| LSP | Compiler-backed navigation/rename within bounded project/workspace contexts; no claim of all LSP methods, remote documents or every editor integration |
| Packages | Local dependencies and exact-commit Git restoration; no registry service, publishing service, version solver or install hooks |
| Concurrency | Snapshot-isolated real threads and structured joins; no shared managed heap, cancellation, detachment, scheduler or thread pool |

Expansion limits, including generic/type/layout and pattern-analysis budgets, are
specified in [language extensions](language-extensions.md). Worker limits and
pointer/GC ownership are specified in [runtime.md](runtime.md). The scoped editor
and package contracts are in [editor.md](editor.md) and [tooling.md](tooling.md); C ABI limits are in
[ffi.md](ffi.md).

## Completion checks

A feature is complete only when its semantics are documented, valid examples run,
invalid examples diagnose, relevant edge cases are tested, and the required
execution modes agree. LLVM verification checks IR structure; it is not a proof
that the compiler preserved the source program's meaning. Differential tests and
target-specific execution remain necessary.

No performance claim is established merely by enabling `-O2`; benchmarks must
record the source, toolchain revision, build flags, machine, output correctness,
and repeated measurements.

The source library is documented in [`stdlib/README.md`](../stdlib/README.md).
It provides a bounded set of numeric, sequence, and byte-string operations; it
does not claim a file/networking/package ecosystem. Additional modules need
their own specified scope and tests.

`tests/fuzz_frontend.py` generates deterministic arithmetic programs in both
structured and stack syntax and compares interpreted results with a Python
reference, including signed division/remainder semantics. It complements the
malformed-input parser checks; neither is a coverage-guided fuzzing campaign.
The separate [libFuzzer target and recorded campaign](fuzzing.md) exercise real
lexer/parser coverage under ASan and UBSan.
