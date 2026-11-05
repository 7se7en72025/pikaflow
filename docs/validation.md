# Validation record

## 0.6.0 clean release validation

The frozen implementation is **`e70e7aa79006637a4f1c6e464b7b1d4c12b8acf1`**,
with clean source (`dirty: false`) and common source archive SHA-256
`29a816f2fe64586c2e009d1fae88b5efab8092f6f780928008d17f8a4b725539`.
Linux and sanitizer post-checks matched all **188 archived files**; Windows also
recorded unchanged source after testing. The six JSON records preserve the
captured evidence; later documentation commits do not change this tested code.

| Host / mode | Complete result | Record |
|---|---|---|
| Windows x64, MSVC 19.52 / LLVM 24.0.0git | **31/31 native suites**, 245.04 s | [Windows](validation/windows-0.6.0-2026-11-05.json) |
| Windows x64, MSVC 19.52, LLVM disabled | **27/27 reference suites**, 78.05 s | [Same record, reference mode](validation/windows-0.6.0-2026-11-05.json) |
| Ubuntu 24.04 x86-64 / WSL2, GNU 13.3 / LLVM 24.0.0 | **33/33 native suites**, 428.08 s | [Linux](validation/linux-0.6.0-2026-11-05.json) |

Times above are CTest's elapsed values. Linux separately recorded a 428.138 s
test wrapper and 227.058 s build. No CTest suite failed or skipped. The Linux
tools unittest internally skips its Windows-only junction case; Windows's
seven-test report suite internally skips Linux-only `/dev/full`.
Native suites include **160 optimizer execution comparisons**, schema-2 report
checks and the existing language/runtime/ABI/editor/package cases. Linux's shared
pass passed **seven FileCheck pipelines, 91 execution checks and seven remark
configurations**, including default silence, passed/missed filtering, source-
located YAML and unchanged IR/runtime behavior with remarks enabled or disabled.
Windows uses the shared source internally, without a dynamic plugin claim.

### Optimizer and instrumented semantic checks

The fresh-cell pass tracks only local `i64`/`bool` allocations within one basic
block. Move aliases, register-write versions and known stores maintain facts;
unknown effects, calls/closures and CFG boundaries discard them. Its 4,096-cell
cap loses only optimization opportunities. Allocations, stores, debug metadata,
managed roots and checked failures remain. There is no general heap alias,
cross-CFG cell, interprocedural or floating-point propagation.
[Legality and budgets](optimization.md)

The [clean sanitizer/campaign record](validation/semantic-fuzzer-0.6.0-2026-11-05.json)
includes **four fresh focused ASan/UBSan suites**, passing in 6.26 s CTest time
(6.334 s wrapper): IR optimizer units, 32 reference source comparisons, seven
schema-2 report tests, and five seed replays with 12 golden self-check outcomes.
Canonical archive bytes changed line endings from the dirty initial snapshot,
so the actual **242.370 s rebuild compiled 19 translation units in 24 steps**;
it was not a no-op reuse. Both candidate and clean provenance remain explicit.
All nine fuzzer target units (harness plus eight implementation files) carry
coverage/ASan/UBSan; the separate nine core/runtime/CLI unit commands also prove
sanitizer instrumentation. No uninstrumented core library was linked into the
fuzzer, and this target does not instrument LLVM backend code.

The final campaign completed **7,294 executions**, seed `20261107`, without
findings, artifacts, warnings or sanitizer reports. Five bounded valid-source
families cover arithmetic/typed words, CFGs, aliased arrays, generic records/aliases
and captured mutable cells with closure calls and GC. Original, optimized and
twice-optimized IR must match an independent full-output oracle and restore
managed heap baselines. It used the default **8 MiB stack**, leak detection,
eight-byte inputs, a requested 120-second budget, five-second input timeout and
1,024-MiB RSS limit. LibFuzzer reported **121 s**, while the wrapper recorded
**114.081 s**; both observations remain unnormalized. Counters were 6,695 coverage
and 12,528 features, peak RSS 502 MiB, with 94 retained inputs/517 bytes (97 corpus
files on disk). These are bounded observations, not coverage percentages or a
correctness proof. [Harness contract](fuzzing.md)

