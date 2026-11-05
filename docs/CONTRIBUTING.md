# Working on PikaFlow and learning LLVM

Start with the [build instructions](../README.md), then run the full CTest suite
for the build you changed. An interpreter-only build is useful for fast frontend
work; it cannot establish native lowering, C ABI or source-debugger correctness.
For a quick Windows edit/build/check loop, use
`scripts/build.ps1 -InterpreterOnly -BuildOnly -Jobs 2`, then the affected focused
test. `-BuildOnly` deliberately skips CTest; run the complete suite once the
change is integrated.

## Follow one program through the compiler

```powershell
build/native/pikaflow.exe dump-tokens examples/hello.pf
build/native/pikaflow.exe dump-ast examples/hello.pf
build/native/pikaflow.exe dump-ir examples/hello.pf
build/native/pikaflow.exe dump-ir examples/hello.pf -O2
build/native/pikaflow.exe opt-report examples/optimization_lab/main.pf
build/native/pikaflow.exe symbols examples/modules/qualified.pf
build/native/pikaflow.exe emit-ir examples/hello.pf -O0 -o build/hello-O0.ll
build/native/pikaflow.exe emit-ir examples/hello.pf -O2 -o build/hello-O2.ll
build/native/pikaflow.exe dump-asm examples/hello.pf -O2
build/native/pikaflow.exe interpret examples/hello.pf
build/native/pikaflow.exe run examples/hello.pf -O2
build/native/pikaflow.exe build examples/hello.pf -O2 -o build/hello.exe
build/hello.exe
```

On Linux, omit `.exe` and prefix executable paths with `./`. Compare the output
of independent execution paths before studying optimization differences.

| Stage / question | Source to read | Relevant checks |
|---|---|---|
| How are indentation, literals and source spans recognized? | [lexer.cpp](../src/lexer.cpp) | [frontend tests](../tests/frontend_tests.cpp), [libFuzzer harness](../tests/fuzz_parser.cpp) if present; see [fuzzing](fuzzing.md) |
| How do precedence, structured code and stack blocks become syntax nodes? | [parser.cpp](../src/parser.cpp), [frontend.hpp](../include/pikaflow/frontend.hpp) | Frontend valid/invalid cases; token-preserving formatter tests |
| Where do names, modules, types, stack effects and closures get checked? | [compiler.cpp](../src/compiler.cpp) | [CLI tests](../tests/test_cli.py), [language extensions](../tests/test_language_extensions.py) |
| Do namespaces and indexed updates preserve visibility/evaluation order? | Compiler resolvers/assignment lowering and parser lvalues | [Namespace/assignment tests](../tests/test_namespaces_assignments.py) |
| How do source names retain identity through shadows, captures and generics? | Optional compiler semantic source pass and AST name locations | [Semantic index tests](../tests/test_semantic_symbols.py), [editor tests](../tests/test_editor.py) |
| What does our own IR guarantee before LLVM sees it? | [ir.hpp](../include/pikaflow/ir.hpp), [ir.cpp](../src/ir.cpp) | [Malformed IR tests](../tests/ir_tests.cpp) |
| How do fresh cells, constants, value identities and live results drive our own optimization? | [optimizer.cpp](../src/optimizer.cpp), [optimizer.hpp](../include/pikaflow/optimizer.hpp) | [Optimizer unit tests](../tests/optimizer_tests.cpp), [cross-mode optimizer tests](../tests/test_optimization.py), [pass description](optimization.md) |
| How can optimization be inspected without executing a source program? | CLI report serialization and protected text output in [main.cpp](../src/main.cpp) | [Report CLI tests](../tests/test_opt_report.py), [optimization lab](../examples/optimization_lab/README.md) |
| How do instrumented semantic inputs drive compiler/optimizer coverage? | [Semantic harness](../tests/fuzz_semantic.cpp) | [Instrumentation, five families and oracle contract](fuzzing.md) |
| How do records and aliases preserve type/field identity? | Compiler type expansion, nominal registry and source field resolution | [Record tests](../tests/test_records.py), [alias tests](../tests/test_type_aliases.py), [record project](../examples/record_project/README.md) |
| How does the same LLVM pass run in the compiler and `opt`? | [Shared pass](../src/llvm/CheckedArithmeticPass.cpp), [plugin entry](../src/llvm/PassPlugin.cpp) | [Plugin contract](llvm-pass-plugin.md), [FileCheck fixtures](../tests/llvm/checked_simplify.ll), Linux `opt`/`lli`/`llc` tests |
| How can we execute without relying on LLVM lowering? | [interpreter.cpp](../src/interpreter.cpp) | [Interpreter tests](../tests/interpreter_tests.cpp), [arithmetic differential tests](../tests/fuzz_frontend.py) |
| How do C++ LLVM APIs build types, instructions and control flow? | [backend.cpp](../src/backend.cpp) | [Backend tests](../tests/backend_tests.cpp); inspect emitted O0 IR |
| How are object generation, target layout, passes and ORC connected? | [backend.cpp](../src/backend.cpp), [CMakeLists.txt](../CMakeLists.txt) | Native/JIT O0/O2 and object/assembly tests |
| Who owns managed objects, GC roots and task snapshots? | [runtime.cpp](../src/runtime.cpp), [runtime.h](../include/pikaflow/runtime.h) | [Runtime tests](../tests/runtime_tests.cpp), [task tests](../tests/task_tests.cpp) |
| Does a foreign aggregate really match the host ABI? | Backend C ABI helpers and [host C fixture](../tests/ffi_fixture.c) | [Generated C comparisons](../tests/test_ffi.py); run separately on both hosts |
| Do debug records actually let a debugger inspect values? | Backend DIBuilder/variable declarations | [Real GDB checks](../tests/test_debugger.py), not only IR metadata checks |
| How do tools preserve unsaved sources and reject rename capture? | [LSP](../tools/lsp.py), [semantic editor operations](../tools/editor.py) and compiler overlays | [Tool tests](../tests/test_tools.py), editor tests and [editor contract](editor.md) |
| How do resolved names become semantic colors and inferred type hints? | [Read-only editor features](../tools/editor_features.py) | [Real compiler/protocol tests](../tests/test_editor_features.py), [editor contract](editor.md) |
| How are project tests isolated and reported? | [Test runner](../tools/project_tests.py), [Windows jobs](../tools/process_jobs.py), [assertions](../stdlib/testing.pf) | [Runner tests](../tests/test_project_tests.py), [testing workflow](testing.md) |
| Can an independently calculated result expose a shared backend mistake? | [Seeded program generator](../tests/fuzz_programs.py) | [Five-template oracle and replay contract](program-testing.md) |
| How are Git dependency blobs pinned, verified and restored without hooks? | [Package manager](../tools/packages.py), [Git source reader](../tools/git_sources.py) | [Real local-Git fixture tests](../tests/test_packages.py), [workflow](tooling.md) |

