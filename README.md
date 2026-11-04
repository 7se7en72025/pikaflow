# PikaFlow

PikaFlow is an experimental, statically typed language and C++17 compiler that
combines structured code with typed stack programming. It has an indentation-based
frontend, a verified custom IR, a reference interpreter, a traced runtime, and an
LLVM backend for native code and ORC JIT execution.

The project draws inspiration from [Pikapika](https://github.com/7se7en72025/pikapika)
and [Goof](https://github.com/sneaky-potato/goof). Its implementation and language
rules are developed here rather than merging the two projects' source code.

**Status:** Windows x64 and Linux x86-64 native/JIT integration each pass all 13
CTest suites, including 216 compiler checks and 52 generated C ABI cross-checks
per host. This is a 0.1 implementation,
not completion of the full approved v1 roadmap. See the [validation record](docs/validation.md)
and [feature status](docs/FEATURES.md) for what has actually been checked.

## A small hybrid program

```text
word square(i64) -> i64:
    dup *

fun add(a: i64, b: i64) -> i64:
    return a + b

fun main() -> unit:
    20 |> add(22) |> print
    let answer = stack(6) -> i64:
        square
    print(answer)
```

This is [`examples/hello.pf`](examples/hello.pf). It prints:

```text
42
36
```

Structured expressions, pipelines, and stack words use the same static types.
Stack blocks share an operand stack within a function; a word consumes its
declared input suffix and returns its declared outputs, preserving the caller's
remaining stack prefix. [Language reference](docs/language.md)

## Build on Windows

Install Git, CMake 3.24 or later, Ninja, Python 3, and Visual Studio C++ build tools
with the Windows SDK. Use a **Developer PowerShell** for native compilation and
linking commands so the SDK/library environment is present.

The reference interpreter does not require LLVM:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -InterpreterOnly -Jobs 2
.\build\interpreter\pikaflow.exe --version
.\build\interpreter\pikaflow.exe interpret .\examples\hello.pf
```

The script configures, builds, and runs CTest. `ExecutionPolicy Bypass` applies to
that script process; it does not change the machine's execution policy.

For the LLVM backend, first build LLVM 24 development libraries. The current
checkout used for integration is
`0247623ddadb8d078a446a704cc20f0a8c944fc8` of `llvm/llvm-project` (24.0.0git).
LLVM APIs are version-sensitive; `LLVMConfig.cmake` and the corresponding libraries
must come from the same build.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Jobs 2
```

By default, the script looks for LLVM at the sibling path
`../llvm-work/llvm-project/build/lib/cmake/llvm`. Override it when needed:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -LLVMDir "C:\path\to\llvm\build\lib\cmake\llvm" -Jobs 2
```

The compiler is written to `build/native/pikaflow.exe`. A compiler binary reporting
`LLVM disabled` cannot run JIT/native commands; use `interpret` or enable LLVM.

## Build on Linux

The interpreter requires a C++17 compiler, CMake, Ninja, and Python 3:

```sh
cmake -S . -B build/interpreter -G Ninja -DCMAKE_BUILD_TYPE=Release -DPIKAFLOW_ENABLE_LLVM=OFF
cmake --build build/interpreter --parallel 2
ctest --test-dir build/interpreter --output-on-failure
./build/interpreter/pikaflow interpret examples/hello.pf
```

For the native backend, supply a built LLVM 24 development configuration. For
example, if that installation is at `/usr/lib/llvm-24`:

```sh
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release -DPIKAFLOW_ENABLE_LLVM=ON -DLLVM_DIR=/usr/lib/llvm-24/lib/cmake/llvm
cmake --build build/native --parallel 2
ctest --test-dir build/native --output-on-failure
```

That path is an example, not a claim that the distribution's default LLVM package
has the required version. Linux C ABI lowering also requires LLVM's `abi` library.
An LLVM source build can be used instead by pointing `LLVM_DIR` to its
`build/lib/cmake/llvm` directory.

## Run and inspect programs

The following commands use the Windows LLVM-enabled binary; on Linux use
`./build/native/pikaflow` instead:

```powershell
.\build\native\pikaflow.exe check examples/hello.pf
.\build\native\pikaflow.exe interpret examples/hello.pf
.\build\native\pikaflow.exe run examples/hello.pf -O2
.\build\native\pikaflow.exe build examples/hello.pf -O2 -o build/hello.exe
.\build\hello.exe
```

| Command | Output or action |
|---|---|
| `check FILE` | Parse, type-check, and verify custom IR; no execution |
| `interpret FILE` | Execute verified custom IR in the reference interpreter |
| `run FILE -O2` | Compile and execute with LLVM ORC JIT |
| `build FILE -o OUTPUT` | Emit an object and link a native executable with the runtime |
| `emit-obj FILE -o OUTPUT` | Write a target object file |
| `dump-tokens FILE` | Tokens with source locations and indentation markers |
| `dump-ast FILE` | Parsed syntax tree |
| `dump-ir FILE` | PikaFlow's typed register/control-flow IR, not LLVM IR |
| `emit-ir FILE -O2` | Verified LLVM IR after the selected optimization pipeline |
| `dump-asm FILE -O2` | Assembly for the host target |

Text inspection commands accept `-o FILE`. LLVM modes accept `-O0` through `-O3`;
`--debug` requests debug information for emission. Debugger usability is checked
separately from IR or metadata emission. Output paths that refer to a loaded
source file, including hard links, are rejected.

## Language and runtime features

- Static `i64`, `f64`, `bool`, `unit`, strings, arrays, tuples, callable values,
  and `Result[T,E]`; explicit function signatures and inferred local bindings.
- `let`/`var`, lexical scopes, recursion, closures with captured mutable cells,
  pipelines, typed stack words and multiple stack outputs.
- Branches, loops, loop control, and nested tuple/array/Result/range patterns.
- Checked integer arithmetic, bounds checks, recoverable Result helpers, and
  source-spanned compile diagnostics with bounded parser recovery.
- A precise non-moving mark-and-sweep collector, explicit unsafe raw-memory
  helpers, persistent GC root handles, and C symbol aliases.
- Private-by-default modules, explicit exports, and source standard-library modules.

```text
import "std:numeric"
import "std:sequences"
import "std:strings"
```

See [`examples/stdlib.pf`](examples/stdlib.pf) and the [library API](stdlib/README.md)
for checked parsing, sorting/search, numeric helpers, and ASCII operations.
Strings currently use byte indexing rather than Unicode grapheme indexing.

Foreign calls require `unsafe`. The LLVM backend implements a bounded C ABI
subset, including compatible flat tuple records; the reference interpreter has
an audited set of supported math calls rather than arbitrary FFI execution.
See [C interoperability](docs/ffi.md) for exact signatures, ownership, `--load`,
`--link`, ABI restrictions, and target-specific verification.

## Tests and measurement

```powershell
ctest --test-dir build/interpreter --output-on-failure
python tests/test_cli.py build/interpreter/pikaflow.exe
python tests/fuzz_frontend.py build/interpreter/pikaflow.exe --cases 30
python scripts/benchmark.py build/native/pikaflow.exe --repeats 3 --output build/benchmark.json
```

The integration suite detects whether LLVM is enabled. LLVM builds add JIT and
native output comparisons at `-O0`/`-O2`, object/IR checks, and C ABI tests.
Bounded generated-program checks complement the parser's malformed-input tests;
they are not a coverage-guided fuzz campaign.
The separate [Clang libFuzzer target](docs/fuzzing.md) completed 64,258 frontend
executions under ASan/UBSan without a reported failure in its bounded campaign.

Benchmarks verify results before recording timings, retain repeated measurements,
and record the compiler binary identity, checkout metadata, machine, source, and
flags. They do not establish a performance advantage. [Benchmark methodology](docs/benchmarking.md)

GitHub CI is **manual only** (`workflow_dispatch`) for Windows/Linux interpreter
checks. Commits and pull requests do not automatically consume runner time.

## Install and continue development

```powershell
cmake --install build/native --prefix "$PWD/build/install"
```

Installation includes the compiler, runtime archive, and standard-library source
under `share/pikaflow/stdlib`. Native linking still needs a compatible host C++
toolchain and SDK. This is a development install, not a standalone distribution
or package manager.

Read the [design and decision record](docs/design.md), [language reference](docs/language.md),
[validation record](docs/validation.md), and [remaining feature scope](docs/FEATURES.md).
Generics, formatting, LSP support, package management, and concurrency are staged
future work; none is implied by the current command set.
