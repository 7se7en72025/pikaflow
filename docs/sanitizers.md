# Sanitized compiler and backend verification

`PIKAFLOW_ENABLE_SANITIZERS=ON` is a Linux/Clang developer configuration for
AddressSanitizer and UndefinedBehaviorSanitizer. It instruments PikaFlow's own
C/C++ sources, including the LLVM backend, shared checked-arithmetic pass,
runtime, frontend, interpreter and host test fixtures. The option is off by
default; ordinary release builds retain their existing behavior.

```sh
cmake -S . -B out/sanitized -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DPIKAFLOW_ENABLE_SANITIZERS=ON \
  -DLLVM_DIR=/usr/lib/llvm-24/lib/cmake/llvm
cmake --build out/sanitized --parallel 1
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir out/sanitized --output-on-failure --parallel 1
```

Use matching Clang sanitizer runtime libraries and the documented LLVM 24 SDK.
`compile_commands.json` records the actual per-source instrumentation options;
the CMake cache records the selected configuration. Preserve those files,
compiler versions, source archive/hash, logs and test results when reporting a
sanitizer run. A passing ordinary release run does not substitute for this build.

## Native builds and installation

The configured private backend header contains `PF_SANITIZERS_ENABLED=1` in this
configuration. When the sanitized compiler executes `build`, its POSIX native
link driver adds `-fsanitize=address,undefined`. This brings in the runtime support
needed by the instrumented `libpikaflow_runtime.a`, including when the compiler
and runtime have been installed or relocated. It also permits linking host C
fixtures that were instrumented by this configuration.

```sh
out/sanitized/pikaflow build examples/showcase/main.pf -O2 -o out/showcase-sanitized
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 out/showcase-sanitized
```

The installed compiler retains this link configuration. Keep the matching
instrumented runtime library and a compatible configured Clang toolchain with it.
No new source-language switch or CLI flag is required. The same command in an
ordinary build does not add sanitizer flags.

## Coverage boundaries

This configuration checks **our C++ backend code while it constructs, optimizes,
verifies, JITs and emits LLVM modules**, together with our instrumented runtime
helpers. It does not rebuild or instrument the prebuilt LLVM SDK. It also does
not add LLVM AddressSanitizer/UBSan instrumentation passes to generated language
IR. Consequently, generated JIT/native machine code is not blanket-instrumented:
its calls into our C++ runtime are checked, while arbitrary generated loads or
stores do not gain compiler-inserted sanitizer checks from this option alone.

The standalone `opt` plugin is skipped in this configuration. Loading an
instrumented plugin into an ordinary uninstrumented `opt` would require a
separate sanitizer-runtime loading arrangement. The identical
`CheckedArithmeticPass` source is still compiled with sanitizers inside
`pikaflow_backend` and exercised by the instrumented compiler/backend tests.
The ordinary Linux release configuration continues to build and test the actual
standalone plugin separately.

This option supplies ASan and UBSan, not ThreadSanitizer or a proof that every
possible memory or concurrency error has been found. Test/campaign scope and
the instrumentation boundary should accompany any result.

## GDB and leak detection

LeakSanitizer cannot perform its process inspection while the inferior is
already traced by GDB. The sanitized `source_debugger` test therefore receives
`PIKAFLOW_SANITIZED_DEBUGGER=1` from CTest. Only in that mode, the Python test
inserts a GDB `set environment ASAN_OPTIONS ...:detect_leaks=0` command before
running the generated inferior.

The original `ASAN_OPTIONS` string is preserved and the final option overrides
only leak detection for that inferior. Options such as `log_path` and
`halt_on_error` remain present; `UBSAN_OPTIONS` is unchanged. ASan and UBSan stay
active in the inferior. The compiler subprocesses that build the debugger
programs inherit the original environment, including `detect_leaks=1`, as do
ordinary test compiler children. No global LSan suppression or whole-CTest
leak-disable setting is used.

The ptrace restriction and sequential option parsing can be checked in LLVM's
`compiler-rt/lib/lsan/lsan_common.cpp` and
`compiler-rt/lib/sanitizer_common/sanitizer_flag_parser.cpp`. Debugger results
should state this narrow leak-check exception alongside the normal leak-enabled
compiler/backend results.
