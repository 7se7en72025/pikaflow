# PikaFlow language reference

This document describes the implemented language subset. The wider approved
project scope and remaining work are tracked in [FEATURES.md](FEATURES.md).
Examples are executable programs in [`examples/`](../examples/).
Generic functions, explicit specialization, guarded cases and Result propagation
are specified in [language extensions](language-extensions.md).

## Source layout

Source files use the `.pf` extension and UTF-8 string contents. Identifiers use
ASCII letters, digits, and underscores and cannot begin with a digit. Blocks are
introduced by `:` and indentation; use spaces, not tabs. Four spaces are the
recommended style. A dedent must match an enclosing indentation level. Blank
lines and comments do not introduce or close blocks.

Both `# comment` and `// comment` are accepted outside strings. Expressions can
span lines inside parentheses or brackets. Statements normally end at a newline;
`;` can separate simple statements. `pass` represents an empty block.

```text
fun main() -> unit:
    # Indented function body.
    print(42)
```

The entry function must be named `main`, take no arguments, and return either
`unit` or `i64`. Returning `i64` supplies the program exit status; output is
separate and is produced by `print`.

## Types and values

| Type | Example | Meaning |
|---|---|---|
| `i64` | `42`, `-12` | Signed 64-bit integer |
| `f64` | `1.5`, `2e3` | 64-bit floating-point number |
| `bool` | `true`, `false` | Boolean |
| `unit` | A function without a result | No value |
| `str` | `"hello"` | Immutable byte string |
| `[T]` | `[1, 2, 3]` | Homogeneous, fixed-length mutable array |
| `(T, U)` | `(42, true)` | Heterogeneous tuple |
| `fn(T)->U` | A function or closure | Callable value |
| `Result[T, E]` | `ok(42)` | Recoverable success or failure |
| `ptr[T]` | `alloc_i64(2)` | Typed foreign-memory pointer, usable within `unsafe` |

`str` and `string` are aliases. `int` and `float` are accepted aliases for `i64`
and `f64`. Functions require explicit parameter types. Local variable types are
inferred unless annotated. An empty array needs an explicit element type.

```text
let answer = 42
var total: i64 = 0
let values: [i64] = []
```

`let` prevents reassignment of the binding. It does not freeze an array object:
`let values = [1]` followed by `values[0] = 2` is valid. `var` permits reassignment
to a value of the same type. Names obey lexical block scope.

Language keywords, canonical type names, and runtime built-in names are reserved
for function/word declarations, parameters, and local bindings. For example,
declaring a function named `len` or a variable named `print` is rejected rather
than inconsistently shadowing the built-in operation. Stack-block locals also
obey block scope; sharing the operand stack does not expose their local bindings.

Integer and floating-point operands do not implicitly mix. Use `f64(value)` or
`i64(value)` explicitly. Float-to-integer conversion truncates toward zero and
fails for non-finite or out-of-range inputs. Integer addition, subtraction,
multiplication, negation, division, and remainder check invalid operations.
Integer overflow and division by zero produce a runtime failure. Floating-point
arithmetic follows the host's ordinary IEEE-style operations; integer overflow
rules do not apply to floats.

Strings support `\n`, `\r`, `\t`, `\0`, `\\`, `\"`, and `\'` escapes. `len(text)`
counts bytes, and indexing a string returns an unsigned byte represented as
`i64`. Unicode character indexing is not implemented.

## Expressions and evaluation

Arithmetic operators are `+`, `-`, `*`, `/`, `%`; `%` requires integers. Numeric
comparisons are `==`, `!=`, `<`, `<=`, `>`, `>=`. Strings and booleans support
equality comparisons. Boolean operators are `and`, `or`, `not` with `&&`, `||`,
`!` aliases. Structured `and`/`or` short-circuit.

From weakest to strongest, precedence is pipeline, `or`, `and`, equality,
ordering, addition/subtraction, multiplication/division/remainder, unary
operators, calls/indexing. Binary operators at the same level associate left.
Arguments and operands evaluate left-to-right; a callable expression is
evaluated before its arguments.
Compound assignments `+=`, `-=`, `*=`, `/=` and `%=` work on mutable locals and
array elements. An indexed target evaluates its base and index once, reads the
old element, evaluates the right-hand side, computes the checked operation and
stores the result. Thus `get_array()[next_index()] += amount()` preserves that
left-to-right order even when all three expressions have side effects.

Pipelines insert the left-hand value as the first argument. That value is
evaluated once:

```text
20 |> add(22) |> print
# Equivalent result to print(add(20, 22)).
```

## Functions, closures, and returns

```text
fun add(a: i64, b: i64) -> i64:
    return a + b

fun counter(start: i64) -> fn()->i64:
    var value = start
    fun next() -> i64:
        value = value + 1
        return value
    return next
```

Functions can recurse. Nested functions and anonymous functions are closures:

