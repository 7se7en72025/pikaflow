# Sanitized compiler and backend verification

`PIKAFLOW_ENABLE_SANITIZERS=ON` is a Linux/Clang developer configuration for
AddressSanitizer and UndefinedBehaviorSanitizer. It instruments PikaFlow's own
C/C++ sources, including the LLVM backend, shared checked-arithmetic pass,
runtime, frontend, interpreter and host test fixtures. The option is off by
default; ordinary release builds retain their existing behavior.

The LLVM-enabled configuration requires an LLVM 24 **build-tree SDK** built with
the same Clang C and C++ drivers and upstream `LLVM_USE_SANITIZER=Address;Undefined`.
An unsanitized SDK can disagree with sanitizer annotations in LLVM headers and
allocator code. Installed SDKs do not currently provide enough exported metadata
to prove this compatibility, so this profile rejects them rather than bypassing
the check. Core-only sanitizer builds (`PIKAFLOW_ENABLE_LLVM=OFF`) need no LLVM SDK.
See the [LLVM annotation-mismatch discussion](https://github.com/llvm/llvm-project/issues/83566)
and the official [`LLVM_USE_SANITIZER` option](https://llvm.org/docs/CMake.html#llvm-use-sanitizer-string).

For example, build the documented LLVM revision with a matching toolchain:

```sh
cmake -S /path/to/llvm-project/llvm -B out/llvm-sanitized -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
  '-DLLVM_USE_SANITIZER=Address;Undefined' -DLLVM_TARGETS_TO_BUILD=X86 \
  -DLLVM_ENABLE_RTTI=ON \
  -DLLVM_BUILD_LLVM_DYLIB=ON -DLLVM_LINK_LLVM_DYLIB=ON \
  '-DLLVM_DYLIB_COMPONENTS=core;support;native;nativecodegen;passes;orcjit;executionengine;runtimedyld;jitlink;target;mc;codegen;scalaropts;instcombine;abi' \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF
cmake --build out/llvm-sanitized --target LLVM --parallel 1
```

Then select that build's configuration, keeping its `CMakeCache.txt` available:

```sh
cmake -S . -B out/sanitized -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DPIKAFLOW_ENABLE_SANITIZERS=ON \
  -DLLVM_DIR="$PWD/out/llvm-sanitized/lib/cmake/llvm"
cmake --build out/sanitized --parallel 1
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir out/sanitized --output-on-failure --parallel 1
```

Use matching Clang sanitizer runtime libraries and the documented LLVM 24 SDK.
The compatibility guard reads the SDK build's cache for its sanitizer selection
and compares compiler realpaths; `LLVMConfig.cmake` does not export
`LLVM_USE_SANITIZER`. A matching monolithic shared SDK links through imported
target `LLVM`, including its ABI component, without requiring unbuilt static
archives. Ordinary release builds retain their existing component linkage.
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
For the shared SDK profile, the installed compiler records the matching LLVM
build's library directory in its install RPATH. The SDK remains an external
dependency at that recorded path; relocating the compiler does not package or
make the LLVM SDK universally relocatable.
No new source-language switch or CLI flag is required. The same command in an
ordinary build does not add sanitizer flags.

## Coverage boundaries

This configuration checks **our C++ backend code while it constructs, optimizes,
verifies, JITs and emits LLVM modules**, together with our instrumented runtime
helpers. LLVM itself must use its matching upstream sanitizer profile. That
upstream profile currently excludes the `vptr` and `function` UBSan categories;
it also retains LLVM's unmodified `llvm/utils/sanitizers/ubsan_ignorelist.txt`
defaults for standard-library headers and CFI-specific entries. These upstream
SDK defaults are not PikaFlow-wide exclusions or custom report suppressions.
The PikaFlow option does not add LLVM AddressSanitizer/UBSan instrumentation passes
to generated language IR. Consequently, generated JIT/native machine code is not
blanket-instrumented:
its calls into our C++ runtime are checked, while arbitrary generated loads or
stores do not gain compiler-inserted sanitizer checks from this option alone.
System libraries are also outside the blanket-instrumentation claim.

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