### Identities, quiet benchmark and local artifacts

| Compiler | SHA-256 |
|---|---|
| Windows native | `cc17a7073dc0a6a423e505926b9b8154b675f47b4e2161206bc4ee1d127ff20d` |
| Windows reference | `a752526cc0ae2e5400f631b110a538fcdd74ed1b9ebbcead630f2c45d3df930c` |
| Linux native | `e604b788c733d0ef355c9437fd35a61491641409db84846e2826f8155626547e` |

The [quiet Windows benchmark](benchmarks/windows-0.6.0-2026-11-05.json) used that
native binary and clean revision after all host/package/sanitizer work finished.
Every interpreter, warmup and timed output matched, with three process-inclusive
samples per configuration. O0/O2 medians were **27.516/24.878 ms** for the loop
and **16.542/16.444 ms** for Fibonacci(20). These samples establish no general
speedup or multiplier. [Measurement scope](benchmarking.md)

| Local package | Bytes | SHA-256 / record |
|---|---:|---|
| `pikaflow-0.6.0-windows-x64-e70e7aa.zip` | 18,242,211 | `dd1a1d27d3eb41eb10ec0e44c4056928ffce7162bf216aca607e78ababef72ca`; [Windows](validation/windows-package-0.6.0-2026-11-05.json) |
| `pikaflow-0.6.0-linux-x86_64-e70e7aa.tar.gz` | 25,311,845 | `90bc3043e219c0e50aa00a8b1852744ee34a00e136ca3999497a544c43372b37`; [Linux](validation/linux-package-0.6.0-2026-11-05.json) |

Both extracted packages passed the optimization lab/schema-2 report proof:
**52 to 48 instructions, two CSE reuses, four dead results removed and eight
cell loads forwarded**. The lab still prints `84`, `48`, `0`. Showcase, record,
tool and project checks also passed. Linux ran three project cases in each of
interpreter/JIT/native modes and verified the installed plugin's passed/missed
YAML at source positions **12:3** and **22:3**, quiet default, identical IR, a
successful `lli` output and a retained located fatal failure. Installation's
RUNPATH adjustment is recorded separately from the build plugin identity.
These are local tested archives with host-library/SDK requirements, not published
universal installers. No remote CI run is claimed. Older releases and the failed
0.5 test-race candidate remain below; their results are not relabeled as 0.6.

## Historical 0.5.0 corrected clean release validation

The final frozen implementation is **`8ec3986fe9bf606e383e1c0c06bb8c3436bf4d49`**,
with clean source (`dirty: false`) and common archive SHA-256
`3ba8f6ffc0cebf8ad899f3e5556da0f18eb905734a3bb23681182eb7912d8104`.
Linux and semantic-fuzzer checks verified all 179 archived files unchanged;
Windows also recorded matching source after testing. The JSON below preserves
all captured fields, with repository line-ending normalization.

| Host / mode | Complete result | Record |
|---|---|---|
| Windows x64, MSVC 19.52 / LLVM 24.0.0git | **31/31 native suites**, 201.89 s | [Windows](validation/windows-0.5.0-2026-11-05.json) |
| Windows x64, MSVC 19.52, LLVM disabled | **27/27 reference suites**, 54.58 s | [Same record, reference mode](validation/windows-0.5.0-2026-11-05.json) |
| Ubuntu 24.04 x86-64 / WSL2, GNU 13.3 / LLVM 24.0.0 | **33/33 native suites**, 342.55 s | [Linux](validation/linux-0.5.0-2026-11-05.json) |

