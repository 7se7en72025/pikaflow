# Validation record

## 0.3 clean release validation

The frozen implementation at **`5f0b10915013d38d834ad72dca785bf8f9f8e4fa`**
passed the complete host suites below. Both hosts used clean source
(`dirty: false`) with the same archive SHA-256:
`69d25facc34387e4b92ee515605162a10ddc852b17ca02d0eeb33d2a0d8cc41f`.
The tracked records are exact copies of the captured JSON, including source,
binary, toolchain and log identities. Earlier release records remain below as
history; they do not substitute for these checks.

| Host / mode | Result | Tracked record |
|---|---|---|
| Windows x64, MSVC 19.52 / LLVM 24.0.0git | **25/25 native suites passed**, 98.00 s | [Windows 0.3 manifest](validation/windows-0.3-2026-11-04.json) |
| Windows x64, MSVC 19.52, LLVM disabled | **21/21 reference suites passed**, 40.80 s | [Reference manifest](validation/windows-reference-0.3-2026-11-04.json) |
| Ubuntu 24.04 x86-64 / WSL2, GNU 13.3 / LLVM 24.0.0 | **26/26 native suites passed**, 255.54 s | [Linux 0.3 manifest](validation/linux-0.3-2026-11-04.json) |

Windows used CMake-selected Python 3.14.2; Linux used Python 3.12.3. The Linux
archive's 118 source files still matched after testing, with no source changes.
No CTest suite was skipped on either host. Within Linux's developer-tools suite,
one Windows-only junction unittest was skipped. The Windows reference build was
started at `a6fd04c`; the only source drift to the frozen revision was
`docs/fuzzing.md`. Its manifest records verified code/test input identities.

The LLVM-enabled suites on each host include:

| Check group | Passed checks |
|---|---:|
| Compiler integration | 216 |
| Generic/recursive ADT/guard/Result/showcase language cases | 133 |
| Namespace, assignment order and resolution cases | 61 |
| Typed-IR optimization execution comparisons | 115 |
| Typed-IR optimizer and semantic-symbol unit suites | Passed |
| Exact-commit Git package tests | 10 |
| Editor workflow tests | 19 |
| Developer-tools unittests | 14; Linux internally skips one Windows-only case |
| Generated C ABI JIT/native execution comparisons | 120 |
| Located C integer range failures | 40 |
| Invalid callback declarations | 3 rejected |
| Threaded task/pointer executions | 50 |
| Invalid task/pointer source programs | 4 rejected |
| Fatal runtime cases / fatal task cases | 47 / 10 |
| Arithmetic differential programs / contextual typing programs | 30 / 6 |

Source cases compare reference interpretation, JIT and native output at `-O0`
and `-O2` where applicable. Host C fixtures independently check ABI expectations.
These counts describe check groups, which can overlap CTest organization; they
are not a sum of independent CTest executables. The reference manifest records
its smaller execution counts separately: 99 compiler, 53 language, 21 namespace,
23 optimizer and 10 task execution checks, alongside the shared tool and failure
checks.

The extra Linux suite exercises actual GDB source breakpoints, stepping,
backtraces and mutated/captured scalar watches. Windows CodeView inspection is
separate evidence. Managed-value visualization, graphical IDE integration and
optimized-code debugger quality remain unverified. Installed-layout and
clean-environment native-linking checks passed on both hosts.

### Corrections covered by the final regressions

An earlier Python 3.14 editor test exposed reliance on the JSON decoder's
recursion threshold. The protocol now checks a **128-container nesting limit**
before parsing, with boundary tests; targeted editor checks also passed with
Python 3.11. That earlier failed candidate is not the release result above.

Review also found that a compiler check and a candidate binding graph could
agree on an incorrect reference. The final index keeps legal `_` value and type
bindings separate from match wildcards, and resolves constructors, function
values, explicit specializations, pipelines, callbacks and stack words according
to their actual compilation context. Qualified underscore constructors have
parser regressions. The 19 editor tests include constructor/local shadowing and
constructor/imported-function conflicts: independent tests execute original and
renamed fixture programs and compare output. The language server itself does
not execute user programs during rename. These checks complement its compilation
and binding-graph validation; they are not a claim that every future rename bug
is impossible. See [editor contracts](editor.md) and [contribution checks](CONTRIBUTING.md).

