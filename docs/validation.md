# Validation record

**Status: implementation and verification in progress.** This record is not a v1
completion declaration. Results below are snapshots of a changing worktree; a
final source revision and final native/JIT test counts will be recorded after
integration settles.

## Recorded evidence

| Host / mode | Snapshot | Evidence |
|---|---|---|
| Windows x64, MSVC 19.52, reference interpreter | Latest recorded interpreter checks | `tests/test_cli.py`: **99 checks passed** |
| Windows x64, frontend | Current frontend suite | Lexer/parser/AST, unsafe/extern syntax, bounded recovery, nesting limits, and 1,000 deterministic malformed inputs passed |
| Windows x64, typed IR | Current IR verifier suite | Signature/type/target/assignment checks and side-effect-destination/captured-direct-call rejection regressions passed |
| Windows x64, generated arithmetic | Earlier current-language snapshot | **30 programs passed**, seed `20261104`, against independent Python arithmetic reference |
| Windows x64, benchmark report tooling | Current report-unit suite | **3 tests passed**; no native performance measurement implied |
| Linux, reference interpreter | Earlier snapshot | **74 CLI checks passed**; this predates the expanded 99-check suite and must not be described as its Linux validation |
| Windows x64, LLVM native/JIT | In progress | LLVM/backend integration and full generated execution checks pending final result |
| Linux x64, LLVM native/JIT | In progress | Requires a compatible LLVM 24 build including the ABI library and host execution tests |
| C ABI generated calls | In progress | Host C fixture and generated JIT/native results must be recorded separately for Windows/Linux |
| Source debugging | Not established | Metadata emission alone does not verify stepping, locals, or debugger integration |
| Benchmarks | Tool available; final run pending | No measured performance advantage is claimed |

The LLVM checkout selected for integration is
`0247623ddadb8d078a446a704cc20f0a8c944fc8` (24.0.0git). The current compiler expects
LLVM major version 24. A final record should identify both the PikaFlow source
revision and the LLVM/toolchain revision used for each host.

## Reproduce checks

```powershell
ctest --test-dir build/interpreter --output-on-failure
python tests/test_cli.py build/interpreter/pikaflow.exe
python tests/fuzz_frontend.py build/interpreter/pikaflow.exe --seed 20261104 --cases 30
python tests/test_benchmark.py
```

For a completed LLVM-enabled build, run CTest in `build/native`; the integration
suite then enables JIT/native `-O0`/`-O2` output comparisons and emission checks.
ABI tests also use a separately compiled host C fixture rather than comparing
generated code only with another generated path.

Passing the reference interpreter does not validate LLVM lowering. Passing LLVM
verification does not prove source-level correctness. Passing one ABI/host does
not establish another host's behavior. Bounded generated-program tests are not a
coverage-guided fuzz campaign.

The manual GitHub workflow has been written and its YAML structure checked. It
has not been dispatched as part of this record; no remote CI result is implied.
