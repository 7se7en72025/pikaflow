# Testing PikaFlow projects

New projects created by `tools/pikaflow.py init` include `tests/test_main.pf` and
a runnable assertion example. Existing project files are never replaced by init.
Tests are ordinary functions in `tests/test_*.pf` modules:

```text
import "std:testing"

fun test_arithmetic() -> unit:
    assert_equal(20 + 22, 42)
    assert_true(true)
    assert_close(0.1 + 0.2, 0.3, 0.000001)
```

Test functions must be non-generic, take no parameters and return `unit`.
Transparent aliases of `unit` are accepted. Helpers, ordinary application `main`
functions and imported modules are compiled but not selected as test cases.
Discovery uses compiler-resolved function metadata, including unused-generic
flags; it does not execute a module to discover its tests.

## Run, filter and report

```powershell
python tools/pikaflow.py --compiler build/native/pikaflow.exe test --project examples/record_project
python tools/pikaflow.py --compiler build/native/pikaflow.exe test --project examples/record_project --list
python tools/pikaflow.py --compiler build/native/pikaflow.exe test --project examples/record_project --filter "*snapshot*" --backend jit -O2
python tools/pikaflow.py --compiler build/native/pikaflow.exe test --project examples/record_project --backend native -O2 --jobs 2 --json-report out/tests.json --junit out/tests.xml
```

Create report parent directories before running the last command. Paths for
reports are relative to the invoking shell. Workers execute with the project
directory as their current directory; source imports keep their original paths.
Filters match a function name or `relative/module.pf::test_name` using glob
patterns. `--list` lists selected cases and does not write execution reports.
All discovered modules must type-check, including helpers outside the filter.

Each selected function is a separate process. A failing assertion, panic or
crashed case cannot terminate the next case. The default backend is the reference
interpreter; JIT and native modes require an LLVM-enabled compiler. Native mode
builds a private executable for each case, then runs it. `--jobs 1..8` controls
parallel workers; the default is one. Parallel tests must coordinate any external
files or services they share.

`--timeout` is a finite positive subprocess budget of at most 600 seconds
(default 30). Captured worker output is capped at 64 KiB per process, combined
across stdout/stderr; compilation and execution each have a budget in native mode.
Compiler metadata discovery has a separate 30-second/4-MiB budget. Discovery is
limited to 128 modules, 512 selected test functions and 2,048 loaded source files.
Linked test files/directories and generated/cache directories are excluded or
rejected. Missing tests, invalid signatures, collection errors, timeouts and
excess output return nonzero rather than an empty successful suite.

On Windows, a worker starts suspended, joins a lifetime-owned job, then resumes;
closing the job cleans up normal descendants even if their parent already exited.
This follows the [Windows job-object model](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects).
Linux workers use their own process groups. This is process lifetime management,
not a security sandbox: tests execute project code, and foreign calls retain the
language's explicit unsafe obligations.

## Assertions and evidence

`std:testing` exports `assert_true(bool)`, generic `assert_equal(actual, expected)`
and `assert_close(actual, expected, tolerance)` for `f64`. Equality follows the
language's operators for the concrete type; it does not add structural equality
to unsupported types. Approximate comparison uses a nonnegative absolute
tolerance. Failures panic in the case process and remain visible in its captured
diagnostic; equality failures also print the two operands.

JSON reports include schema version, compiler version/hash, backend/optimization,
each case's process results, output and timing, and collection/infrastructure
errors. JUnit reports distinguish assertion/process failures from collection
errors; forbidden XML control characters are replaced in report text. Tests and
project metadata are fingerprinted, and changes during execution make the run
unsuccessful. The compiler executable is fingerprinted separately.

Reports use atomic replacement and cannot overwrite loaded compiler sources,
the test compiler, project manifests/locks or hidden project metadata/caches.
JSON and JUnit destinations must differ. These checks are repeated after tests,
since test code can change files. Original source files are never rewritten to
select a test: the compiler's `--entry NAME` chooses a zero-argument structured
function in its own source module.

The complete [record project](../examples/record_project/README.md) combines
records, transparent aliases, generic assertions and isolated tasks. The separate
[program generator](program-testing.md) exercises independent Python oracles for
compiler development; it is not the project-test discovery mechanism.
