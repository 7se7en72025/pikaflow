# C interoperability

Foreign declarations are explicit contracts with native code. Calls require an
`unsafe` block. A source-level alias can name a C symbol without shadowing a
language builtin:

```text
extern fun foreign_free(memory: ptr[unit]) -> unit = "free"
```

The caller must declare the real signature and respect ownership. In particular,
`i64` means C `int64_t`, not C `int` or Windows `long`. The current language
supports explicit 8/16/32/64-bit integer annotations as described below.

## Supported boundary types

| PikaFlow type | C representation |
| --- | --- |
| `i64` | `int64_t` |
| `f64` | `double` |
| `bool` | `_Bool` / `bool`, including ABI extension attributes |
| `unit` return | `void` |
| `ptr[T]` | Raw native pointer; no ownership transfer is inferred |
| Plain tuple | Naturally aligned C struct with fields in tuple order |
| `c_i8`, `c_i16`, `c_i32`, `c_i64` | Explicit signed C integer width, carried as source `i64` |
| `c_u8`, `c_u16`, `c_u32`, `c_u64` | Explicit unsigned C integer width, carried as source `i64` |

Plain C records may contain `i64`, `f64`, `bool`, raw pointers, and nested plain
records. Their memory
layout uses the target data layout, with one-byte C boolean fields. The compiler
copies fields between the managed PikaFlow tuple and a native temporary. It does
not pass a managed tuple pointer as though it were a C struct.

Nominal language ADTs are managed values and cannot be used as C records. Unions,
packed records, bitfields, C++ objects, and other managed-reference fields are
outside this subset and must not be inferred from a tuple declaration.

## Integer widths

The `c_` integer annotations are valid only inside external declarations,
including nested record fields. Ordinary source values and arithmetic remain
`i64`. For example, C's `int abs(int)` on the supported x86-64 hosts can be called
with an explicit 32-bit signature:

```text
extern fun absolute(value: c_i32) -> c_i32 = "abs"

fun main() -> unit:
    unsafe:
        print(absolute(-42))
```

Inputs to 8-, 16-, and 32-bit parameters are checked before the C call, including
record fields. An out-of-range value reports a runtime error at the source call;
it does not wrap silently. Signed results are sign-extended to source `i64` and
unsigned narrow results are zero-extended. `c_u64` preserves all 64 bits in its
signed source carrier: `-1` encodes C's `UINT64_MAX`, and a returned `UINT64_MAX`
appears as source `-1`. This supplies an ABI contract rather than unsigned source
arithmetic.

A record such as `(c_i8, c_u16, c_i32)` is still passed and returned as an ordinary
source tuple of `i64` values. Its native fields use widths 1, 2, and 4 bytes,
with the C compiler's natural padding and alignment.

Annotations describe fixed parameters and the result. Variadic extra arguments
retain their ordinary source ABI: `i64` is `int64_t`, `f64` is `double`, and
`bool` promotes to C `int`. A format string does not change those widths.
Narrow annotated extern calls require native or JIT execution; the interpreter
rejects them explicitly. Compatible 64-bit annotations remain usable with its
existing audited scalar externs.

## Example

```c
#include <stdint.h>
typedef struct Pair { int64_t a, b; } Pair;
#ifdef _WIN32
__declspec(dllexport)
#endif
Pair pair_shift(Pair value, int64_t delta) {
    Pair result;
    result.a = value.a + delta;
    result.b = value.b + delta;
    return result;
}
```

```text
extern fun shift(value: (i64, i64), delta: i64) -> (i64, i64) = "pair_shift"

fun main() -> unit:
    unsafe:
        let value = shift((10, 20), 6)
        print(value[0] + value[1])
```

For Windows, build the C library in a Developer PowerShell:

```powershell
cl /nologo /LD pair.c /link /OUT:pair.dll /IMPLIB:pair.lib
./build/native/pikaflow.exe run demo.pf --load ./pair.dll -O2
./build/native/pikaflow.exe build demo.pf --link ./pair.lib -o demo.exe
./demo.exe
```