```text
let double = fun(value: i64) -> i64:
    return value * 2
```

Captured mutable bindings share managed cells, so changes remain visible after
the enclosing function returns. Immutable bindings keep their original value;
capturing an array copies its reference rather than copying the array contents.
The garbage collector traces closure environments.

`return` without an expression is for `unit` functions. Non-unit functions must
return a correctly typed value on every path recognized by the compiler.
Structured functions cannot return with unconsumed operand-stack values.

## Structured control flow

```text
if condition:
    print(1)
elif other_condition:
    print(2)
else:
    print(3)

while condition:
    # Reevaluate condition each iteration.
    work()

for value in [1, 2, 3]:
    print(value)

for index in range(0, 10, 2):
    print(index)
```

Conditions require `bool`; integers do not implicitly become booleans.
`break` and `continue` require an enclosing loop. `range(end)`,
`range(start, end)`, and `range(start, end, step)` use an exclusive end; a zero
step fails. Iterating a string yields its bytes. Loop `else` clauses are not
implemented and are rejected.

`match` handles literal/equality cases, tuple and array destructuring, identifier
bindings, `ok(...)`/`err(...)` Result patterns, integer `range(low, high)` patterns,
and an optional final wildcard:

```text
match value:
    case 1:
        print("one")
    case _:
        print("other")
```

Patterns can nest: `case [(first, second), (_, last)]:` matches an array containing
two tuples. Array patterns require an exact length; a length mismatch tries the
next case before indexing. `range(low, high)` excludes its upper endpoint.
Pattern-bound names are local to that case body. Tuple values can also be
destructured with `let (a, b) = expression`. `case pattern if condition:` adds a
boolean guard. Nominal `enum` declarations support generic and recursive payloads;
their matches are checked for exhaustiveness, including nested enum/tuple/bool/
Result combinations. Guarded cases do not establish coverage. See the
[generic/ADT reference](language-extensions.md) for constructors and examples.

## Typed stack programming

Every function has an operand stack shared by its stack blocks. Structured
functions pass arguments/results normally; stack words consume a declared suffix
of the caller's stack and produce their declared outputs. There is no unchecked
process-global operand stack.

```text
word square(i64) -> i64:
    dup *

fun main() -> unit:
    let factor = 3
    let result = stack(14) -> i64:
        factor *
    print(result)
```

Explicit stack inputs are pushed left-to-right: the last argument is on top.
Postfix operations pop the right operand first, so `20 3 -` computes `17`.
Structured variables are visible within a stack block. Bare user-defined word
names apply their declared stack effects.

| Operation | Stack effect |
|---|---|
| `dup` | `a -- a a` |
| `swap` | `a b -- b a` |
| `drop` | `a --` |
| `over` | `a b -- a b a` |
| `rot` | `a b c -- b c a` |
| `+`, `-`, `*`, `/`, `%` | `a b -- result` |
| Comparison | `a b -- bool` |
| `print`, `dump` | `value --` |
| `len` | `sequence -- i64` |
| `neg`, `not` | `value -- result` |
| `and`, `or` | `bool bool -- bool` (both inputs already evaluated) |
| `gc` | `--` |

A block's declared output is removed from the operand stack and becomes its
structured expression value. A tuple output removes multiple stack entries.
Without `->`, the block returns `unit` and leaves its remaining entries on the
function's shared operand stack:

```text
stack(10, 20):
    swap
let difference = stack() -> i64:
    -
```

Stack height/types are checked at branches and loop backedges. A stack `if:`
consumes a boolean condition from the top; a stack `while:` consumes its condition
and requires its body to replenish the next condition. Stack underflow and
incompatible paths are compile-time errors. A word may not leave undeclared
outputs.

## Arrays, tuples, and garbage collection

Array indexing and stores check bounds at runtime. Tuple indices must be integer
literals so their types can be determined statically. Arrays are fixed-length;
resizing and slicing are not implemented.

Strings, arrays, tuples, closures, and capture cells use a precise non-moving
mark-and-sweep collector. Compiled/interpreted frames expose managed-reference
roots; allocated objects trace their children. `gc()` forces collection for
testing. Foreign `ptr` values are not automatically owned or traced.

Foreign-memory operations require an explicit `unsafe:` block. The available
built-ins are `alloc_i64(count)`, `alloc_f64(count)`, `alloc_bool(count)`,
`load(pointer, index)`, `store(pointer, index, value)`, `offset(pointer, elements)`
and `free(pointer)`. Allocations must be freed explicitly. The runtime checks
its own allocation bounds, type and alignment, including interior/one-past offsets.
Foreign pointers have no allocation metadata: validity, bounds, aliasing and
lifetime remain the unsafe caller's responsibility. `ptr[unit]` uses byte
offsets. A one-past pointer can be formed but not dereferenced. Stale-pointer
detection is not a use-after-free guarantee. `free` accepts only the original base
of a runtime-owned block; foreign allocations require their matching foreign API.