Times are CTest's reported elapsed values; the Linux wrapper separately recorded
342.607 s and a 0.216 s no-op incremental build. No CTest suite was skipped.
The Linux tools suite internally skips its Windows-only junction unittest;
Windows's report suite skips the Linux-only `/dev/full` unittest. The report
suite has seven tests and covers JSON/count accuracy, nonexecution, flags,
protected inputs/hard links/metadata, and write/flush failures. Linux's plugin
passed **seven FileCheck pipelines and 85 executions**, including v1 effects,
v2 successful constants, retained failing calls and real source/IR roundtrips.
Windows uses the shared pass internally; a dynamic Windows plugin remains outside
the recorded host configuration.

The existing language/runtime/editor/package suites also passed, including
135 optimizer comparisons and the default ten-program independent oracle
(40 outputs and ten native builds per native host). No new parser campaign or
extended 300-program campaign was run for 0.5; those 0.4 records remain below.

### Failed Linux candidate and test-only correction

The [earlier clean `083b232` candidate](validation/linux-candidate-0.5.0-2026-11-05.json)
passed 32/33 Linux suites. Its cleanup test raced between checking
`/proc/PID/stat` existence and reading it after the child was reaped, raising
`ProcessLookupError`. The correction reads directly while handling both
`FileNotFoundError` and `ProcessLookupError`. It changes the Python test only,
not C++ compiler/runtime behavior. The corrected freeze was rerun on both hosts;
the failed candidate is not counted as the final successful result.

### Instrumented semantic/optimizer exploration

The [clean final campaign](validation/semantic-fuzzer-0.5.0-2026-11-05.json)
completed **11,228 libFuzzer executions**, seed `20261106`, with no findings,
artifacts, sanitizer reports or warnings. All nine implementation units were
compiled with coverage/ASan/UBSan, without linking an uninstrumented core.
Four bounded source families cover scalar/stack arithmetic, branches/loops,
aliased arrays and generic records/aliases. Original, optimized and twice-
optimized IR must match an independent full-output oracle and release their
managed heap. LLVM backend code is not instrumented by this target.

The run used the default 8 MiB stack, leak detection, eight-byte maximum inputs,
a 120-second requested budget, five-second input timeout and 1,024-MiB RSS limit.
LibFuzzer reported **121 seconds**; the wrapper observed **112.623 seconds**.
Both observations remain in the record rather than becoming one precise timing
claim. Instrumentation reported counter 6,141, feature count 9,856 and peak RSS
496 MiB; these are not source coverage percentages. Final corpus size was
72 inputs/334 bytes. Identical instrumented inputs allowed a verified no-op
build; the archived source post-check remained exact.
[Harness and limits](fuzzing.md)

### Identities, benchmark and local artifacts

| Compiler | SHA-256 |
|---|---|
| Windows native | `168cf46b6278694f49209fe83d7089aff34ce0cf3e189a1e73f1168a0d875b20` |
| Windows reference | `b049a738f0c69cfc3f6ee280d17106e7e9956a50b9bcc81362f537d34c5cebfe` |
| Linux native | `c47cb6a04da5e52a314a21c3a184e4763c3adcc7b2dc49e405434e249dc37ae7` |

The [quiet Windows benchmark](benchmarks/windows-0.5.0-2026-11-05.json) used
that native binary and clean revision after host/package work finished. Every
output matched in three process-inclusive samples per configuration. O0/O2
medians were **28.050/29.861 ms** for the loop and **18.180/18.361 ms** for
Fibonacci(20). These measurements establish no general speedup.

| Local package | Bytes | SHA-256 / record |
|---|---:|---|
| `pikaflow-0.5.0-windows-x64-8ec3986.zip` | 18,240,019 | `30380167d908aee20b58773fb47433c09c9c7e91ade745e93b44fc83afe01e40`; [Windows](validation/windows-package-0.5.0-2026-11-05.json) |
| `pikaflow-0.5.0-linux-x86_64-8ec3986.tar.gz` | 25,268,883 | `d2140ae9c189f55928e364c82372cd0324cc9e53f8eef26571b73897bd20bed1`; [Linux](validation/linux-package-0.5.0-2026-11-05.json) |