### Identities, fuzzing, benchmark and local package

| Compiler | SHA-256 |
|---|---|
| Windows native | `0ef879d8c236c91a850c4e10234a674080d548ff57725a127d77f4bc86e4e778` |
| Windows reference | `bc7effad51327782faab69456a81e90fb61858298655e388796be3fe912256e7` |
| Linux native | `a6c60117650f2f856d589ddb572118567b6f76d0ca15d0862ca2e3baa816e6d1` |

Windows uses LLVM source `0247623ddadb8d078a446a704cc20f0a8c944fc8`;
Linux uses LLVM 24.0.0 nightly `5883ca2582af`. The Linux record retains exact
configure/build/test commands, per-suite results, toolchain outputs and log
hashes. These identities do not imply bit-for-bit build reproducibility or
compatibility with every LLVM revision.

The latest bounded [parser fuzz campaign](fuzzing.md) completed **79,589 executions
in 46 seconds** with ASan/UBSan and no reported finding or crash. It identifies
its exact parser snapshot (`fa90a35`) separately. It exercises lexer/parser/common
code, and provides no sanitizer claim for the later semantic-index changes,
code generation, runtime or editor. Earlier campaigns remain historical evidence.

The [0.3 benchmark report](benchmarks/windows-0.3-2026-11-04.json) was captured
from this clean frozen revision and the same Windows native binary after the
host tests finished. Every output matched. Three whole-process samples per
configuration gave median loop times of **27.957/28.602 ms** and Fibonacci(20)
times of **17.252/18.722 ms**, at `-O0`/`-O2`. Startup and captured output are
included; these measurements establish no performance or optimization advantage.
See [benchmark methodology](benchmarking.md).

A [local Windows package record](validation/windows-package-0.3-2026-11-04.json)
identifies `pikaflow-0.3.0-windows-x64-5f0b109.zip`, **18,175,451 bytes**, SHA-256
`03cff2c8b384e2c1bafd36d2089a613da6689dd23e83755f0c466e55e7825494`.
The extracted compiler, showcase, namespaces, interpreter/JIT, installed tools
and underscore/context symbol-index cases passed. This is a local artifact,
not a published Release or a universal installer; native linking needs a
compatible host toolchain and SDK.

Reproduce the configured suites with:

```powershell
ctest --test-dir build/interpreter --output-on-failure
ctest --test-dir build/native --output-on-failure
```

Raw logs and archives remain under ignored `out/windows-validation/` and
`out/linux-validation/` paths. The GitHub workflow is manual-only and was not
dispatched; these are local host results.

## Historical 0.2 clean release validation

The frozen implementation at **`39fcac34a3528a4e74ea1cdbbff58dd9e433a19b`**
passed the complete host suites below. Both hosts used the same clean source
archive (`dirty: false`), SHA-256
`a0a23920e63a4ec67b944ff0263ac10632dd7f31ad1e977d011691abd63dd788`.
These results establish the supported [0.2 contracts](FEATURES.md), with their
stated limits; they do not imply unrestricted ABI or editor-protocol support.

| Host / mode | Result | Tracked record |
|---|---|---|
| Windows x64, MSVC 19.52 / LLVM 24.0.0git | **19/19 native suites passed**, 55.30 s | [Windows 0.2 manifest](validation/windows-0.2-2026-11-04.json) |
| Windows x64, reference build | **15/15 suites passed**, 12.74 s | Same Windows manifest |
| Ubuntu 24.04 x86-64 / WSL, GNU 13.3 / LLVM 24.0.0 | **20/20 native suites passed**, 180.57 s | [Linux 0.2 manifest](validation/linux-0.2-2026-11-04.json) |

The Linux suite includes the additional real GDB test: breakpoints, stepping,
backtrace and mutated/captured scalar watches. Windows CodeView object inspection
is separate evidence; graphical IDE integration, managed-value visualization and
optimized-code debugger quality remain unverified.

The LLVM-enabled suites on each host include:

