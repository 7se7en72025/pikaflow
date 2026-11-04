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

## Validation snapshot

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