Extracted packages passed the optimization lab and report proof (50 to 48
instructions, one common expression reused, two dead results removed), alongside
showcase, records, tools and project cases. Linux additionally ran the installed
v2 plugin through `opt`/`lli`; installation's RUNPATH change accounts for its
different installed plugin hash. These local archives retain host-library/SDK
requirements and are not published universal installers. No remote CI run is
claimed. Raw records/logs remain under the corresponding ignored
`out/*0.5-corrected/` paths.

## Historical 0.4.0 clean release validation

The final frozen implementation is **`1cfd6aab929c0c608061159900270981c2b3d12c`**.
Windows, Linux and the final sanitizer run captured clean source (`dirty: false`)
with archive SHA-256
`b976d621302cfae302b552da5469ea805961bf79a702cf552a9c0cc1217b586a`.
Linux and sanitizer post-checks verified all 158 archived files unchanged;
Windows also recorded no source changes. Later commits update documentation.
The tracked JSON preserves captured fields/values with repository line endings;
original raw files and logs remain in the indicated ignored `out/` directories.

| Host / mode | Complete result | Record |
|---|---|---|
| Windows x64, MSVC 19.52 / LLVM 24.0.0git | **30/30 native suites**, 189.98 s | [Windows](validation/windows-0.4.0-2026-11-05.json) |
| Windows x64, MSVC 19.52, LLVM disabled | **26/26 reference suites**, 50.61 s | [Same Windows record, reference mode](validation/windows-0.4.0-2026-11-05.json) |
| Ubuntu 24.04 x86-64 / WSL2, GNU 13.3 / LLVM 24.0.0 | **32/32 native suites**, 300.60 s | [Linux](validation/linux-0.4.0-2026-11-05.json) |

These are CTest-reported times. Windows wrapper measurements were 190.000 /
50.641 s; Linux's build took 153.545 s. No CTest suite was skipped. One internal
Linux developer-tools unittest is Windows-junction-only and skips there. Linux
adds real GDB and standalone LLVM-plugin suites. The current Windows LLVM build
does not support that standalone plugin; it runs the same pass source internally.
Windows used Python 3.14.2 for CTest; Linux used Python 3.12.3. Toolchain/LLVM
identities remain explicit in the manifests rather than implying every revision
is compatible.

| Check group | Windows native / Linux native |
|---|---:|
| Compiler integration | 216 / 217 |
| Language extensions / namespace and assignment | 139 / 61 on each host |
| Records | 90 / 90 |
| Type aliases / extra C wire alias executions and traps | 116 / 8 on each host |
| Optimizer output comparisons | 135 / 135 |
| Project-runner tests | 14 / 14 |
| Editor workflows / semantic decorations | 23 / 8 on each host |
| Git packages / developer-tools tests | 11 / 16 on each host (one internal Linux skip) |
| Generated C ABI comparisons / located range failures | 120 / 40 on each host |
| Task/pointer executions / rejected programs | 50 / 4 on each host |
| Fatal runtime / task cases | 47 / 10 on each host |
| Default independent program campaign | 10 programs, 40 outputs, 10 native builds per host |
| Linux `opt` plugin | 4 FileCheck pipelines, 23 execution checks |

These groups overlap CTest organization. Runtime, frontend, verifier, optimizer,
interpreter, semantic-symbol, contextual/arithmetic, installed-layout and native
environment checks also passed. Reference-mode counts include 99 compiler,
55 language, 56 record, 76 alias and 27 optimizer executions; C wire alias
execution requires LLVM. Native comparisons cover `-O0`/`-O2` and independent
host-C fixtures where applicable. Project tests cover process/report/source
protection, including Windows descendants surviving an exited parent. The
[feature contracts](FEATURES.md) define the supported subset.

