# Checked arithmetic LLVM pass

PikaFlow's LLVM backend and standalone plugin use the same
`pika::CheckedArithmeticPass`, implemented in
[`src/llvm/CheckedArithmeticPass.cpp`](../src/llvm/CheckedArithmeticPass.cpp).
This is a new pass manager function pass, with the pipeline name
`pikaflow-checked-simplify`. Loading the plugin does not enable it automatically.

## Build and run

The standalone plugin is supported with a Linux LLVM 24 installation that enables
plugins and provides its shared LLVM library. CMake builds it by default when
`LLVM_ENABLE_PLUGINS`, `LLVM_LINK_LLVM_DYLIB`, and the imported `LLVM` target are
available. Disable it with `-DPIKAFLOW_BUILD_LLVM_PLUGIN=OFF`.

```sh
cmake -S . -B build/plugin -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR=/usr/lib/llvm-24/lib/cmake/llvm
cmake --build build/plugin --target pikaflow pikaflow_checked_passes
/usr/lib/llvm-24/bin/opt \
  -load-pass-plugin build/plugin/pikaflow_checked_passes.so \
  -passes=pikaflow-checked-simplify -verify-each -S input.ll -o output.ll
```

`function(pikaflow-checked-simplify)` also works inside a module pipeline. The
plugin installs to `lib/pikaflow/plugins`. Its entry point uses LLVM 24's plugin
API 3 from `llvm/Plugins/PassPlugin.h`; use matching LLVM tools and libraries.

The current Windows LLVM build has plugin host exports disabled. CMake therefore
skips the standalone DLL there. Windows still uses the same pass source linked
into PikaFlow's backend. A Windows `opt` plugin is not claimed or tested.
LLVM-disabled builds do not require or build this plugin.

PikaFlow's `emit-ir -O0` keeps virtual registers in stack slots. To expose their
constant operands first, use
`-passes='function(mem2reg,pikaflow-checked-simplify)'`. This runs LLVM's standard
SSA promotion and then the custom pass; it does not run PikaFlow's typed IR
optimizer. Already-promoted IR can use the pass directly.

## Trusted runtime contract

The pass requires an exact, nonvariadic C `i64(i64, i64)` signature and this
attribute on the callee:

```llvm
declare i64 @pf_checked_add(i64, i64) #0
attributes #0 = { "pikaflow.checked-arithmetic"="v2" }
```

The attribute is a **semantic assertion supplied by the IR producer**, not a
property the plugin proves. Two versions have deliberately different scope:

- **v1:** only the identities below promise the specified return value without
  observable effects, failure, synchronization, or exception. Calls with other
  constant operands remain unchanged; such a function may log for those inputs.
- **v2:** all successful inputs have PikaFlow's checked signed-i64 arithmetic
  result and no extra observable effects, failure, synchronization, or exception.
  Addition, subtraction and multiplication fail when the mathematical result
  exceeds `[-2^63, 2^63-1]`. Division truncates toward zero; remainder has the
  dividend's sign. A zero divisor, `INT64_MIN / -1`, and `INT64_MIN % -1` fail.
  The pass only folds successful inputs and retains the original failing calls,
  including their diagnostics and any failure-path effects.

The pass reads v1 without upgrading it. PikaFlow's runtime-call producer now
explicitly emits v2 for its known helpers. A matching name alone never authorizes
transformation. Marking an arbitrary function v2 while it logs on a successful
input violates that contract; marking it v1 authorizes only identity rewrites.

| Call with constant right operand | Replacement |
| --- | --- |
| `pf_checked_add(x, 0)` | `x` |
| `pf_checked_sub(x, 0)` | `x` |
| `pf_checked_mul(x, 1)` | `x` |
| `pf_checked_div(x, 1)` | `x` |
| `pf_checked_mul(x, 0)` | `0` |
| `pf_checked_mod(x, 1)` | `0` |

For v2, two literal `i64` operands also fold to a constant when successful. For
example, `pf_checked_add(20, 22)` becomes `42`, and
`pf_checked_mod(-85, 2)` becomes `-1`. The implementation uses LLVM's 64-bit
`APInt::sadd_ov`, `ssub_ov`, `smul_ov` and `sdiv_ov` rather than overflowing host
C++ arithmetic. It guards zero before division, and checks `INT64_MIN % -1`
explicitly before `APInt::srem`: APInt's remainder alone would produce zero for
that pair, hiding PikaFlow's required failure.

Only the call is erased. Instructions computing arguments remain, including
effectful calls and earlier checked failures. A chain of newly constant calls can
fold in instruction order; no reassociation or floating-point identities are
introduced. Poison and undef operands are left unchanged.

It skips indirect calls, `invoke`, operand bundles, `musttail`, incompatible
signatures/calling conventions, unknown contract versions, and unrecognized
helper names. As an optional pass it respects `optnone`. It reports preserved
analyses conservatively after changing a function; `-verify-each` checks resulting
IR in the regression suite.

## Verification

`ctest --test-dir build/plugin -R '^llvm_pass_plugin$' --output-on-failure` runs
the real plugin through `opt`, `FileCheck`, `lli`, and `llc`. The test includes:

- Seven FileCheck pipelines covering v1/v2 identities, successful constant
  boundaries, fatal calls, skipped/undef/poison call forms, invalid contracts,
  and both unmarked and legacy-v1 helpers with observable side effects.
- Before/after `lli` executions for boundary values and eight fatal IR-helper
  paths, including an overflow before multiplication by zero.
- Twenty successful constant boundary functions and a five-operation constant
  chain, plus fourteen fatal cases executed before and after transformation
  through both `lli` and native `llc` objects against the **actual C++ runtime**.
  Fatal cases must retain their error text and explicit source location.
- A PikaFlow interpreter reference and an actual `emit-ir -O0` module transformed
  by the plugin, compiled by `llc`, linked against the real runtime, and executed
  before and after transformation.

The focused Linux LLVM 24 test has 85 execution checks. This verifies the plugin
and shared backend pass on that toolchain; it does not establish compatibility
with other LLVM major versions or Windows plugin hosts.
