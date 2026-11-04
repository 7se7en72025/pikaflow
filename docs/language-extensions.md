# Generic functions, guarded matches and Result propagation

## User-defined enums

```text
enum Option[T]:
    Some(value: T)
    None

fun value_or[T](option: Option[T], fallback: T) -> T:
    match option:
        case Some(value):
            return value
        case None():
            return fallback
```

An enum may have generic parameters, payload-bearing variants and empty variants.
Constructors are called as functions: `Some(42)`, `None::[i64]()` or an annotated
`let missing: Option[i64] = None()`. Type arguments are inferred from payloads and
expected types. `export enum` exports its type and constructors to direct
importers; private enum declarations and constructors retain module privacy.
Enum types have nominal identity even when payload shapes happen to match.

Every enum match must be exhaustive. A bounded pattern-matrix analysis accounts
for nested enums, tuple fields, bool values, Result alternatives and wildcards;
guards never establish coverage. Integer/string/array payloads require a binding
or wildcard for complete coverage. Analysis is bounded to 8192 expansion steps
and 128 levels, with a diagnostic on exhaustion. Constructors and pattern arity
are checked before lowering. Enum representation is private: tuple indexing,
tuple destructuring, `len` and C-record interop do not expose its internal tag or
inactive payload slots. Runtime storage uses existing traced tuples, preserving
managed payload lifetime through collection.

Direct and mutually recursive enum fields are supported, including generic lists
and trees. Canonical field layouts live in a checked nominal-layout registry;
recursive references have finite named types, while the runtime follows actual
tuple payload references. Arrays can form cycles between enum values, and the
tracing collector handles those cycles. Nonregular type growth such as
`Grow[T]` containing `Grow[[T]]` is bounded by the type/layout expansion limits
instead of constructing infinitely many specializations.

## Generic functions and words

Generic module-level functions and words are specialized at compile time. Type
parameters are declared after the name; calls usually infer them from concrete
arguments and an annotated result context:

```text
fun identity[T](value: T) -> T:
    return value

fun count[T](values: [T]) -> i64:
    return len(values)

fun main() -> unit:
    print(identity(42))
    print(count::[i64]([]))
    let integer_identity = identity::[i64]
    print(integer_identity(42))
```

`::[types]` is explicit specialization, including specialized function values.
Its syntax is distinct from array indexing. Type arguments may themselves be
arrays, tuples or function types. One concrete function body is generated for
each distinct specialization; ordinary recursive calls reuse that specialization.
Generic bodies are checked when instantiated, and an operation unsupported by a
concrete type produces a source diagnostic. There are no trait declarations or
runtime type dispatch. Generic declarations are currently module-level; concrete
closures created inside a generic function retain its substituted types. Export
and direct-import privacy rules apply to generic declarations just as to concrete
functions.

Expansion is bounded to 64 active instantiations and 256 distinct specializations
per compilation. Uninferred/conflicting arguments, unbounded type growth and
invalid operations diagnose instead of generating unchecked IR.

`case pattern if condition:` evaluates the guard after a pattern succeeds and
its bindings enter the arm's scope. A false guard tries the next arm. Guards
must produce bool and preserve the shared operand stack. Pattern bindings do not
escape their arm. Guarded cases do not establish exhaustive coverage, including
`case _ if condition:`; use an unguarded final case when needed.

Postfix `?` unwraps an `ok` value or immediately returns an `err` from the
enclosing Result-returning function:

```text
fun increment(good: bool) -> Result[i64, str]:
    let value = read_value(good)?
    return ok(value + 1)
```

The operand and enclosing function must both use Result with the same error
type. Their success types may differ. Evaluation stops on the error path; later
operations are not executed. Propagation requires an empty shared operand stack,
matching the explicit structured-return contract. It is not an exception or an
implicit conversion between error types.

`tests/test_language_extensions.py` exercises positive and negative source
programs through the reference interpreter and, when LLVM is enabled, JIT/native
execution at both optimization levels.