### Sanitizer finding, correction and final rerun

Candidate `96ae47b` passed Release checks but its LLVM-disabled Clang ASan/UBSan
language run **failed**: a valid eight-function generic chain with 60 additions
per function exhausted the default 8 MiB stack. The
[failed candidate record](validation/sanitizer-candidate-0.4.0-2026-11-05.json)
retains that stack-overflow finding. Larger-stack probes helped diagnosis; they
are not the successful final validation.

Extracting call, aggregate and advanced-expression dispatch into helpers reduced
the measured Clang-ASan `expr` frame from **17,568 to 7,392 bytes**, preserving
the existing 512 traversal budget. The final clean archive was rebuilt and tested
at the ordinary **8 MiB** limit with ASan/UBSan and leak detection. Four unit
suites, 56 record cases, 76 alias cases, 55 language cases and 50 independent
generated interpreter programs passed with no reported sanitizer finding.
[Final sanitizer manifest](validation/sanitizer-0.4.0-2026-11-05.json)

The [post-check record](validation/sanitizer-post-checks-0.4.0-2026-11-05.json)
also confirms the valid-chain result, a controlled 512-limit diagnostic for a
larger chain, and eight byte-equal O0/O2 IR comparisons with the earlier compiler.
Its measured frame sizes are toolchain-specific, not universal stack guarantees.
It distinguishes LF Git-blob hashes from CRLF archive-export hashes; the final
build used the actual archive bytes. LLVM backend code was not sanitizer
instrumented. The earlier unavailable TSan runtime remains a limitation.

### Independent program and parser campaigns

The [extended program record](validation/programs-0.4.0-2026-11-05.json) ran
**300 programs**, seed `20261106`, against independently calculated Python
expectations: 300 interpreter, 300 O0 JIT, 300 O2 JIT, five O0 native and five O2
native outputs, totaling **910 comparisons** and **ten native builds** in
34.859 s. Each of five templates contributed 60 programs. The compiler hash and
clean revision match the Windows final build; source post-checks passed. Native
execution is sampled by template rather than run for all 300 programs.
[Generator and replay contract](program-testing.md)

The separate parser libFuzzer campaign completed **37,183 executions** under
ASan/UBSan with no reported finding. It was captured at `96ae47b`, with 13 seeds
and a 45-second requested budget. All 12 recorded source/build inputs match raw
members of the final archive byte for byte, proven separately without newline
normalization. `compiler.cpp` is outside that parser-only target; this equality
does not erase its earlier semantic sanitizer finding.
[Parser record](validation/parser-0.4.0-2026-11-05.json),
[input equivalence](validation/parser-input-equivalence-0.4.0-2026-11-05.json),
[fuzzing scope](fuzzing.md)

### Binary, benchmark and local package identities

| Artifact | SHA-256 |
|---|---|
| Windows native compiler | `a249b48ae614d847fe6253aa071558053aa62647d043701912349f65753b7df4` |
| Windows reference compiler | `bea2c6ff05531444d924536d9564b737425f51d65378eba5bf4b127e6b7ab232` |
| Linux native compiler | `2c13068996de3ab15cf115e8daaa7091f801971f16816864ffbb2e9466799263` |
| Final sanitizer compiler | `503407682a90a476a660d49c6116086fcef28a57b4be8f824329bfed2fd760b2` |

The [quiet Windows benchmark](benchmarks/windows-0.4.0-2026-11-05.json) used the
same clean revision and native binary after host/package processes finished.
All outputs matched across three process-inclusive samples per configuration.
O0/O2 medians were **26.218/24.860 ms** for the checked loop and
**15.972/16.566 ms** for Fibonacci(20). These small workloads and startup-inclusive
timings establish no general optimization or cross-language advantage.
[Methodology](benchmarking.md)

