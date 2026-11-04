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

The GitHub workflow in `.github/workflows/ci.yml` is manual (`workflow_dispatch`)
only. It builds and tests the interpreter on Windows and Linux when explicitly
dispatched; commits and pull requests do not automatically run it.
