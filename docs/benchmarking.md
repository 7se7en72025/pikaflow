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

## 0.4 recorded native run

The [Windows 0.4 report](benchmarks/windows-0.4.0-2026-11-05.json) used clean
final revision `1cfd6aab929c0c608061159900270981c2b3d12c` after host tests and
package probes finished. It reports `PikaFlow 0.4.0; LLVM enabled;
x86_64-pc-windows-msvc`, binary SHA-256
`a249b48ae614d847fe6253aa071558053aa62647d043701912349f65753b7df4`,
matching the [final Windows manifest](validation/windows-0.4.0-2026-11-05.json).

```powershell
python scripts/benchmark.py build/native/pikaflow.exe --repeats 3 --output out/benchmark-0.4-final-windows.json
```

Interpreter expectations, native warmups and every timed output matched.

| Workload | `-O0` median | `-O2` median |
|---|---:|---:|
| Checked loop over 50,000 integers | 26.218 ms | 24.860 ms |
| Recursive Fibonacci(20) | 15.972 ms | 16.566 ms |

All three samples, flags, sources and machine identity remain in the report.
Process startup and captured output are included. The workload medians move in
different directions; these noisy samples establish no general speedup or
performance multiplier. Earlier release reports below are historical records,
not controlled before/after comparisons with this feature release.

## Historical 0.3.1 recorded native run

The [0.3.1 Windows report](benchmarks/windows-0.3.1-2026-11-05.json) was captured
after both host suites completed, from clean frozen commit
`d77a3b5cdab180e061d66f0fee18634a913ad001` (`dirty: false`):

```powershell
python scripts/benchmark.py build/native/pikaflow.exe --repeats 3 --output out/benchmark-0.3.1-final-windows.json
```

The native compiler reports `PikaFlow 0.3.1; LLVM enabled;
x86_64-pc-windows-msvc`, SHA-256
`046b0c02eeb05b5f6cb798676e5df4f5c0c3115603c81e14d7f1c64b37cd5dde`,
matching the [Windows validation record](validation/windows-0.3.1-2026-11-05.json).
All interpreter references, native warmups and three timed outputs per
configuration matched.

| Workload | `-O0` median | `-O2` median |
|---|---:|---:|
| Checked loop over 50,000 integers | 30.401 ms | 28.232 ms |
| Recursive Fibonacci(20) | 17.231 ms | 17.417 ms |

Startup and captured output are included. These small process-inclusive samples
are noisy; the different directions of the workload medians establish no
general optimization speedup or cross-language advantage. The report preserves
all samples and machine/source identities. The earlier results below are
history, not a controlled performance comparison with this corrective release.

## Historical 0.3 recorded native run

The [0.3 Windows report](benchmarks/windows-0.3-2026-11-04.json) was captured from
clean frozen commit `5f0b10915013d38d834ad72dca785bf8f9f8e4fa` (`dirty: false`)
after the host suites finished:

```powershell
python scripts/benchmark.py build/native/pikaflow.exe --repeats 3 --output out/benchmark-0.3-final-windows.json
```

The compiler reports `PikaFlow 0.3.0; LLVM enabled; x86_64-pc-windows-msvc`;
its SHA-256 is
`0ef879d8c236c91a850c4e10234a674080d548ff57725a127d77f4bc86e4e778`,
matching the [Windows validation manifest](validation/windows-0.3-2026-11-04.json).
Interpreter references, native warmups and all three timed outputs per
configuration matched the expected results.

| Workload | `-O0` median | `-O2` median |
|---|---:|---:|
| Checked loop over 50,000 integers | 27.957 ms | 28.602 ms |
| Recursive Fibonacci(20) | 17.252 ms | 18.722 ms |

These complete-process samples include startup and output capture. Both medians
were higher at `-O2` in this run; no optimization speedup or cross-language
performance advantage is established. The report retains every sample, source,
expected output, flag and machine identity. Earlier reports below describe their
own source/binary snapshots and are preserved as history.

## Historical 0.2 native run

The [0.2 Windows report](benchmarks/windows-0.2-2026-11-04.json) was captured from
clean release commit `39fcac34a3528a4e74ea1cdbbff58dd9e433a19b` (`dirty: false`):

```powershell
python scripts/benchmark.py build/native/pikaflow.exe --repeats 3 --output out/benchmark-0.2-windows.json
```

The compiler identified itself as `PikaFlow 0.2.0; LLVM enabled;
x86_64-pc-windows-msvc`, binary SHA-256
`ceecd3867110d6a03e868bd1e5a3f4f764667205a47f24f45d3736914ed9f8d7`.
All interpreter references, warmups and timed outputs matched.

| Workload | `-O0` median | `-O2` median |
|---|---:|---:|
| Checked loop over 50,000 integers | 34.778 ms | 31.075 ms |
| Recursive Fibonacci(20) | 20.124 ms | 19.283 ms |

The report retains sources, expected outputs, all three samples, build times,
machine details and exact identities. Startup/output capture and measurement
noise are included; these samples establish no general performance or optimization
advantage and are not a comparison with another compiler or language.

## Historical 0.1 native run

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