| Local archive | Bytes | SHA-256 / verification |
|---|---:|---|
| `pikaflow-0.4.0-windows-x64-1cfd6aa.zip` | 18,237,440 | `cdb36b6de3003b1a2f6dc2f51212c63e158e82fdbff54da3b135900b732c4e18`; [Windows package record](validation/windows-package-0.4.0-2026-11-05.json) |
| `pikaflow-0.4.0-linux-x86_64-1cfd6aa.tar.gz` | 25,226,377 | `18fa9c473a7d50e503a9abd2d5e4295a6d1aff7e4dabf0204dac698ae8c9e7b9`; [Linux package record](validation/linux-package-0.4.0-2026-11-05.json) |

Extracted packages passed showcase, installed tools and record/project checks.
Linux exercised three project cases under each backend, native/JIT demos and the
installed plugin through `opt`/`lli`. Its installed plugin hash differs from the
build copy because installation changes RUNPATH; both identities and that change
are recorded. These are local tested artifacts, not published Releases or universal
installers. Host libraries/SDKs remain necessary, and the plugin needs matching
shared LLVM 24 libraries. The manual GitHub workflow was not dispatched.

## Historical 0.3.1 corrective release validation

The frozen implementation **`d77a3b5cdab180e061d66f0fee18634a913ad001`**
passed the complete host suites below. Both captures were clean (`dirty: false`)
and used source archive SHA-256
`5bb56a5d7ad428c7c11478be98dbcfb7a08660b3b0ea0386a885644104936c80`.
The tracked manifests preserve every captured JSON field; Git normalizes line
endings, and the original raw files remain under `out/`. Linux's
123 archived files still matched after its tests; Windows also recorded no
source changes. Documentation updates follow that tested implementation.

| Host / mode | Result | Exact record |
|---|---|---|
| Windows x64, MSVC 19.52 / LLVM 24.0.0git | **25/25 native suites**, 156.67 s | [Windows manifest](validation/windows-0.3.1-2026-11-05.json) |
| Windows x64, MSVC 19.52, LLVM disabled | **21/21 reference suites**, 38.22 s | [Same Windows manifest, reference mode](validation/windows-0.3.1-2026-11-05.json) |
| Ubuntu 24.04 x86-64 / WSL2, GNU 13.3 / LLVM 24.0.0 | **26/26 native suites**, 214.63 s | [Linux manifest](validation/linux-0.3.1-2026-11-05.json) |

Times above are CTest's reported elapsed times; the Windows manifest separately
records subprocess wall times of 156.719 / 38.265 seconds. No CTest suite was
skipped. The Linux tools suite internally skips one Windows-only junction test.
Windows used Python 3.14.2; Linux used Python 3.12.3. LLVM revisions remain
`0247623ddadb8d078a446a704cc20f0a8c944fc8` on Windows and nightly
`5883ca2582af` on Linux.

| Check group | Windows native / Linux native |
|---|---:|
| Compiler integration | 216 / 217 |
| Language extensions | 139 / 139 |
| Namespace and assignment | 61 / 61 |
| Typed-IR optimizer execution comparisons | 115 / 115 |
| Developer-tools tests | 16 / 16 (one internal Linux skip) |
| Git packages / editor workflows | 10 / 19 on each host |
| Generated C ABI execution comparisons | 120 / 120 |
| Located C integer range failures / invalid callbacks | 40 / 3 on each host |
| Task/pointer executions / rejected source programs | 50 / 4 on each host |
| Fatal runtime / task cases | 47 / 10 on each host |
| Arithmetic differential / contextual typing programs | 30 / 6 on each host |

These groups overlap CTest organization. Shared runtime, frontend, typed-IR,
optimizer, interpreter and semantic-symbol unit suites also passed. The Windows
reference suite includes 99 compiler, 55 language, 21 namespace, 23 optimizer
and 10 task execution checks. Native suites compare interpreter/JIT/native
results at `-O0`/`-O2` where applicable and use independent host C fixtures.
Linux includes real GDB checks. Both hosts passed installed-layout and native
environment checks; this does not establish every debugger or ABI.

