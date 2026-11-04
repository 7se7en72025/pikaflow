# Typed IR optimization

PikaFlow optimizes its own typed register IR before lowering it to LLVM. At
`-O0`, native/JIT lowering uses the original verified program. At `-O1`, `-O2`,
and `-O3`, it uses a verified optimized copy. The reference interpreter always
executes the original program.

The pipeline is:

```text
typed IR -> verify -> constant/CFG propagation -> verify
         -> LLVM lowering -> LLVM verifier -> LLVM optimization pipeline
         -> checked-identity LLVM pass -> LLVM verifier -> JIT/object/assembly
```

The public API is `optimizeIR(program, &statistics)` in
`include/pikaflow/optimizer.hpp`. It returns a copy and reports constants folded,
branches folded, unreachable instructions removed, and functions skipped by the
analysis bounds. Register types, parameter registers, function names, source
locations, debug binding names, nominal layouts, C ABI descriptors, and editor
symbol metadata remain attached to that copy.

## Dataflow over the CFG

The pass forms basic blocks at branch targets and after terminators. It tracks
each register's incoming fact at a block:

| Fact | Meaning |
| --- | --- |
| Unseen | No executable incoming definition has supplied a value yet |
| Constant | Every executable incoming path supplies the same integer/boolean |
| Varying | The value is unknown or incoming paths disagree |

Parameters start as varying. Literal integers/booleans create constants; moves
propagate them. At a join, two equal constants stay constant, and different
constants become varying. The worklist continues until incoming facts stop
changing. A known boolean condition propagates only its executable successor;
an unknown condition propagates both successors.

Registers may be assigned more than once. A loop's first iteration therefore
does not establish a permanent constant: a backedge that supplies another value
changes the header fact to varying and reprocesses its successors. This is a
dataflow analysis over mutable typed registers, rather than an assumption that
the IR already satisfies SSA rules.

After analysis, a known conditional branch becomes an unconditional jump. A
reachability walk removes dead instructions and remaps every surviving branch
target. The pass does not delete reachable allocations, collection, printing,
calls, FFI, tasks, raw-memory operations, or other effects merely because their
result is unused.

## Checked arithmetic

The pass folds integer addition, subtraction, multiplication, division,
remainder, negation, comparisons, boolean negation, and moves when their inputs
are known and the operation is safe. Overflow checks use integer bounds and
unsigned magnitudes; the optimizer itself does not execute overflowing signed
C++ arithmetic.

Failures remain runtime operations with their original source locations. For
example, all of these retain their checked failures:

```text
9223372036854775807 + 1
(9223372036854775807 + 1) * 0
42 / (1 - 1)
(-9223372036854775807 - 1) / -1
(-9223372036854775807 - 1) % -1
-(-9223372036854775807 - 1)
```

The multiplication by zero in the second example cannot erase the earlier
addition failure. Heap loads and calls supply varying values. This pass does
not infer cell contents, fold floating-point operations, assume anything about
an external function's result, or remove checked traps because a result is
unused.

## Inspecting the effect

Save this as `constant.pf`:

```text
fun main() -> unit:
    if (6 * 7) - 42 == 0:
        print((20 + 22) * 2)
    else:
        print(999)
    print(3 * 14)
```

Then compare:

```powershell
pikaflow dump-ir constant.pf -O0
pikaflow dump-ir constant.pf -O2
pikaflow emit-ir constant.pf -O0
pikaflow emit-ir constant.pf -O2
pikaflow dump-asm constant.pf -O0
pikaflow dump-asm constant.pf -O2
```

The optimized custom IR contains integer results instead of the constant
arithmetic and no conditional branch. Its unreachable `print(999)` arm is gone.
The baseline LLVM IR/assembly calls `pf_checked_mul`; the optimized version
does not. This effect happens in typed IR before LLVM sees the checked runtime
calls. Normal LLVM passes then optimize the lowered register slots and CFG.
The separate checked-identity LLVM pass still handles safe identities involving
unknown values, such as checked `x + 0` or `x * 1`.

The optimization makes no general speed claim. Source-location updates and
reachable runtime effects remain observable work, and performance depends on
the program and target.

## Bounds and tests

Analysis is limited to one million block/register facts and eight million
work units per function. A function exceeding either analysis bound is left
unchanged. Unresolved incoming branch facts also cause that conservative
fallback. Input and output still pass the typed IR verifier.
The verifier independently rejects functions exceeding 64 million
instruction/register pairs before allocating its definite-assignment table.

`tests/optimizer_tests.cpp` checks arithmetic boundaries, retained traps, joins,
loop backedges, unreachable effects, retained allocations/collection, source
and ABI metadata, floating-point opacity, idempotence, invalid input, and the
bounded fallback. `tests/test_optimization.py` uses a fixed random seed and
compares the unoptimized interpreter with JIT/native `-O0` and `-O2`, including
mutation, closures, task results, floating-point comparisons, and located
runtime errors. It also checks the custom IR, LLVM IR, and assembly effect of
the example above.

This pass does not implement heap alias analysis, interprocedural propagation,
register liveness for GC roots, or a general floating-point optimizer. LLVM's
own optimization pipeline remains a separate stage.
