# PikaFlow

PikaFlow is an experimental, statically typed language and C++17 compiler that
combines structured code with typed stack programming. It has an indentation-based
frontend, a verified custom IR, a reference interpreter, a traced runtime, and an
LLVM backend for native code and ORC JIT execution.

The project draws inspiration from [Pikapika](https://github.com/7se7en72025/pikapika)
and [Goof](https://github.com/sneaky-potato/goof). Its implementation and language
rules are developed here rather than merging the two projects' source code.

**Status:** PikaFlow 0.3 adds named module namespaces, single-evaluation compound
updates, cross-function diagnostics, typed-IR dataflow optimization and a compiler
semantic index for incremental editor navigation and verified rename. It includes
generic recursive ADTs, Result propagation, isolated threaded tasks, C interop,
formatting and exact-commit Git/local packages. Native targets are Windows x64
and Linux x86-64. The [validation record](docs/validation.md) identifies frozen
source/toolchain results. Clean `5f0b109` passed 25/25 Windows native, 21/21
Windows reference and 26/26 Linux native suites; historical records remain separate.
See [feature status](docs/FEATURES.md) for supported contracts and limits.

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

The [integrated showcase](examples/showcase/main.pf) combines imported generic
recursive trees, exhaustive guarded patterns, Result `?`, a worker that runs GC,
stack arithmetic and a standard-library pipeline:

```powershell
.\build\native\pikaflow.exe run examples/showcase/main.pf -O2
```

It prints `43`, `36` and `negative seed`. Its source is also checked through the
interpreter and both JIT/native optimization levels by the language suite.

## Build on Windows

Install Git, CMake 3.24 or later, Ninja, Python 3, and Visual Studio C++ build tools
with the Windows SDK. `scripts/build.ps1` locates Visual Studio and initializes
its toolchain environment. For manual MSVC CMake configuration, use a
**Developer PowerShell**. The configured native compiler captures SDK library paths;
subsequent language `build` commands can run from ordinary PowerShell.

The reference interpreter does not require LLVM:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -InterpreterOnly -Jobs 2
.\build\interpreter\pikaflow.exe --version
.\build\interpreter\pikaflow.exe interpret .\examples\hello.pf
```

The script configures, builds, and runs CTest. `ExecutionPolicy Bypass` applies to
that script process; it does not change the machine's execution policy.
For a development iteration, `-BuildOnly` skips CTest so a focused check can run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -InterpreterOnly -BuildOnly -Jobs 2
python tests/test_namespaces_assignments.py build/interpreter/pikaflow.exe
```

Run the full suite after integrating the change; a build-only result is not test
evidence.

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
| `dump-ir FILE -O0/-O2` | Original/optimized PikaFlow typed IR, before LLVM lowering |
| `symbols FILE` | Compiler-resolved source definitions, references and types as JSON |
| `emit-ir FILE -O2` | Verified LLVM IR after the selected optimization pipeline |
| `dump-asm FILE -O2` | Assembly for the host target |

Text inspection commands accept `-o FILE`. LLVM modes accept `-O0` through `-O3`;
`--debug` requests debug information for emission. Debugger usability is checked
separately from IR or metadata emission. Output paths that refer to a loaded
source file, including hard links, are rejected.
The [typed-IR optimizer](docs/optimization.md) propagates integer/boolean constants
over the CFG, folds safe arithmetic/branches and removes unreachable instructions
before LLVM lowering. Checked failures and reachable effects are preserved; LLVM
optimization remains a separate stage.

## Language and runtime features

- Static `i64`, `f64`, `bool`, `unit`, strings, arrays, tuples, callable values,
  and `Result[T,E]`; explicit function signatures and inferred local bindings.
- `let`/`var`, lexical scopes, recursion, closures with captured mutable cells,
  pipelines, typed stack words and multiple stack outputs.
- Inferred/explicit generic function specialization; nominal generic enums with
  recursive/mutually recursive payloads and GC-managed cycles.
- Branches, loops, loop control, guarded nested patterns and exhaustive enum
  matches; Result `?` returns errors early with a checked error type.
- Named module aliases qualify functions, generic calls, types, constructors,
  exported externs and stack words. Indexed `+=` and related updates evaluate
  their base, index and right-hand side once, in order.
- Real-thread `spawn`/`join` with typed handles, isolated graph snapshots and
  automatic joins at function-scope exit.
- Checked integer arithmetic, bounds checks, recoverable Result helpers, and
  source-spanned compile diagnostics with bounded parser recovery.
- A precise non-moving mark-and-sweep collector, explicit unsafe raw-memory
  helpers, persistent GC root handles, and C symbol aliases.
- Private-by-default modules, explicit exports, and source standard-library modules.

[`examples/modules/qualified.pf`](examples/modules/qualified.pf) demonstrates
`import "math.pf" as math` and `math.twice(21)`. Bounded semantic recovery reports
independent function-body errors while discarding failed compilation transactions.

The [language extension reference](docs/language-extensions.md) specifies syntax,
inference, recursive types and expansion limits. Task heap isolation and unsafe
memory obligations are described in [runtime.md](docs/runtime.md).

```text
import "std:numeric"
import "std:sequences"
import "std:strings"
```

See [`examples/stdlib.pf`](examples/stdlib.pf) and the [library API](stdlib/README.md)
for checked parsing, sorting/search, numeric helpers, and ASCII operations.
Strings currently use byte indexing rather than Unicode grapheme indexing.

Foreign calls require `unsafe`. The LLVM backend implements a bounded C ABI
subset, including nested natural-layout records, scalar noncapturing callbacks
and explicit C integer widths; the reference interpreter has
an audited set of supported math calls rather than arbitrary FFI execution.
See [C interoperability](docs/ffi.md) for exact signatures, ownership, `--load`,
`--link`, ABI restrictions, and target-specific verification.

## Tests and measurement

```powershell
ctest --test-dir build/interpreter --output-on-failure
python tests/test_cli.py build/interpreter/pikaflow.exe
python tests/test_language_extensions.py build/native/pikaflow.exe
python tests/fuzz_frontend.py build/interpreter/pikaflow.exe --cases 30
python scripts/benchmark.py build/native/pikaflow.exe --repeats 3 --output build/benchmark.json
```

The integration suite detects whether LLVM is enabled. LLVM builds add JIT and
native output comparisons at `-O0`/`-O2`, object/IR checks, and C ABI tests.
Bounded generated-program checks complement the parser's malformed-input tests;
they are not a coverage-guided fuzz campaign.
The separate [Clang libFuzzer target](docs/fuzzing.md) completed 79,589 parser
executions under ASan/UBSan in its latest bounded campaign without a reported
finding. The final native suites include 216 compiler, 133 language, 61 namespace,
115 optimizer execution and 19 editor workflow checks on each host, with
[host-specific counts and identities](docs/validation.md).

Benchmarks verify results before recording timings, retain repeated measurements,
and record the compiler binary identity, checkout metadata, machine, source, and
flags. They do not establish a performance advantage. [Benchmark methodology](docs/benchmarking.md)

GitHub CI is **manual only** (`workflow_dispatch`) for Windows/Linux interpreter
checks. Commits and pull requests do not automatically consume runner time.

## Formatting, editor support and local packages

The developer tools require Python 3.10 or newer, without third-party Python
dependencies:

```powershell
python tools/pikaflow.py --compiler build/native/pikaflow.exe fmt examples/showcase/main.pf --check
python tools/pikaflow.py --compiler build/native/pikaflow.exe lsp
python tools/pikaflow.py init my_project --name my_project
python tools/pikaflow.py deps lock --project my_project
python tools/pikaflow.py deps check --project my_project
python tools/pikaflow.py --compiler build/native/pikaflow.exe interpret --project my_project
```

Formatting verifies compiler-token equivalence before replacing a file. The stdio
language server supports incremental/full document changes, unsaved diagnostics,
formatting, symbols/completion, compiler-backed hover, definitions, references
and verified rename. Renames are checked against the candidate binding graph to
reject capture and stay within the editable workspace. Multi-project workspaces
are indexed by project context. See [editor.md](docs/editor.md) for the advertised
protocol subset and limits.

Packages may use local paths or exact Git commit pins. Restore reviewed Git pins
explicitly, then build offline from the verified cache:

```powershell
python tools/pikaflow.py deps fetch --locked --project my_project
python tools/pikaflow.py deps check --locked --offline --project my_project
python tools/pikaflow.py --compiler build/native/pikaflow.exe interpret --locked --project my_project
```

There is no package registry or publishing service. [tooling.md](docs/tooling.md)
specifies manifests, pin/lock integrity, fetching and editor setup.

## Install and continue development

```powershell
cmake --install build/native --prefix "$PWD/build/install"
```

Installation includes the compiler, runtime archive, standard-library source
under `share/pikaflow/stdlib`, and Python tools under `share/pikaflow/tools`.
Native linking still needs a compatible host C++
toolchain and SDK. This is a development install, not a standalone distribution
or package manager.

Read the [design and decision record](docs/design.md), [language reference](docs/language.md),
[validation record](docs/validation.md), and [supported feature scope](docs/FEATURES.md).
The release has bounded, documented contracts; broader ABI, editor-protocol,
distributed-package and shared-memory concurrency features require their own
design and validation rather than being implied by these commands.
