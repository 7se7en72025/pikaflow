# Design and decision record

PikaFlow is intended to teach real compiler engineering through a language that
combines structured syntax and typed stack operations. The project is implemented
in C++17 and uses LLVM's C++ APIs. The design deliberately exposes observable
intermediate stages and keeps an independent reference execution path.

This document records decisions delegated and approved during project planning.
It separates implemented behavior from the wider roadmap. The authoritative
syntax/behavior reference is [language.md](language.md), implementation status is
[FEATURES.md](FEATURES.md), and verification evidence is [validation.md](validation.md).

## Compiler path

```text
UTF-8 source
  -> lexer (tokens, indentation, locations)
  -> parser (AST)
  -> name/type/stack-effect checking
  -> custom typed register/control-flow IR
     -> reference interpreter -> common runtime
     -> verified scalar optimization -> LLVM IR -> object + runtime -> executable
                                           -> ORC JIT -> execution
```

The custom IR uses typed virtual registers and explicit control-flow operations.
It is not presented as LLVM SSA: control-flow joins may write a shared virtual
register. The verifier checks instruction types, signatures, valid targets,
side-effect destinations, closure environments, and definite assignment at joins.
The LLVM backend lowers these registers to local storage and allows LLVM's
optimization pipeline to promote/simplify that representation.

The interpreter and LLVM backend share language/runtime contracts but implement
execution separately. Agreement between interpreter, JIT, and optimized/unoptimized
native output is evidence for tested programs; it is not a proof of compiler
correctness. LLVM's verifier likewise checks IR invariants rather than proving
the source translation correct.

## Structured and stack composition

The language uses indentation-based structured blocks, named functions, local
bindings, and explicit stack blocks. A stack block is an expression whose declared
outputs become structured values. Multiple outputs are represented as tuples.
Values remaining beneath those outputs stay on the enclosing function's operand
stack and can be consumed by later blocks.

```text
stack(10, 20):
    swap
let difference = stack() -> i64:
    -
```

Each function has a shared stack across its own blocks. This is a deliberate,
statically checkable interpretation of shared stack programming rather than an
untyped process-global operand stack. A typed word consumes the declared suffix
of its caller's stack and produces its declared outputs. The prefix remains
untouched by that call. Structured function calls pass their normal parameters
and result; hidden cross-function stack mutation is not allowed.

Stack inputs are pushed left-to-right, with the last input on top. Binary postfix
operators pop the right operand before the left. Branches must leave compatible
stack shapes/types, and loop backedges preserve their declared carried state.
Break/continue use their appropriate loop exit/backedge contracts. Structured
returns cannot silently discard residual stack values.

These rules make the interaction with LLVM control flow and SSA lowering visible
while keeping stack effects statically checkable. Stack blocks have lexical
local scopes even though their operand stack is shared.

## Types and evaluation

- Static types with explicit function signatures and locally inferred initializers.
- `let` freezes the binding; `var` allows same-typed reassignment. Referenced array
  objects and record fields can still be mutated through a `let` binding.
- Arithmetic requires matching numeric types. Potentially lossy numeric conversion
  is explicit. `unit` represents absence of a result and is not a container element
  or parameter value.
- Evaluation is left-to-right, including the callable before its arguments.
  Structured boolean operations short-circuit; postfix boolean inputs have already
  been evaluated when their operator executes.
- Pipelines evaluate their left input once and insert it as the first call argument.
- Integer overflow, invalid division/remainder, and invalid bounds produce defined
  runtime failures. Optimization must not introduce unchecked signed-overflow
  assumptions that change these semantics.

Keywords, canonical type names, and built-in operations are reserved in declarations.
This prevents direct calls and first-class references from resolving the same name
to different meanings. Module/private symbol resolution is performed before
lowering to globally unique IR names.

## Records and transparent aliases

Records add nominal types and named fields over the existing traced tuple/layout
machinery. A constructor evaluates supplied field expressions in written order
and stores them in declaration order; field updates evaluate their receiver once.
Record assignment/capture preserves reference aliases, while task transfer still
copies the reachable graph. Recursive record references use the canonical nominal
registry. The compiler checks field identity and module privacy before lowering.
These managed records do not acquire a C struct ABI through their source shape.

Transparent aliases expand in their defining module and retain the underlying
type's identity. They add no constructor or runtime wrapper. Explicit expansion
and node budgets reject cycles/pathological growth; recursion through a nominal
record stays representable. Public aliases may expose private nominal types as
opaque handles without exporting their fields or constructors. See
[language.md](language.md) for written syntax and exact limits.

