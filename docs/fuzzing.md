# Frontend fuzzing

`tests/fuzz_parser.cpp` is a coverage-guided libFuzzer entry point for arbitrary
source bytes. It calls the real lexer and parser, accepts source diagnostics
(`pika::Error`), and leaves unexpected exceptions, crashes and sanitizer failures
visible to the fuzzer. Inputs larger than 16 KiB are skipped. This target does not
execute programs or validate the semantic compiler, interpreter, LLVM backend or
runtime.

The small committed corpus covers hybrid words, nested Result patterns, and
escaped/UTF-8 string bytes. Keep generated corpus mutations and crash artifacts
under the ignored `out/` directory.

## Build and run

Use Clang with libFuzzer, AddressSanitizer and UndefinedBehaviorSanitizer runtimes.
The optional CMake target is disabled by default and rejects unsupported compiler
drivers. It compiles the frontend directly without LLVM backend libraries:

```sh
cmake -S . -B out/fuzz-build -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
  -DPIKAFLOW_ENABLE_LLVM=OFF -DPIKAFLOW_BUILD_FUZZER=ON
cmake --build out/fuzz-build --target pikaflow_parser_fuzzer
mkdir -p out/fuzz-validation/corpus out/fuzz-validation/artifacts
cp tests/fuzz_corpus/* out/fuzz-validation/corpus/
out/fuzz-build/pikaflow_parser_fuzzer out/fuzz-validation/corpus \
  -max_total_time=45 -max_len=4096 -rss_limit_mb=1024 \
  -artifact_prefix=out/fuzz-validation/artifacts/
```

A standalone build with the same sanitizers is also supported:

```sh
clang++ -std=c++17 -O1 -g -fno-omit-frame-pointer \
  -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -Iinclude \
  src/common.cpp src/lexer.cpp src/parser.cpp tests/fuzz_parser.cpp \
  -o out/fuzz-validation/fuzz_parser
```

This is bounded bug discovery, not a proof of correctness or a performance
benchmark. `tests/fuzz_frontend.py` separately compares generated arithmetic
programs with an independent Python result; it is not coverage-guided fuzzing.

## 0.4 campaigns and their input boundaries

The latest parser campaign is recorded at clean candidate
`96ae47b209eac483595fd264cc3cc0ba5d93fdd1`. It used Clang 18.1.3,
libFuzzer/ASan/UBSan, leak detection and the LLVM 24 symbolizer. Seed `20261105`
and the existing command budgets (`45` seconds, `4096` bytes, `1024` MiB RSS,
`5` seconds per input) produced **37,183 executions**, exit **0**, with no reported
sanitizer finding, crash artifact or warning.

The 13 starting inputs totaled 13,264 bytes: eight tracked seeds and five
generated depth stress inputs. Final in-memory statistics were 556 retained
inputs/299 KiB, coverage counter 2,282, feature count 9,645, peak RSS 414 MiB and
an evolving size limit of 3,809 bytes. LibFuzzer reported 46 elapsed seconds;
the runner recorded 42.545 seconds. Both observations remain intact in the
[campaign manifest](validation/parser-0.4.0-2026-11-05.json); neither counter nor
timing is a coverage percentage or compiler performance measurement.

The full source archive SHA-256 was
`114adda2c5ee3352df3a290ca06806eb88de57d4e42d23fa1a726541fe8cc547`;
the fuzzer binary SHA-256 was
`3e5a41368e8f77ced2a56d927fec5e4805960add3410813f566009c2fe643ded`.
The [input-equivalence record](validation/parser-input-equivalence-0.4.0-2026-11-05.json)
checks all 12 recorded source/build inputs against raw members of final archive
`b976d621302cfae302b552da5469ea805961bf79a702cf552a9c0cc1217b586a`
at `1cfd6aa`: every byte matches, without newline normalization. The later
compiler-expression fix changed `compiler.cpp`, which the parser target does
not compile. Original logs/corpus/artifacts stay in `out/fuzz-0.4-validation/`.