| Check group | Passed checks |
|---|---:|
| Compiler integration | 216 |
| Generic/recursive ADT/guard/Result/showcase language cases | 133 |
| Formatter, LSP and local-package integration | 14 tests; Linux skips one Windows-only junction case |
| Generated C ABI JIT/native execution comparisons | 120 |
| Located C integer range failures | 40 |
| Invalid callback declarations | 3 rejected |
| Threaded task/pointer executions | 50 |
| Invalid task/pointer source programs | 4 rejected |
| Fatal runtime cases | 47 |
| Fatal task cases | 10 |
| Arithmetic differential programs | 30 |
| Contextual typing/callable namespace programs | 6 |

Source execution cases compare the interpreter, JIT and native code at `-O0` and
`-O2` where applicable. Host C fixture calls independently check the expected ABI.
Runtime tests also check actual simultaneous workers, graph isolation/cycles,
structured joins and collection. These group counts overlap suite organization;
they are not a sum of independent CTest executables.

Installation checks passed for the compiler, runtime, standard library and Python
tools, including a relocated Unicode prefix, source-tree fallback resistance,
clean-environment native linking and token-checked installed formatting. This is
a development installation with host toolchain requirements, not a universal
standalone installer.

The Windows native compiler SHA-256 is
`ceecd3867110d6a03e868bd1e5a3f4f764667205a47f24f45d3736914ed9f8d7`;
the reference compiler SHA-256 is
`153f844784a94b416b5d3677dea45c82b4bc3bf98fccec70630f4494f3fdb1a0`.
The Linux native compiler SHA-256 is
`eb8481b564fd150e9f3fd9ba112bcdcca87eee0d0786b03ec97fbe04eef6f848`.
The manifests retain exact compiler/toolchain and source identities. Windows uses
LLVM checkout `0247623ddadb8d078a446a704cc20f0a8c944fc8`; Linux uses real LLVM
24.0.0 nightly `5883ca2582af`. These records do not promise bit-for-bit build
reproducibility or compatibility with every LLVM main revision.

The separate [0.2 benchmark report](benchmarks/windows-0.2-2026-11-04.json) uses
that clean source commit and Windows binary. Both workloads verified outputs
before three timed repetitions at each optimization level. Whole-process median
times were 34.778/31.075 ms for the checked loop and 20.124/19.283 ms for
Fibonacci(20), at `-O0`/`-O2`. Startup and output capture are included; no general
performance or optimization advantage is claimed. See [methodology](benchmarking.md).

The refreshed [frontend fuzz campaign](fuzzing.md) ran **80,430 executions in
49 seconds** under Clang ASan/UBSan with no reported finding or crash artifacts.
Its separately identified source snapshot and bounded parser-only scope are
recorded there; it is not part of the frozen native suite count. Runtime task
ASan/UBSan checks passed, while this host could not start ThreadSanitizer's
runtime; no race-clean TSan result is claimed. [Runtime details](runtime.md)

Reproduce the configured release suites with:

```powershell
ctest --test-dir build/interpreter --output-on-failure
ctest --test-dir build/native --output-on-failure
```

Raw host build logs and ignored source archives remain under `out/windows-validation/`
and `out/linux-validation/`; the tracked manifests above preserve their essential
identities and results. The manual-only GitHub workflow was not dispatched, so
these local results are not remote CI claims.

## Historical 0.1 evidence

The records below are retained as the earlier baseline. Their counts, hashes and
debugger limitations describe 0.1 and are superseded by the later records above;
they must not be reported as the current release's feature or test coverage.

**Status: the recorded Windows/Linux native integration checks passed.** This
record is not a v1 completion declaration. Results below identify tested snapshots
and their limits; the broader approved roadmap remains tracked in
[feature status](FEATURES.md).

### Recorded evidence