### Corrective regressions

- Formatting preserves compound assignment as a single operator, retaining the
  compiler-token equivalence check before replacement. Nonstring document URIs
  return protocol errors instead of Python attribute/type failures, and message
  payloads require UTF-8 rather than accepting UTF-16/32 JSON encodings.
- Text output checks buffered flush/close failures before reporting success.
  The Linux `/dev/full` regression accounts for its extra compiler check.
- Long binary/postfix/`elif` syntax and combined compilation traversal now have
  explicit bounds. Excessive inputs diagnose rather than overflowing native
  stacks. The [language reference](language.md) gives the budgets.
- Managed printing uses a heap worklist, with exact-output coverage for a
  50,000-deep graph containing both a cycle and repeated shared children.
- IR validation bounds nominal shapes before recursive layout equality. Its
  heterogeneous-index proof treats incoming parameter registers as nonconstant;
  a later literal assignment cannot retroactively type an earlier dynamic read.
- MSVC linking uses the exact requested executable path, disables incremental
  `.ilk` generation, isolates export sidecars and checks the debug PDB output
  against protected inputs. Adjacent-file preservation has subprocess coverage.

Focused runtime and IR Clang 18 ASan/UBSan runs passed with leak detection and
no finding. Their [original focused manifest](validation/focused-sanitizers-0.3.1-2026-11-05.json)
records **`sourceDirty: true`**, captured HEAD `6a72340`, and four source hashes.
All four match the corresponding committed blobs at `d77a3b5`; this linkage
does not turn that focused run into a clean-checkout or whole-program sanitizer
claim. The runtime stdout was preserved from the tool transcript; the manifest
distinguishes it from the saved IR process logs. The earlier TSan host startup
failure remains a limitation, not a race-clean result.

### Identities, fuzzing, benchmark and local artifact

| Compiler | SHA-256 |
|---|---|
| Windows native | `046b0c02eeb05b5f6cb798676e5df4f5c0c3115603c81e14d7f1c64b37cd5dde` |
| Windows reference | `4c97ded68ae121c69b7627c607ff0fbaa5061461246a3096c170aa3e3ef807ff` |
| Linux native | `7c70a5798f530b73784763aea844b884759ccf4c10649369fd07e0e7cd2ab842` |

The [parser manifest](validation/parser-0.3.1-2026-11-05.json) records 48,721
libFuzzer executions, a 45-second requested budget, 12 starting seeds and zero
reported findings/artifacts. Only lexer/parser/common code was exercised by that
target. See [fuzzing.md](fuzzing.md) for instrumentation and timing limits.

The clean [Windows benchmark](benchmarks/windows-0.3.1-2026-11-05.json) uses the
same native compiler after the host suites finished. All outputs matched across
three process-inclusive samples per configuration. Loop medians were
**30.401/28.232 ms** and Fibonacci(20) medians **17.231/17.417 ms**, at
`-O0`/`-O2`. These small noisy workloads establish no general speedup or
performance advantage. [Methodology](benchmarking.md)

The [local package record](validation/windows-package-0.3.1-2026-11-05.json)
identifies `pikaflow-0.3.1-windows-x64-d77a3b5.zip`, **18,181,114 bytes**,
SHA-256 `a96a182ad4b07ae9638e586c12abb2adb85d3e5a2c0fc03346f570870008af9b`.
Extracted showcase/namespaces, interpreter/JIT, installed tools and compound
formatting checks passed. This is a local artifact, not a published Release or
universal installer; native linking requires a compatible host toolchain/SDK.

Raw host logs, archives and sanitizer artifacts remain under ignored `out/`
paths identified in these records. GitHub's manual workflow was not dispatched.
These bounded checks provide evidence for their snapshots and inputs; they do
not establish perfection or unrestricted safety for foreign pointers.

## Historical 0.3 clean release validation

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