The DLL must be discoverable when the native executable runs, such as beside the
executable. `--load` permanently loads a library into the JIT process. `--link`
passes a library to the native linker; it does not copy runtime dependencies.

For Linux, build `libpair.so` with `cc -shared -fPIC pair.c -o libpair.so`, pass its
path to `--load` or `--link`, and make it discoverable to the executable using the
loader's normal search path, such as `LD_LIBRARY_PATH`.

## Native output files

The executable output must not alias loaded source or project metadata. With
MSVC, the backend passes an explicit linker output path, disables incremental
`.ilk` generation and writes temporary export `.lib`/`.exp` artifacts in an
isolated directory. Debug builds place their PDB at `OUTPUT.pdb`; the CLI also
checks that path against protected inputs before linking. Tests preserve
adjacent sentinel files and source/metadata aliases, including failed links.
This does not copy external DLL dependencies or remove the host SDK requirement.

## ABI lowering

Windows x64 follows the ordinary C record rules: records of 1, 2, 4, or 8 bytes
are coerced to integers; other record arguments use aligned caller temporaries.
Large record returns use a hidden `sret` pointer. This subset follows Clang's
`WinX86_64ABIInfo` classification, not the SysV classifier.

Linux x86-64 uses LLVM 24's `llvm::abi::FunctionInfo`, X86-64 target classifier,
and `IRTypeMapper`. Classification covers the complete call signature so
integer/SSE register exhaustion and aggregate stack fallback are considered
together. The LLVM `abi` component is required for this build.

The source implementations are in `clang/lib/CodeGen/Targets/X86.cpp` and
`llvm/lib/ABI/Targets/X86.cpp` of the pinned LLVM checkout.

Nested records retain their own natural field offsets and alignment. Returning
a record creates managed tuples recursively, with temporary GC roots while
child tuples are allocated. The SysV classifier sees the complete recursive
record rather than a flattened list of source values.

## C callbacks

`c_callback(name)` creates a native C function pointer for a module-level source
function. The operation requires `unsafe`. Parameters may be `i64`, `f64`,
`bool`, or raw pointers, and the result may additionally be `unit`. For example:

```text
extern fun invoke(callback: ptr[unit]) -> i64 = "invoke_callback"

fun add(a: i64, b: i64) -> i64:
    return a + b

fun main() -> unit:
    unsafe:
        print(invoke(c_callback(add)))
```

The matching C declaration is
`int64_t invoke_callback(int64_t (*callback)(int64_t, int64_t))`.
The compiler generates a C ABI wrapper that supplies the source function's
hidden environment argument. Boolean parameters and results retain the required
ABI extension attributes. Captured closures and managed/record callback
signatures are rejected. A raw pointer does not encode the callback signature;
the caller must match the C declaration.
Callback source functions retain their ordinary scalar widths; extern-only
`c_` annotations do not change a callback's signature.

The callback's code must remain loaded until C has finished using it. A native
executable retains its generated functions for the process lifetime; a CLI JIT
run unloads its module when that run returns. C must finish callback invocations
before that point. The interpreter reports that callbacks require native or
JIT execution.

## Strings and ownership

`cstr(text)` borrows a null-terminated string buffer. Keep the managed string
alive for as long as C uses it. If C retains a pointer beyond a call, use
`pin(text)` to create a persistent GC root and later release it with `unpin`.
The pin handle is not the character-data pointer. C allocations must be released
through the corresponding C allocator; they are not runtime-owned raw blocks.

## Verification

`tests/ffi_fixture.c` is compiled by the host C compiler into a shared library.
`ffi_host_tests.c` first checks the reference C calls. Backend tests and
`test_ffi.py` exercise generated calls, including scalar and mixed records,
padding, small/non-power-of-two records, large returns, raw-pointer fields,
nested records, repeated nested returns across collection, scalar callbacks,
all explicit integer widths, narrow nested record layouts, checked input range
failures with source locations, register pressure, variadic promotions, and real libc/math aliases. Generated
execution is checked separately for JIT/native code and `-O0`/`-O2`.

Passing on one host establishes evidence for that host ABI only. Linux and
Windows results must be recorded separately; emitting IR alone does not verify
cross-platform ABI correctness.
