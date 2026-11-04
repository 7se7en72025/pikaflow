# Generated program differential tests

`tests/fuzz_programs.py` generates bounded, well-typed programs from five
templates. Each template has a separate Python calculation of its expected
output. Compiler modes must match that expectation, so agreement between the
interpreter and LLVM backends alone is insufficient.

```sh
python tests/fuzz_programs.py build/native/pikaflow --cases 10 --seed 20261105 \
  --artifacts out/program-failures
```

On Windows, use `build/native/pikaflow.exe`. CTest registers the default campaign
as `semantic_programs`:

```sh
ctest --test-dir build/native -R '^semantic_programs$' --output-on-failure
```

## Programs and independent expectations

Templates run in a fixed round-robin order; random parameters vary the initial
values, loop counts, increments, and multipliers.

| Template | Language behavior checked | Python expectation |
| --- | --- | --- |
| Mutable arrays and loops | Indexed compound updates, while/for loops, branches, GC, reduction | Update a copied Python list, then compute its sum |
| Closure cells | Two closures mutate one captured binding across calls and GC | Apply alternating additions and multiplications to a Python integer |
| Generic record aliases | Generic nested records share one box; compound-assignment receiver is evaluated once | Apply the update recurrence and count receiver evaluations |
| Stack words and pipelines | Word calls, swap, dup, shared stack across a loop, structured pipeline | Compute exchanged values, an affine square, and the integer range sum |
| Task snapshots and GC | Two spawn-time snapshots, captured-cell mutation, managed results, repeated join, parent isolation | Make separate list copies for each snapshot and model result aliasing after join |

For example, a closure case starting at `4`, adding `3`, and multiplying by `2`
prints `7, 14, 17, 34` on its first four alternating calls. The oracle computes
those integers directly; it does not interpret generated PikaFlow source or read
compiler output to obtain expectations.

All loops have at most eight iterations. Integer inputs and multipliers are
small, with results far inside signed 64-bit bounds. Workers do not print, so task
scheduling cannot reorder expected output. The generated programs use supported
language features and do not perform unsafe native memory access or external
calls.

## Execution budget

Every program runs through the reference interpreter. When `--version` reports
LLVM enabled, every program also runs through the O0 and O2 JIT. The first program
of each template additionally builds and runs O0 and O2 native executables.

The default ten programs therefore produce **40 output comparisons and ten
native builds**. An LLVM-disabled compiler performs ten interpreter comparisons.
Native builds stay capped at ten even when `--cases` increases. The JSON summary
reports template counts, mode counts, seed, and native build count; compiler
version probes and build commands are not counted as output comparisons.

`--cases` accepts 1–1000. `--timeout` is a finite number of seconds per subprocess,
greater than zero and at most 600; the default is 60. Small case counts may omit
later templates. Use the same script revision, seed, case count, and Python
version for exact campaign regeneration.

## Failure artifacts

A mismatch, nonzero process exit, launch error, or timeout exits unsuccessfully.
The script creates a unique directory under `--artifacts`, preserving:

- `program.pf`, including the seed, case index, and template in its header.
- `failure.json`, containing parameters, mode, expected and actual output,
  stderr, exit status, command, timeout information, and replay command arrays.

Replay commands reference the preserved source rather than the removed temporary
workspace. For native execution failures they include the build command followed
by the executable command. Startup failures, such as a missing compiler, have a
JSON report without a generated program. Successful campaigns remove their
temporary source and executable files and create no failure directory.

The focused Windows LLVM-enabled run passed ten default programs and another
thirty programs with seed `42`: 140 output comparisons in total. A Linux
LLVM-disabled run also passed the ten default reference cases. The harness's
failure path was also checked with an intentionally wrong oracle, an injected
timeout, and invalid CLI bounds. These are deterministic program differential
tests, not coverage-guided fuzzing, a grammar coverage measurement, or a proof of
compiler correctness. The separate parser libFuzzer target exercises a different
input boundary.