Separate [generated-program tests](program-testing.md) use five bounded templates
and independent Python expectations rather than coverage-guided mutation. The
[extended final run](validation/programs-0.4.0-2026-11-05.json) checked 300 programs
with seed `20261106`: 910 interpreter/JIT/native outputs and ten native builds.
Each template supplied 60 cases; native execution sampled the first of each
template at both optimization levels. No mismatch occurred. This evidence is
specific to the template/parameter space, not arbitrary semantic coverage.

A further [clean final ASan/UBSan run](validation/sanitizer-0.4.0-2026-11-05.json)
instrumented the LLVM-disabled semantic compiler, verifier, optimizer,
interpreter and runtime. It passed focused units/source cases and 50 generated
programs with leak detection at the default 8 MiB stack. An earlier candidate
had a real compiler stack-overflow finding, retained in
[the failed manifest](validation/sanitizer-candidate-0.4.0-2026-11-05.json).
The [correction/post-checks](validation/sanitizer-post-checks-0.4.0-2026-11-05.json)
and [validation history](validation.md) distinguish that failure from the final
successful rerun. LLVM backend code was not sanitizer instrumented.

## Historical 0.3.1 depth-bound hardening refresh

The latest campaign used clean source and corpus commit
`d77a3b5cdab180e061d66f0fee18634a913ad001`, source archive SHA-256
`5bb56a5d7ad428c7c11478be98dbcfb7a08660b3b0ea0386a885644104936c80`.
The seven tracked seeds were supplemented with five generated depth stress
inputs: binary chains, call chains, `elif`, repeated Result propagation and
explicit type applications. The [recorded manifest](validation/parser-0.3.1-2026-11-05.json)
retains their content and hashes, commands, toolchain and exploration counters.

Clang 18.1.3 built the same CMake frontend target with libFuzzer/ASan/UBSan,
leak detection and the LLVM 24 symbolizer. The command used seed `20261105`,
`-max_total_time=45 -max_len=4096 -rss_limit_mb=1024 -timeout=5` and
`-print_final_stats=1`. It exited **0**, completing **48,721 executions**,
with **zero reported sanitizer findings, crash artifacts or warnings**.
The 12 initial inputs totaled 13,034 bytes; the final in-memory corpus had
623 inputs/348 KiB, coverage counter 2,239, feature count 10,289 and peak RSS
411 MiB. The evolving size limit reached 3,809 bytes.

LibFuzzer reported 46 elapsed seconds; the runner independently recorded
43.372 seconds. Both raw observations remain in the record rather than being
reconciled into a precise performance measurement. The budget is approximate.
Exploration counters are instrumentation-specific, not coverage percentages.
The executable SHA-256 is
`44b8a3d9fc226677420d42ecc7e142afb52b74b10fa30178ef9bc681ad2eb070`;
the raw log SHA-256 is
`a06b6ab81c42cc24b89d6fdda1dd70e05eaecc6d21aa0b66339e4155566f852e`.

Archives, binary, logs, corpus and artifacts remain under ignored
`out/fuzz-0.3.1-validation/`. This target exercises only the real lexer/parser
and common code, not semantic compilation, the interpreter, LLVM backend,
runtime or editor. Focused runtime/IR sanitizer evidence is separately identified
in [validation.md](validation.md). Earlier campaigns below retain their own
source identities and are not reused as a claim about later code.

## Historical initial validation snapshot

On 2026-11-04, WSL Ubuntu 24.04 used Ubuntu Clang 18.1.3 (1ubuntu1), targeting
`x86_64-pc-linux-gnu`. The preinstalled compiler lacked its sanitizer runtimes;
the matching official Ubuntu `libclang-rt-18-dev` package was installed after an
apt simulation confirmed zero upgrades and zero removals. Its three 32-bit
dependencies were also new installations.
The fuzzed frontend and harness content is captured by commit `7c380ad`.

The standalone command above built successfully. This exact run started with
copies of the three committed seeds:

```sh
out/fuzz-validation/fuzz_parser out/fuzz-validation/corpus \
  -max_total_time=45 -max_len=4096 -rss_limit_mb=1024 \
  -artifact_prefix=out/fuzz-validation/artifacts/
```

