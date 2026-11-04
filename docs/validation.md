# Validation record

**Status: the recorded Windows/Linux native integration checks passed.** This
record is not a v1 completion declaration. Results below identify tested snapshots
and their limits; the broader approved roadmap remains tracked in
[feature status](FEATURES.md).

## Recorded evidence

| Host / mode | Snapshot | Evidence |
|---|---|---|
| Windows x64, MSVC 19.52, reference interpreter | Latest recorded interpreter checks | `tests/test_cli.py`: **99 checks passed** |
| Windows x64, frontend | Current frontend suite | Lexer/parser/AST, unsafe/extern syntax, bounded recovery, nesting limits, and 1,000 deterministic malformed inputs passed |
| Windows x64, typed IR | Current IR verifier suite | Signature/type/target/assignment checks and side-effect-destination/captured-direct-call rejection regressions passed |
| Windows x64, generated arithmetic | Earlier current-language snapshot | **30 programs passed**, seed `20261104`, against independent Python arithmetic reference |
| WSL Ubuntu 24.04, Clang 18 frontend sanitizers | Bounded libFuzzer snapshot | **64,258 executions / 46 seconds**, no crash artifacts or ASan/UBSan failures; see [commands and limits](fuzzing.md) |
| Windows x64, benchmark report tooling | Current report-unit suite | **3 tests passed**; no native performance measurement implied |
| Linux, reference interpreter | Earlier snapshot | **74 CLI checks passed**; this predates the expanded 99-check suite and must not be described as its Linux validation |
| Windows x64, LLVM native/JIT | Full native CTest snapshot | **13/13 suites passed** in 9.88 s, including **216 CLI integration checks**, **6 contextual typing/callable namespace programs**, **34 runtime failure cases**, and generated arithmetic comparisons |
| WSL Ubuntu 24.04 x86-64, GNU 13.3 Release, LLVM 24 | Full native CTest snapshot | **13/13 suites passed** in 69.22 s, including **216 CLI integration checks**, **6 contextual programs**, **34 runtime failure cases** and **30 differential arithmetic programs** |
| Windows x64, C ABI | Host C fixture plus generated cross-checks | **52 source JIT/native checks** across `-O0`/`-O2`; **12 direct backend record scenarios** at both levels; host C reference calls passed |
| Linux x86-64, C ABI | Host C fixture plus generated cross-checks | **52 source JIT/native checks** across `-O0`/`-O2` and **12 direct backend record scenarios** at both levels passed after the POD register-passing classification fix |
| Windows/Linux, native environment | Automated execution checks | Unicode output paths, adjacent-file preservation and temporary cleanup on success/failure passed on both; Windows additionally passed without developer-shell variables |
| Source debugging | CodeView emission/inspection checked on Windows | Object inspection found line ranges and locals `a`/`b`; interactive stepping and debugger usability remain unverified |
| Windows x64, native benchmarks | Output-verified synthetic snapshot | Two workloads at `-O0`/`-O2`, three timed repetitions each; [raw report](benchmarks/windows-2026-11-04.json); process startup/output capture included, no performance advantage claimed |

The Windows LLVM checkout is
`0247623ddadb8d078a446a704cc20f0a8c944fc8` (24.0.0git), built with MSVC 19.52.
Linux used real LLVM 24.0.0 development packages and the GNU 13.3 host toolchain,
with `LLVM_DIR=/usr/lib/llvm-24/lib/cmake/llvm`. The compiler expects LLVM major
version 24; these results do not establish compatibility with every LLVM main
revision.
The Linux package version was
`1:24~++20261103082040+5883ca2582af-1~exp1~20261103202158.1911`.

The Linux v3 source archive captured checkout `be5092f35755de2ee6e06ad0e5a70a606d572ad7`
plus worktree changes: 70 files, SHA-256
`b47198565558f47f6481c238c6a206c0114e074643d1e40f66bc0cfedef4e87c`.
The core semantic changes are committed in `92e0e38`; the coordinated backend/C
ABI changes are committed in `8a96379`. The Linux compiler identified itself as
`PikaFlow 0.1.0; LLVM enabled; x86_64-pc-linux-gnu`, with binary SHA-256
`978c32da60e0c4cebc6187c7b0640ccc12b43ceec29640950b2f5534eefa85ef`.
The Linux
snapshot metadata and full CTest log are retained locally under
`out/linux-validation/snapshot-native-v3.json` and
`out/linux-native-validation/build/Testing/Temporary/LastTest.log`; the Windows
log is `build/native/Testing/Temporary/LastTest.log`. These ignored local build
artifacts are not part of a clean-checkout reproducibility claim. The benchmark
report separately records its exact Windows compiler binary SHA-256.
The Windows CodeView object inspection is retained locally in
`out/debug-validation/codeview.txt`; readable line records and source locals
were checked, without claiming interactive debugger validation.

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
The separately recorded libFuzzer run supplies bounded coverage-guided frontend
evidence; it does not validate source semantics, lowering or runtime behavior.

The manual GitHub workflow has been written and its YAML structure checked. It
has not been dispatched as part of this record; no remote CI result is implied.