| Host / mode | Snapshot | Evidence |
|---|---|---|
| Windows x64, MSVC 19.52, reference interpreter | Latest recorded interpreter checks | `tests/test_cli.py`: **99 checks passed** |
| Windows x64, frontend | Current frontend suite | Lexer/parser/AST, unsafe/extern syntax, bounded recovery, nesting limits, and 1,000 deterministic malformed inputs passed |
| Windows x64, typed IR | Current IR verifier suite | Signature/type/target/assignment checks and side-effect-destination/captured-direct-call rejection regressions passed |
| Windows x64, generated arithmetic | Earlier current-language snapshot | **30 programs passed**, seed `20261104`, against independent Python arithmetic reference |
| WSL Ubuntu 24.04, Clang 18 frontend sanitizers | Bounded libFuzzer snapshot | **64,258 executions / 46 seconds**, no crash artifacts or ASan/UBSan failures; see [commands and limits](fuzzing.md) |
| Windows x64, benchmark report tooling | Current report-unit suite | **3 tests passed**; no native performance measurement implied |
| Linux, reference interpreter | Earlier snapshot | **74 CLI checks passed**; this predates the expanded 99-check suite and must not be described as its Linux validation |
| Windows x64, LLVM native/JIT | Clean `6ca5992` native CTest | **13/13 suites passed** in 19.09 s, including **216 CLI integration checks**, **6 contextual typing/callable namespace programs**, **34 runtime failure cases**, and **30 differential arithmetic programs** |
| WSL Ubuntu 24.04 x86-64, GNU 13.3 Release, LLVM 24 | Clean `6ca5992` native CTest | **13/13 suites passed** in 74.03 s, including **216 CLI integration checks**, **6 contextual programs**, **34 runtime failure cases** and **30 differential arithmetic programs** |
| Windows x64, C ABI | Host C fixture plus generated cross-checks | **52 source JIT/native checks** across `-O0`/`-O2`; **12 direct backend record scenarios** at both levels; host C reference calls passed |
| Linux x86-64, C ABI | Host C fixture plus generated cross-checks | **52 source JIT/native checks** across `-O0`/`-O2` and **12 direct backend record scenarios** at both levels passed after the POD register-passing classification fix |
| Windows/Linux, native environment | Automated execution checks | Unicode output paths, adjacent-file preservation and temporary cleanup on success/failure passed on both; Windows additionally passed without developer-shell variables |
| Windows x64, installed/relocated layout | Clean `6ca5992` install | **3 checks passed**: a relocated Unicode prefix selected the installed stdlib marker; native compilation worked without a developer environment; corrupting only the installed runtime copy caused linking to fail, proving preference over the build-tree fallback; [record](validation/install-windows-2026-11-04.json) |
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

The final Windows run used clean commit
`6ca599277480b22513e8d263680c3ee7e82cbffd`, with binary SHA-256
`602078d66fb4d3bc60b3b7a70db165a2a507ac2be6e0af33294c06ebaedb2015`.
The tracked benchmark and install records identify that same source/binary pair.

The final Linux source archive captured that same clean commit (`dirty: false`),
with 85 files and archive SHA-256
`dda57eafb35c9b0181abd13affb80ba250fab8ebd2b51ac6e281dc8b286829b9`.
The Linux compiler identified itself as
`PikaFlow 0.1.0; LLVM enabled; x86_64-pc-linux-gnu`, with binary SHA-256
`a9619a89e93d737f7bf30e5647c29a1012afab8bd7474496009adb0383f2bed7`.
The core semantic changes are committed in `92e0e38`; the coordinated backend/C
ABI changes are committed in `8a96379`. The final Linux snapshot metadata and
full CTest log are retained locally under
`out/linux-validation/snapshot-native-final.json` and
`out/linux-native-validation/build/Testing/Temporary/LastTest.log`; the Windows
log is `build/native/Testing/Temporary/LastTest.log`. These raw local build logs
are ignored artifacts; the source commit, toolchain identities and archive/binary
hashes above identify the exact clean-checkout validation rather than promising
bit-for-bit reproducible builds. The tracked benchmark report separately records
its exact Windows compiler binary SHA-256.
The [tracked final Linux result manifest](validation/linux-2026-11-04.json) copies
`out/linux-validation/native-final-results.json`;
the frozen source archive is `out/linux-validation/source-native-final.tar`.
The Windows CodeView object inspection is retained locally in
`out/debug-validation/codeview.txt`; readable line records and source locals
were checked, without claiming interactive debugger validation.

### Reproduce checks

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