```text
unsafe:
    let memory = alloc_i64(2)
    store(memory, 0, 42)
    print(load(memory, 0))
    free(memory)
```

`cstr(text)` exposes a managed string's NUL-terminated buffer as `ptr[unit]`.
The string must remain alive while foreign code uses that buffer. `pin(value)`
returns a persistent root handle for a managed object; `unpin(handle)` releases
that handle. A pin handle is not the object's data pointer. Handles retained
without `unpin` retain the managed object.

## Recoverable errors

```text
fun divide(a: i64, b: i64) -> Result[i64, str]:
    if b == 0:
        return err("zero denominator")
    return ok(a / b)
```

`is_ok(result)` returns a boolean, `unwrap(result)` extracts a success, and
`error(result)` extracts a failure payload. Accessing the inactive alternative
terminates with a runtime diagnostic. `err(...)` requires an expected
`Result[T,E]` type from an annotation or return signature. Result alternatives may
include arrays, tuples, user-defined recursive enums and other supported values.
Postfix `?` extracts an `ok` payload or immediately returns `err` from the
enclosing Result-returning function. Error types must match; success types may
differ. The shared operand stack must be empty at propagation, matching the
structured-return contract. This is checked control flow, not exception unwinding.

## Modules and foreign calls

`import "relative/path.pf"` loads another source file relative to the importer.
`import "std:numeric"`, `"std:sequences"`, and `"std:strings"` resolve the bundled
source library in development builds and installed compiler layouts.
`export fun ...`, `export word ...`, `export extern fun ...` and `export enum ...`
expose declarations; other declarations stay private. Import cycles and ambiguous
exported names diagnose. A named import exposes the target's exports under a
namespace rather than adding them to the unqualified lookup set:

```text
import "modules/math.pf" as math
fun main() -> unit:
    print(math.twice(21))
```

Namespaces also qualify generic functions (`math.identity::[i64](42)`), enum types
(`types.Option[i64]`), constructors/patterns (`types.Some(value)`), exported externs
and bare stack words. Only directly exported declarations are accessible;
aliases do not bypass privacy or expose transitive imports. Aliases must be
unique non-reserved names in their module and cannot be reused for its bindings.
Extern source aliases are also module-local; an imported module's private extern
declaration is not exposed to its importer. Export an ordinary wrapper function
when a module needs to offer a checked interface around foreign code.

```text
extern fun pf_checked_add(a: i64, b: i64) -> i64
```

A source alias can refer to a differently named foreign symbol. This is useful
when the C symbol conflicts with a reserved language helper:

```text
extern fun libc_free(pointer: ptr[unit]) -> unit = "free"
```

The alias is the name used in PikaFlow calls; the string is the actual linker/JIT
symbol. Empty symbols and embedded NUL bytes are rejected.

Extern declarations use the platform C calling convention. Scalar/pointer
signatures, nested natural-layout C-record tuples, explicit C integer widths and supported variadic promotions can
be emitted by the LLVM backend; foreign symbols must be supplied by linked code
or the JIT process. The reference
interpreter does not execute arbitrary foreign functions. Calls must appear in an
explicit `unsafe:` block; declarations alone do not execute foreign code. Use
`export extern fun` to expose a foreign declaration to direct importers.
The exact supported record types and platform restrictions are documented in
[C interoperability](ffi.md). Noncapturing scalar callbacks use unsafe
`c_callback(named_function)`. Packed records, unions, bitfields, C++ object ABI,
captured/managed callback signatures and address-of syntax are outside this
boundary. Declaring an incorrect foreign signature
does not become safe merely because the program type-checks.

## Source standard library

The modules in [`stdlib/`](../stdlib/) are implemented in PikaFlow itself.
`numeric.pf` provides recoverable absolute value/division/clamping and a typed
`square` stack word. `sequences.pf` provides checked sum, minimum, in-place
insertion sort, and binary search for integer arrays. `strings.pf` provides ASCII
classification and decimal `parse_i64`, which supports both i64 endpoints and
returns recoverable errors for invalid/out-of-range input.

See the [library API reference](../stdlib/README.md) for exact contracts and
[`examples/stdlib.pf`](../examples/stdlib.pf) for imports and use.

## Diagnostics and resource limits

Frontend/type errors include filename, line, column, source line, and a caret.
The parser collects up to 20 independent errors, resynchronizing at unindented
declarations. The first error keeps its original source location, and subsequent
diagnostics include their own locations. After declaration/signature validation,
semantic checking collects up to 20 function-body errors. Failed function
transactions discard emitted functions, specializations and nominal layouts;
any semantic error prevents returning executable IR. Lexical and declaration/
signature errors still stop their phase at the first error. Excessively nested input is rejected rather
than exhausting the native parser stack. CLI source files are limited to 16 MiB.
Within-function semantic recovery and Unicode display-column tracking remain
future work.
