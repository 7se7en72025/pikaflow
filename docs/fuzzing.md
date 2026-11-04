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
