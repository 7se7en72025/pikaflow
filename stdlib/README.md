# PikaFlow standard-library modules

These modules are ordinary PikaFlow source. Import their exported names with
paths relative to the importing file, for example:

```text
import "../stdlib/numeric.pf"
import "../stdlib/sequences.pf"
import "../stdlib/strings.pf"
```

The complete example is [`examples/stdlib.pf`](../examples/stdlib.pf).

## `numeric.pf`

| Export | Behavior |
|---|---|
| `abs_checked(i64) -> Result[i64, str]` | Absolute value; MIN returns an error |
| `min_i64(i64, i64) -> i64` | Smaller input |
| `max_i64(i64, i64) -> i64` | Larger input |
| `clamp_i64(value, lower, upper) -> Result[i64, str]` | Inclusive clamp; reversed bounds return an error |
| `safe_divide(i64, i64) -> Result[i64, str]` | Truncating division; zero and MIN/-1 return errors |
| `word square(i64) -> i64` | Checked multiplication; overflow is a runtime failure |

## `sequences.pf`

| Export | Behavior |
|---|---|
| `sum([i64]) -> i64` | Checked sum; empty input returns zero |
| `min_or_error([i64]) -> Result[i64, str]` | Minimum; empty input returns an error |
| `sort_in_place([i64]) -> unit` | Stable insertion sort; mutates the array |
| `binary_search([i64], i64) -> i64` | Matching index or -1; requires sorted input |

Insertion sort is O(n²) worst-case time and O(1) auxiliary storage at the source
algorithm level. Binary search is O(log n); duplicate values may return any
matching index. The interpreter/compiler's register and GC overhead is separate
from these source-level algorithm bounds. `sum` propagates checked arithmetic
failures; it does not return a recoverable Result on overflow.

## `strings.pf`

| Export | Behavior |
|---|---|
| `is_ascii_digit(i64) -> bool` | True for byte values 48 through 57 |
| `is_ascii_alpha(i64) -> bool` | True for ASCII A-Z or a-z |
| `parse_i64(str) -> Result[i64, str]` | Parse an optionally signed decimal integer |

`parse_i64` accepts leading zeros and an optional leading `+` or `-`, requires
at least one digit, and rejects whitespace, separators, trailing bytes, and
out-of-range values. Both MIN and MAX are supported. Overflow is detected before
performing an invalid arithmetic operation, so malformed/out-of-range input
returns an error rather than triggering the runtime overflow handler.

These APIs are byte/ASCII-oriented. They do not implement Unicode grapheme
segmentation, normalization, locale-sensitive classification, I/O, or networking.