It exited **0** after **64,258 executions in 46 seconds**, with no crash artifacts
and no ASan/UBSan failure. The automatic seed was `2128163483`; libFuzzer reported
coverage counter count `1847`, feature count `7523`, a retained corpus of 575
inputs/51 KiB, and RSS 423 MiB. These are instrumentation-specific exploration
counters, not source coverage percentages or compiler performance measurements.
The evolving input-length limit remained 219 bytes during this short campaign,
so specifying a 4096-byte maximum does not establish that all larger sizes were
explored. The harness itself admits inputs up to 16 KiB.

The installed WSL environment had no external `llvm-symbolizer`; libFuzzer
reported a symbolizer warning. Sanitizers remained enabled, but an eventual crash
would need a symbolizer or offline address translation for readable stack traces.
The raw run log, mutated corpus and artifacts directory are retained locally in
ignored `out/fuzz-validation/`. This snapshot validates only a bounded frontend
campaign, not the rest of the compiler.

## Parser refresh after the roadmap features

The second campaign used an exact `git archive` snapshot of commit
`3e7cafc95c7365c3d1c87bbe585902420670df8f`, containing the harness, headers,
common code, lexer, parser and committed corpus. It included the new
`generics_enums` and `tasks_ffi` seeds: recursive generic enum syntax, function
specialization, guarded patterns, Result propagation, typed tasks, pointer
offsets and explicit C integer annotations. This still exercises parsing only;
it does not type-check or execute those features.

Ubuntu Clang 18.1.3 (1ubuntu1), target `x86_64-pc-linux-gnu`, built with the same
libFuzzer/ASan/UBSan flags above. The compiler executable SHA-256 was
`8ef402d453d1ba4902e4ee0f0f847f6cfa01400c95aa43c24e97818b9c0e3f45`;
the source archive SHA-256 was
`10753117d760d6c2c160e6de5f2fe689e7fd3ae22a721c2fdfdd67566ba2a1b0`.

```sh
ASAN_OPTIONS=detect_leaks=1 ./fuzz_parser corpus -seed=20261104 \
  -max_total_time=45 -max_len=4096 -rss_limit_mb=1024 -timeout=5 \
  -print_final_stats=1 -artifact_prefix=artifacts/
```

The process exited **0** after **80,430 executions in 49 seconds**, with no
crash artifacts or ASan/UBSan finding. It started with five seeds totaling 1,331
bytes and finished with 710 retained inputs/88 KiB, counter count 2,142, feature
count 8,589 and peak RSS 423 MiB. The evolving size limit was 399 bytes; the
4096-byte configured maximum does not imply that larger inputs were explored.
The requested 45-second budget is approximate; the final reported elapsed time
was 49 seconds. External symbolizer warnings remained and limit readable crash
stacks, as in the earlier run.

The exact archive, toolchain version, input/output hashes, executable, full log,
mutated corpus and empty artifacts directory are retained under ignored
`out/fuzz-roadmap-validation/`. Its `results.json` records the machine-readable
summary. These runs provide bounded parser bug discovery, not semantic,
concurrency or backend verification.

## 0.3 qualified-name and compound-update refresh

The next campaign used an exact full `git archive` of source commit
`3c46faf1744c39d07c1573b6792e40779001a63b`. Its source archive SHA-256 is
`6e8f73a144da176051e6e0ab29b6ffdabb8f4609163cda319f7f1b14f1d0c606`.
The six starting inputs came from a separate corpus archive of commit
`0edac06e78b10764c34e3309b80ea7c290975589`, SHA-256
`d1506e490f23f5029729215498167912fce819916414d2c6a6765528ac0100aa`.
This separation keeps the tested parser source exactly at `3c46faf` while
including the new `qualified` seed. That seed covers imported aliases, dotted
generic types/calls/patterns/words, pipelines, and indexed compound updates.

Ubuntu Clang 18.1.3 built the optional CMake target with LLVM disabled:

```sh
cmake -S source -B build -G Ninja \
  -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DPIKAFLOW_ENABLE_LLVM=OFF -DPIKAFLOW_BUILD_FUZZER=ON -DBUILD_TESTING=OFF
cmake --build build --target pikaflow_parser_fuzzer --parallel 1
ASAN_OPTIONS=detect_leaks=1 \
ASAN_SYMBOLIZER_PATH=/usr/lib/llvm-24/bin/llvm-symbolizer \
  build/pikaflow_parser_fuzzer corpus -seed=20261104 \
  -max_total_time=45 -max_len=4096 -rss_limit_mb=1024 -timeout=5 \
  -print_final_stats=1 -artifact_prefix=artifacts/
```

