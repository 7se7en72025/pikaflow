# Typed IR optimization

PikaFlow optimizes its own typed register IR before lowering it to LLVM. At
`-O0`, native/JIT lowering uses the original verified program. At `-O1`, `-O2`,
and `-O3`, it uses a verified optimized copy. The reference interpreter always
executes the original program.

The pipeline is:

```text
typed IR -> verify -> constant/CFG propagation -> local scalar CSE
         -> scalar liveness/dead-result removal -> verify
         -> LLVM lowering -> LLVM verifier -> LLVM optimization pipeline
         -> checked-identity LLVM pass -> LLVM verifier -> JIT/object/assembly
```

The public API is `optimizeIR(program, &statistics)` in
`include/pikaflow/optimizer.hpp`. It returns a copy and reports constants folded,
branches folded, unreachable instructions removed, common expressions reused,
dead scalar results removed, functions skipped by constant-analysis bounds and
liveness passes skipped by their bounds. Register types, parameter registers,
function names, source locations, debug binding names, nominal layouts, C ABI descriptors, and editor
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

## Local scalar common expressions

After constant propagation, local value numbering recognizes repeated dynamic
integer arithmetic, integer/boolean comparisons and boolean negation within one
basic block. A scalar move carries its source value identity; every other
register write creates a new identity. A cached result can be reused only while
its representative register still contains that value. All incoming registers
start independently at every block boundary, so a move scanned on another CFG
edge cannot establish equality at a join. The pass does not assume SSA form.

The first checked operation stays in place. If it fails, execution terminates
there; if control reaches an identical operation on unchanged scalar values,
that second check cannot fail. It becomes a move from the earlier result, with
the second instruction's source location retained. This also works across an
intervening call, print or collection: the earlier successful operation and all
effects retain their order. Reassigning either an operand or the representative
invalidates reuse. No expressions are moved between blocks or across a branch.

Heap loads, cell contents, strings, pointers, floating-point operations,
conversions, allocations, calls, foreign calls, tasks and raw-memory operations
do not participate. There is no algebraic reassociation or commutative operand
reordering. In particular, the pass cannot cache an array read across mutation.

## Scalar dead results

A backwards CFG worklist computes register liveness, including branch joins and
loop backedges. It removes a dead result only for integer/boolean literals,
scalar zero/move, boolean negation and integer/boolean comparisons. Removing a
dead instruction also removes its operand uses, allowing dead scalar dependency
chains to disappear. A live read before a later overwrite still retains its
reaching definition. Surviving branch targets are relocated after deletion.

Potentially failing arithmetic stays even when its result is unused, unless
constant folding already proved that operation safe. Reachable heap operations,
allocations, managed moves/zeros, collection, printing, calls, foreign calls,
tasks and raw-memory operations remain. Floating-point instructions remain
opaque. Definitions named by debug metadata are retained, and metadata is
copied unchanged. This is scalar result liveness, not GC-root liveness; managed
register assignments and their root lifetimes are deliberately untouched.

For example, a word that calculates `dup 5 < drop` can lose its unused comparison
and literal. A word that calculates `dup 1 + drop` must keep the checked addition
because an input of `INT64_MAX` still fails.

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

Constant analysis is limited to one million block/register facts and eight
million work units per function. A function exceeding either bound is left
unchanged, including skipping the later passes. Unresolved incoming branch
facts also cause that conservative fallback. Local CSE retains at most 4,096
expression entries per block; reaching the cap clears its cache and loses only
optimization opportunities. Liveness independently uses the same fact/work
budgets. If it exceeds either, no dead-result edits are applied and
`livenessSkipped` increases; earlier verified constant/CSE transformations remain.
Input and output still pass the typed IR verifier.
The verifier independently rejects functions exceeding 64 million
instruction/register pairs before allocating its definite-assignment table.

`tests/optimizer_tests.cpp` checks arithmetic boundaries, retained traps, joins,
loop backedges, unreachable effects, retained allocations/collection, source
and ABI metadata, floating-point opacity, idempotence, invalid input, and the
bounded fallback. Further regressions cover dynamic expression reuse, overwritten
operands and representatives, edge-dependent register aliases, scalar copies,
loop liveness, retained heap/string operations and atomic liveness-budget
fallback. `tests/test_optimization.py` uses a fixed random seed and
compares the unoptimized interpreter with JIT/native `-O0` and `-O2`, including
mutation, closures, task results, floating-point comparisons, and located
runtime errors. It also checks the custom IR, LLVM IR, and assembly effect of
the example above. Dynamic stack-word fixtures additionally prove that one
repeated checked addition becomes a move, an unused comparison disappears,
reachable prints remain, and first/unused checked failures retain their original
source sites. These are executable regression checks, not performance claims.

These passes do not implement heap alias analysis, interprocedural propagation,
register liveness for GC roots, or a general floating-point optimizer. LLVM's
own optimization pipeline remains a separate stage.
