# Reproducible native benchmark reports

Build the LLVM-enabled compiler first, then run:

```powershell
python scripts/benchmark.py build/native/pikaflow.exe --repeats 3 --output build/benchmark.json
```

Omit `--output` to print JSON to standard output. LLVM-disabled compilers are
rejected before compiling or recording measurements.

The tool uses two small, fixed workloads: a checked integer accumulation loop
and recursive Fibonacci. Each workload is interpreted first and compared with a
known output. Native executables are built at `-O0` and `-O2`, their outputs are
verified in an untimed warmup, and every timed run is checked again. A mismatch
fails the benchmark rather than producing a successful report.

Reports record the compiler's version and binary SHA-256, the benchmark checkout's
Git revision/dirty state when available, machine/platform information, source program, expected output,
optimization flags, build duration, repeated process durations, median, minimum,
and maximum. Measurements include process startup and output capture; they are
not isolated instruction-level measurements.
The checkout revision describes the report script's repository, not proof of
which source revision produced an arbitrary compiler binary supplied by the user.

These small synthetic cases are tools for investigation, not evidence that
PikaFlow is faster than another language or that one optimization level always
wins. Compare reports only with their machine, toolchain, correctness checks,
flags, timing scope, and noise in view.

`tests/test_benchmark.py` tests output-mismatch rejection, LLVM-disabled rejection,
and report structure without creating native executables. Actual native benchmark
execution remains a separate verification step.

## Recorded native run

The [2026-11-04 Windows report](benchmarks/windows-2026-11-04.json) was produced
from normal PowerShell with:

```powershell
python scripts/benchmark.py build/native/pikaflow.exe --repeats 3 --output out/benchmark-final-windows.json
```

All interpreter references, native warmups and timed outputs matched. Whole-process
median times were:

| Workload | `-O0` | `-O2` |
|---|---:|---:|
| Checked loop over 50,000 integers | 32.162 ms | 31.343 ms |
| Recursive Fibonacci(20) | 17.841 ms | 19.281 ms |

These short runs include startup and output capture, and their small differences
do not establish an optimization advantage. The report records all three samples,
build times, sources, expected outputs, machine information and compiler SHA-256.
The final run used clean checkout
`6ca599277480b22513e8d263680c3ee7e82cbffd` (`dirty: false`) and the freshly
validated Windows compiler, SHA-256
`602078d66fb4d3bc60b3b7a70db165a2a507ac2be6e0af33294c06ebaedb2015`.
The recursive case was slower at `-O2` in this run; these samples establish no
general optimization speedup.

The GitHub workflow in `.github/workflows/ci.yml` is manual (`workflow_dispatch`)
only. It builds and tests the interpreter on Windows and Linux when explicitly
dispatched; commits and pull requests do not automatically run it.