The target's compile commands include `-O1 -g -fno-omit-frame-pointer`,
`-fsanitize=fuzzer,address,undefined`, and `-fno-sanitize-recover=all`. The compiler
executable SHA-256 remains
`8ef402d453d1ba4902e4ee0f0f847f6cfa01400c95aa43c24e97818b9c0e3f45`;
the new fuzzer binary SHA-256 is
`12b61c50531353fc127e2dbb433ae776f13eada188b6cd8379f9ee898ecb6f29`.

| Observation | Result |
| --- | --- |
| Exit status | 0 |
| Executions | 78,842 |
| Reported elapsed / measured wall time | 46 seconds / 46.379 seconds |
| Starting corpus | 6 inputs, 1,731 bytes |
| Final in-memory corpus | 649 inputs, 101 KiB |
| Coverage counter / feature count | 2,185 / 8,706 |
| Evolving input-length limit | 400 bytes |
| Peak RSS | 423 MiB |
| Crash artifacts / sanitizer findings | 0 / 0 |

The existing LLVM 24 symbolizer resolved source frames in this run; no
symbolizer warnings were reported. No additional toolchain packages were
installed for this refresh. The configured 4096-byte maximum does not establish
that larger inputs were explored: the evolving length limit stayed at 400.
Exploration counters are instrumentation-specific, rather than source coverage
percentages or performance results.

The full log SHA-256 is
`d36f347b596d10f1682e1b547fb9efe3c4c49ce995d9ab23210a884fa9d07a71`.
The archives, initial input hashes, compiler and symbolizer hashes, CMake cache,
compile commands, executable, run log, corpus file manifest, mutated corpus and
empty artifacts directory are retained under ignored
`out/fuzz-0.3-validation/`; `results.json` records the machine-readable summary.
Only the real lexer/parser ran. Qualified-name binding, assignment evaluation
order, semantic indexing, the typed optimizer, LLVM execution and editor behavior
were not exercised by this fuzz target.

## Historical 0.3 grammar: underscore constructor patterns

The parser subsequently distinguished qualified underscore names and constructor
calls from a bare match wildcard. A fresh campaign used exact source commit
`fa90a352b2fffed9a7fe41dedc8150f59a4ff27d`, source archive SHA-256
`03b774542a4fd01ce562322c43880c7be11b077aae992b3227c6149447e13a26`.
Its seven seeds came from commit
`391510b2f2c8c67fe38ec63b91bdb2b40e9b0b6d`, corpus archive SHA-256
`21a41df5833a1576beaf78b5fa781a3239f7ca551744e4c96bb84bbbddf35c22`.
The new `underscore_patterns` seed includes `_.Ctor(...)`, `_()`, a bare
`case _:`, and an underscore in a `let` destructuring target.

The same Clang 18 CMake target, LLVM-off configuration, ASan/UBSan/leak settings,
LLVM 24 symbolizer, seed `20261104`, 45-second budget, 4096-byte maximum,
1024-MiB RSS limit and 5-second input timeout were used. The new executable
SHA-256 is
`7a301e4409416fdba2e81f33628773381461b50ea2f407d8919bf4a500e6d853`.

The process exited **0** after **79,589 executions in 46 seconds** (46.638 seconds
wall time), with no sanitizer finding, crash artifact or symbolizer warning.
The initial corpus contained seven inputs/2,008 bytes. Final in-memory stats
were 646 retained inputs/100 KiB, counter count 2,205, feature count 8,576, and
peak RSS 421 MiB. The evolving input-length limit remained 400 bytes.

The full log SHA-256 is
`cf9f38b34f964e31a2b9d21737ba88934d62982b54f5296067ca7222a494af68`.
The exact archives, executable, configuration, per-file hashes, raw log, corpus
manifest and empty artifacts directory are retained under ignored
`out/fuzz-0.3-final-validation/`, with the summary in `results.json`.
Earlier campaign records above remain historical results for their respective
snapshots. This refresh exercises lexer/parser behavior only; it does not
validate semantic binding roles, rename safety, or editor execution.
