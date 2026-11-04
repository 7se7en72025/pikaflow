# Feature status

PikaFlow's approved project scope is larger than its first implementation.
This file distinguishes available code from remaining work; an approved design
decision is not by itself an implemented or verified feature.

## Available implementation

| Area | Current implementation |
|---|---|
| Frontend | C++17 lexer, indentation parser, source locations, token/AST dumps |
| Language | Static types, locals, explicit signatures, inferred initializers, lexical scopes |
| Values | Checked `i64`, `f64`, booleans, byte strings, arrays, tuples |
| Functions | Named functions, recursion, nested/anonymous closures, function values |
| Hybrid programming | Stack blocks, typed words, multiple results, shared stack within functions, pipelines |
| Control flow | `if`/`elif`/`else`, `while`, range/sequence `for`, `break`, `continue`, destructuring/Result/range/wildcard `match` |
| Modules | Relative imports, explicit exports, private declarations, import-cycle detection |
| Recoverable errors | Typed `Result[T,E]`, `ok`, `err`, `is_ok`, checked `unwrap`/`error`, Result patterns |
| Custom IR | Typed register instructions, control-flow verification, definite-assignment checks, dumps |
| Reference execution | Typed-IR interpreter using the common runtime |
| Runtime | Printing, bounds checks, checked arithmetic, arrays/tuples/closures, traced non-moving GC |
| LLVM backend | LLVM IR generation/verification, optimization pipelines, object emission, native linking, ORC JIT |
| Foreign calls | Scalar/pointer declarations and limited scalar varargs in LLVM execution |
| Unsafe memory | Explicit unsafe blocks, typed raw allocation/load/store/free, C-string exposure, persistent GC root handles |
| Developer CLI | `check`, `interpret`, `run`, `emit-ir`, `emit-obj`, `build`, inspection commands |

The end-to-end integration suite in [`tests/test_cli.py`](../tests/test_cli.py)
checks the actual executable and conditionally enables LLVM tests only when the
binary reports `LLVM enabled`. Interpreter tests alone do not validate native
compilation or JIT behavior. See the project README for the latest recorded build
and test results.

## Remaining approved work

| Target | Boundary of current implementation |
|---|---|
| Fully featured grammar | Syntax exists for the implemented subset; grammar evolution still needs specification/examples |
| Error recovery | Bounded top-level parser recovery exists; within-function semantic recovery does not |
| Rich pattern matching | Tuple/array/Result/range/binding patterns exist; guards and general ADT exhaustiveness checking do not |
| Raw-pointer programming | Checked allocation/load/store/free built-ins exist; arbitrary foreign-address dereference, address-of, and pointer arithmetic do not |
| Full C ABI interoperability | Aggregate/struct lowering, callbacks, and ownership annotations do not; explicit root handles exist |
| Full Result/error system | Typed Result alternatives exist; general user-defined ADTs and propagation syntax do not |
| Broad standard library | Runtime helpers exist; files, networking, collections, and standard-library modules do not |
| Source debugger experience | LLVM debug support must be verified on target debuggers; debugger usability is not established by emitting metadata |
| Linux support | CMake/runtime/backend are intended to be portable; a Windows test run does not validate Linux |
| LLVM main compatibility | One LLVM version/revision is tested at a time; ongoing compatibility requires repeated builds and tests |
| Specialized optimization work | LLVM pipelines exist; substantial project-specific analysis/transform passes require separate correctness tests |
| Production fuzzing | Bounded malformed-input smoke checks exist; coverage-guided long-running fuzz campaigns remain work |
| Generics | Compile-time specialization is planned, not implemented |
| Formatter | Planned after grammar stability |
| LSP | Planned after v1; not implemented |
| Package manager | Planned after module/dependency rules stabilize |
| Concurrency | Post-v1 memory-model, GC, and stack-coordination design; not implemented |

## Completion checks

A feature is complete only when its semantics are documented, valid examples run,
invalid examples diagnose, relevant edge cases are tested, and the required
execution modes agree. LLVM verification checks IR structure; it is not a proof
that the compiler preserved the source program's meaning. Differential tests and
target-specific execution remain necessary.

No performance claim is established merely by enabling `-O2`; benchmarks must
record the source, toolchain revision, build flags, machine, output correctness,
and repeated measurements.
