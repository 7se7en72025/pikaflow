# Implementation status

PikaFlow **0.3.1** is a corrective release of the 0.3 contract. It preserves
compound assignment tokens during formatting, rejects malformed LSP messages,
bounds syntax/semantic traversal, prints deep managed graphs iteratively, fixes
typed-IR validation gaps and protects adjacent files during MSVC native linking.
The frozen clean `d77a3b5` passed **25/25 Windows native** (156.67 s), **21/21
Windows reference** (38.22 s) and **26/26 Linux native** (214.63 s) suites.
The native suites include 139 language, 61 namespace, 115 optimizer, 19 editor,
16 developer-tools and 10 Git package cases, plus 216 Windows / 217 Linux
compiler checks. Linux retains one internal Windows-only junction test skip;
no CTest suite was skipped. Exact identities and the preserved earlier results
are in [validation.md](validation.md).

The 0.3.1 parser campaign completed 48,721 executions with no reported finding.
Focused runtime/IR ASan/UBSan runs also passed; their original dirty-worktree
manifest is retained with source hashes matching the frozen implementation.
Neither bounded run establishes a correctness proof. The output-verified
[0.3.1 benchmark](benchmarks/windows-0.3.1-2026-11-05.json) retains three
process-inclusive samples per configuration and makes no performance claim.

## Historical 0.3 baseline

PikaFlow 0.3 includes generic recursive ADTs, guarded/exhaustive patterns, Result
propagation, isolated threaded tasks, C interop and developer tools. It adds named
module aliases, single-evaluation compound updates, transactional cross-function
diagnostics, typed-IR dataflow optimization, compiler semantic navigation/rename
and exact-commit Git package restoration. These are implemented source paths with
focused regression tests; complete host-suite results are recorded by frozen
revision in [validation.md](validation.md).

The frozen clean `5f0b109` release passed **25/25 Windows native**, **21/21
Windows reference** and **26/26 Linux native** CTest suites. Native checks on
each host include 216 compiler, 133 language, 61 namespace/assignment, 115
optimizer execution, 19 editor workflow and 10 Git package cases, alongside C
ABI, tasks, runtime failures, installed layout and source-binding verification.
Linux also ran actual GDB tests; its tools suite internally skips one
Windows-only junction unittest. Exact counts and identities are in
[the 0.3 validation record](validation.md).

The clean `39fcac3` results are the historical 0.2 baseline. Their 19/19 Windows
native, 15/15 reference and 20/20 Linux suites are retained as history, not reused
as an automatic full-suite claim for later source revisions.

## Approved target

- C++ compiler; indentation-based structured syntax plus explicit stack blocks.
- Named module namespaces, source binding metadata and single-evaluation updates.
- Static types, inferred local types, typed shared operand-stack contracts.
- Functions, recursion, closures, pipelines, arrays, tuples, strings, modules.
- Inferred/explicit generic specialization, recursive nominal enums and guarded
  exhaustive matches.
- Checked arithmetic, recoverable `Result` values, explicit unsafe C boundary.
- Custom typed IR, reference interpreter, LLVM IR and native object generation,
  CFG/dataflow optimization, ORC JIT, source debug information.
- Precise non-moving mark/sweep runtime, roots for live managed values.
- Isolated real-thread tasks, formatter, compiler-backed LSP navigation/rename,
  local/exact-commit Git packages and integrity locks.
- Windows x64 and Linux x64, executable regression tests and IR verification.

## Implemented and checked core

The repository contains the indentation frontend, static semantic analysis,
structured/typed-stack compiler, typed IR verifier, reference interpreter, traced
runtime, LLVM native/JIT backend, CLI, source standard library and executable
examples. Closures, mutation, Result/destructuring patterns, module privacy,
checked arithmetic, generics, recursive enum graphs and explicit unsafe memory/foreign calls have executable
regression coverage. The LLVM backend includes optimization, native assembly and
object emission, linking and source debug metadata.
The custom IR optimizer propagates constants over executable CFG edges, preserves
checked failures/effects and verifies its output before LLVM lowering. Namespace,
assignment-order and semantic-index tests cover private exports, qualified generic
types/calls, captures, shadowing, writes and exact identifier positions.

The C boundary supports natural-layout nested records, scalar noncapturing
callbacks, scalar/pointer calls, explicit integer boundary widths and supported
varargs on the x86-64 targets. Packed records, unions, bitfields, C++ object ABI
and managed/captured callbacks remain outside that subset. Headless GDB tests
exercise real breakpoints, stepping, backtraces and mutated/captured scalar
watches; Windows CodeView line/local records are also inspected. Graphical IDE
integration, managed-value visualization and optimized-code debugger quality
remain unverified.

The historical 0.3 parser libFuzzer campaign completed 79,589 executions in
46 seconds under ASan/UBSan without a reported finding. Its scope and exact
parser snapshot are recorded in [fuzzing.md](fuzzing.md); semantic-index changes
are covered by source/editor regressions rather than that parser harness.
The [0.3 native benchmark report](benchmarks/windows-0.3-2026-11-04.json) records
verified outputs and three process-inclusive samples per configuration from the
clean frozen release. It establishes no performance advantage.

The earlier clean `6ca5992` validation/benchmark/install records establish the
0.1 baseline, not automatic verification of every later extension. The language
source cases compare interpreter/JIT/native outputs at `-O0`/`-O2`,
alongside independent host-C fixtures, threaded runtime tests, debugger sessions
and protocol/tool subprocess tests. Final source/archive/binary hashes and
host-specific suite results are maintained in [validation.md](validation.md).

## Delivered tooling and task contracts

The formatter preserves real compiler tokens. The language server supports
incremental changes, compiler-backed hover/definition/references and verified
capture-avoiding rename alongside diagnostics, formatting and completion.
Grouped project contexts keep package/name resolution scoped to the appropriate
manifest. Its advertised subset and limits are documented in [editor.md](editor.md).
Packages use local paths or exact Git commits, deterministic locks and explicit
fetch/offline verification, without a registry, version solver or install hooks.
Real worker threads copy capture/result graphs between
isolated heaps and join at function-scope exit; they do not share managed mutable
state. Concurrent collection and a borrow checker are not part of this model.

The [integrated showcase](../examples/showcase/main.pf) combines imported generic
recursive trees, exhaustive guarded patterns, Result `?`, a worker with GC,
typed stack operations, a standard-library word and a pipeline. It prints `43`,
`36` and `negative seed` in that order.

See [FEATURES.md](FEATURES.md) for the complete supported-feature boundaries.