PikaFlow's typed register IR is not LLVM SSA. LLVM lowering initially stores
virtual registers in local slots; optimization may promote them and introduce
SSA joins. Inspect an `if`/loop at O0 and O2 to see that transition. A verifier
success means an IR is structurally valid, not that source semantics are correct.

For rename changes, compiler checking and reindexing alone can agree on the same
incorrect resolver. Add independent fixture tests that execute the original and
renamed programs and compare output. Cover legal `_` bindings versus pattern
wildcards and constructor/function/value precedence in each AST context; the
language server must not execute user programs as part of a rename request.

## Exercise project and LLVM-pass workflows

```powershell
python tools/pikaflow.py --compiler build/native/pikaflow.exe test --project examples/record_project --backend interpret
python tests/fuzz_programs.py build/native/pikaflow.exe --cases 10 --seed 20261105 --artifacts out/program-failures
```

On a matching Linux LLVM 24 plugin host, build `pikaflow_checked_passes` and run
the [documented `opt` pipeline](llvm-pass-plugin.md). Read the producer contract
before marking a runtime helper; an incorrect attribute can authorize an invalid
transformation even when LLVM verification succeeds. v1 promises only safe
identities; v2 additionally promises exact, effect-free successful arithmetic
and authorizes checked APInt constant folds. Retain overflow/division failures
and legacy effectful nonidentity cases in the tests. Standard LLVM remarks must
remain quiet by default; test source-located passed/missed YAML and identical
IR/runtime behavior with remarks enabled and disabled.

For scalar-cell forwarding, cover alias writes, overwritten representatives,
escaping calls/closures, CFG boundaries and the 4,096-cell cache reset. Verify
that cell allocations/stores, debug bindings, roots and checked failures remain;
CSE counts alone cannot establish legality. [Optimization contract](optimization.md)

Sanitizer resource checks should retain ordinary host stack limits. The 0.4
candidate's valid generic-chain failure required reducing expression-dispatch
frame size, followed by a clean default-8-MiB rerun. Raising the stack for
diagnosis did not count as the release fix. [Validation history](validation.md)

Use the [supported sanitizer profile](sanitizers.md) for backend/runtime changes.
Retain actual compile-command proof, the matching LLVM SDK cache/library/source
identity, positive private fault-probe reports and separate unexpected-report
scans. The profile does not add checks to all generated instructions. Preserve
upstream SDK defaults without introducing project-wide suppressions; the narrow
GDB inferior-only LSan exception must not disable compiler-child leak checks.

## Make a change reviewable

1. Specify the observable behavior and the invalid form before modifying a shared
   type/opcode/runtime interface. Check interpreter and LLVM callers together.
2. Add a small valid program and a meaningful negative/edge case. For arithmetic,
   preserve overflow/division behavior at every optimization level.
3. For managed values, exercise collection while values cross function/closure,
   task, aggregate-return or foreign boundaries. A passing small example can hide
   a missing root until allocation pressure rises.
4. Run the affected focused test first, then the complete suite once integrated.
   Cross-platform ABI changes require actual host C comparisons on each target.
5. Document the language/runtime contract and its boundary. Commit the code and
   its evidence together; never commit generated build trees or credentials.

Use the supplied `.clang-format` style for C++. The language formatter preserves
PikaFlow tokens; Python tools require no pip packages. A useful commit message
describes behavior, such as “Preserve managed nested records across C returns”.

## Good LLVM learning investigations

- Trace one captured mutable variable from AST to cell/root to LLVM load/store.
- Inspect the legality of the project's checked-arithmetic identity transform.
  Build counterexamples involving overflow before adding a new transform.
- Trace the custom IR's incoming register facts through a loop/backedge and
  compare `dump-ir -O0`, `dump-ir -O2` and emitted LLVM IR.
- Compare Windows and SysV parameter/return lowering for one nested C record.
- Trace how an ORC symbol resolves a runtime function and a generated C callback.
- Follow an isolated task's snapshot, worker thunk, result import and GC roots.
- Use a source debugger to inspect both a normal local and a captured cell.
- Trace one qualified identifier from its exact source token to a compiler
  binding ID, editor reference and capture-checked rename candidate.

These are investigations on implemented code, not claims that completing them
automatically qualifies a contribution for LLVM upstream. Upstream changes need
their own relevant LLVM test, review and project-specific requirements.

The live [project plan](PROJECT_PLAN.md) records timestamps, estimates and the
completion evidence; [feature status](FEATURES.md) defines supported boundaries.
