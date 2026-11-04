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
attributes #0 = { "pikaflow.checked-arithmetic"="v1" }
```

The attribute is a **semantic assertion supplied by the IR producer**, not a
property the plugin proves. For the identities below, the marked functions must
return the specified value without any other observable effect, failure,
synchronization, or exception. Other inputs may fail according to PikaFlow's
checked signed arithmetic rules. Attaching the attribute to a function that logs,
mutates memory, or fails for an identity input violates this contract. A matching
name alone never authorizes transformation. PikaFlow's runtime-call lowerer adds
the attribute to its known checked helpers.

| Call with constant right operand | Replacement |
| --- | --- |
| `pf_checked_add(x, 0)` | `x` |
| `pf_checked_sub(x, 0)` | `x` |
| `pf_checked_mul(x, 1)` | `x` |
| `pf_checked_div(x, 1)` | `x` |
| `pf_checked_mul(x, 0)` | `0` |
| `pf_checked_mod(x, 1)` | `0` |

Only the call is erased. Instructions computing its arguments remain, including
effectful calls and earlier checked failures. Division by zero, signed overflow,
and `INT64_MIN / -1` or `% -1` retain their runtime checks. The pass does not
reassociate expressions or apply floating-point identities.

It skips indirect calls, `invoke`, operand bundles, `musttail`, incompatible
signatures/calling conventions, unknown contract versions, and unrecognized
helper names. As an optional pass it respects `optnone`. It reports preserved
analyses conservatively after changing a function; `-verify-each` checks resulting
IR in the regression suite.

## Verification

`ctest --test-dir build/plugin -R '^llvm_pass_plugin$' --output-on-failure` runs
the real plugin through `opt`, `FileCheck`, `lli`, and `llc`. The test includes:

- Four FileCheck pipelines covering the identities, skipped call forms, invalid
  contracts, and an unmarked helper with observable side effects.
- Before/after executions for boundary values and eight fatal checked-arithmetic
  paths, including an overflow before multiplication by zero.
- A PikaFlow interpreter reference and an actual `emit-ir -O0` module transformed
  by the plugin, compiled by `llc`, linked against the real runtime, and executed
  before and after transformation.

The focused Linux LLVM 24 test has 23 execution checks. This verifies the plugin
and shared backend pass on that toolchain; it does not establish compatibility
with other LLVM major versions or Windows plugin hosts.