## Closures and memory

Closures have typed signatures and managed environments. Immutable captures retain
their original value; mutable captures share cells whose lifetime can outlive the
declaring function. References to arrays are captured as references to the same
object. Named nested functions can capture their own function cell, supporting
recursive closures after the outer function returns.

Managed strings, arrays, tuples, closures, and cells use a precise, non-moving
mark-and-sweep collector. Runtime frames expose managed-reference roots, and the
collector traverses object children. Stable addresses simplify interaction with
foreign code, but rooting is still required: a stable address does not guarantee
that an otherwise unreachable object stays alive.

The current implementation roots managed register slots for a frame's lifetime.
This can keep dead temporaries alive until their slots are overwritten or the
function returns. It is a correctness-oriented baseline, not a liveness-optimal
collector or a claim of minimal memory use.

Foreign allocation/load/store/free and C calls require explicit `unsafe` blocks.
Runtime-owned raw allocations and C-library allocations have separate ownership.
`pin`/`unpin` create/release persistent GC roots; a pin handle is not an object's
data pointer. `cstr` borrows a string buffer. General borrow checking, automatic
foreign ownership inference and moving/concurrent GC are outside this model.
Explicit unsafe `offset` performs checked element/byte arithmetic for owned
blocks and overflow checks for foreign addresses; foreign validity and lifetime
are still caller obligations.

LLVM supplies integration mechanisms rather than a ready-made collector; this
runtime supplies the collector and allocation policy. [LLVM GC documentation](https://llvm.org/docs/GarbageCollection.html)

## Recoverable errors and matching

`Result[T,E]` distinguishes ordinary recoverable failures from programming errors.
`ok`/`err` construct alternatives. `unwrap` and `error` validate the active
alternative before exposing its payload. Nested tuples, arrays, Result constructors,
bindings, literals, exclusive integer ranges, and wildcards can be matched.

Pattern bindings are local to a case. Array shape checks occur before element
loads. Exhaustive returning branches are treated as terminating control flow;
`panic` is divergent rather than an ordinary value-returning expression.
Generic recursive enums use opaque nominal types and a canonical field-layout
registry checked by the typed IR verifier. A bounded pattern matrix analyzes
nested constructor coverage. Guards run after pattern bindings and do not
establish coverage. Result `?` branches to an early error return with the checked
enclosing error type; it does not unwind through JIT frames.

## Modules and C boundary

Files are namespaces with explicit `export` on functions, words, enums, records,
type aliases and extern declarations. Direct imports expose
only directly imported exported declarations; transitive private names and private
extern aliases do not leak. Import cycles diagnose. `std:` resolves bundled
standard-library modules from the development or installed library layout.
`import "file.pf" as module` instead exposes only qualified exports, including
generic calls, enum types/constructors, foreign declarations and typed words.
The compiler resolves qualified names to the same concrete declarations before
IR lowering, so namespaces do not introduce a runtime lookup mechanism.
Compound indexed updates keep one AST lvalue and evaluate base, index, old value
and right-hand side once in order; they are not desugared by duplicating a
potentially effectful lvalue expression.

An extern source alias is independent of its actual C symbol, allowing a name
such as `libc_free` to refer to `"free"`. Calling conventions, argument layout,
returns, and vararg promotions are target-specific. The backend supports a bounded
nested natural C-record/scalar-callback subset rather than arbitrary C/C++ ABI compatibility.
Its constraints, examples, and target-specific tests are in [ffi.md](ffi.md).

The reference interpreter supports an audited set of exact foreign math signatures.
It does not cast arbitrary addresses to guessed C++ function types or emulate the
whole platform ABI. Foreign libraries can be loaded for JIT execution or linked
for native execution where the declared boundary is supported.

## Build, diagnostics, and observability

LLVM 24 APIs and a recorded source revision are used for integration. A working
LLVM configuration/library set is versioned as a unit; following LLVM main requires
deliberate rebuilds, adaptation, and tests rather than assuming compatibility.

Tokens, AST, custom IR, LLVM IR, and target assembly are independently inspectable.
At optimized levels, the typed register IR first passes through verified integer/
boolean constant propagation, checked arithmetic/branch folding and unreachable
instruction removal, followed by local scalar CSE and backwards CFG liveness.
Value identities account for mutable registers; liveness accounts for backedges.
The first possible checked failure and reachable effects remain. Managed
assignments/roots and floating-point operations stay opaque to these new passes.
LLVM's optimization pipeline remains a separate stage.
[Optimization details](optimization.md)

The backend and Linux standalone plugin share one checked-arithmetic LLVM pass.
The plugin transforms only exact helper signatures carrying the explicit
`pikaflow.checked-arithmetic=v1` producer contract. Names alone are insufficient:
the producer promises the identity inputs have no other effects or failures.
The Linux `opt` tests include invalid contracts and effectful unmarked helpers.
[Plugin contract](llvm-pass-plugin.md)
Compiler errors retain filename/line/column and a source caret. Parser recovery
collects up to 20 independent top-level errors. Semantic function-body checking
uses transactions: a failed function discards emitted code/specializations/layouts,
independent functions are checked, and any errors prevent producing a Program.
Lexical and declaration/signature validation still fail at the first error.
Resource limits prevent pathological nesting from
exhausting the native parser stack.

An optional compiler semantic source pass reuses the module/export tables and
AST lexical scopes to emit definition/reference IDs. It includes writes,
qualified aliases/types, capture references and unused generic templates, while
omitting synthetic entry/argument names as rename targets. This powers editor
hover, definitions, references, full semantic tokens, inferred-local hints and
candidate-checked rename; the editor does not
infer bindings with a separate regex parser. Incremental/unsaved buffers use
original-path compiler overlays. Workspace groups are checked in their project
contexts. [Editor contract](editor.md)

Output protection rejects loaded source paths and equivalent hard links before
writing. Native linking launches the compiler driver with structured arguments
rather than constructing a shell command from user paths.

Tests cover valid and invalid programs, arithmetic/bounds failures, closures/GC,
stack joins, module visibility, aliasing, source preservation, ABI fixtures, and
execution-mode agreement. Generated arithmetic programs use an independent Python
reference with truncating division/remainder semantics. The manual GitHub workflow
does not dispatch automatically on pushes or pull requests.

## Project execution and independent expectations

Project tests are ordinary non-generic zero-argument unit-returning functions.
Compiler metadata discovers them, and `--entry` selects them without rewriting
source. Assertions live in a source standard-library module. Each case has its
own bounded worker process; Windows starts it suspended and assigns a lifetime
job before resuming, while Linux uses process groups. This supports cleanup,
timeouts and output bounds rather than sandboxing project code. Source/compiler
fingerprints and atomic JSON/JUnit reports make the result inspectable.
[Testing contract](testing.md)

Compiler development also uses generated source with independently calculated
Python expectations. Five bounded templates cover mutable arrays, captured cells,
generic record aliasing, stack/pipeline composition and isolated task graphs.
The seed and template parameters reproduce a campaign; independent expectations
can detect a mistake shared by interpreter and LLVM paths. This complements
parser libFuzzer and sanitizer runs rather than measuring semantic coverage.
[Program oracle](program-testing.md)

## Staged scope

| Decision | Current direction | Later work |
|---|---|---|
| Native platforms | Windows x64 and Linux x64 targets | Additional targets need separate ABI/runtime tests |
| Optimization | Verified typed-IR constants, local scalar CSE/liveness, LLVM pipelines and shared checked identities | Heap/interprocedural/floating-point custom analyses require separate legality tests |
| Debugging | Real GDB scalar/captured-local stepping/watch tests and Windows CodeView inspection | Managed visualization, optimized-code quality and graphical IDE integration remain unverified |
| Standard library | Numeric, sequence, and ASCII/byte-string source modules | Additional APIs require specified contracts/tests |
| Generics | Inferred/explicit module-level monomorphization; recursive nominal ADTs | Bounded specialization/type expansion; no runtime generic dispatch |
| Formatter | Compiler-token-equivalent formatting with atomic replacement | No expression/comment reflow |
| LSP | Compiler diagnostics, incremental sync, semantic navigation/rename/tokens/hints, formatting and completion | Bounded project contexts and advertised methods; no claim of every protocol extension/editor |
| Package management | Local paths and exact-commit Git pins with deterministic locks, explicit fetching and verified offline use | No registry service, version solver, publishing or install hooks |
| Concurrency | Snapshot-isolated real threads, typed joins and independent heaps | No shared managed heap, cancellation or thread pool |

The project's ambition is measured through specified semantics, working programs,
reproducible builds, and tested behavior. Neither feature count nor selecting a
more difficult design establishes a "10x" correctness or performance advantage.
