# C interoperability

Foreign declarations are explicit contracts with native code. Calls require an
`unsafe` block. A source-level alias can name a C symbol without shadowing a
language builtin:

```text
extern fun foreign_free(memory: ptr[unit]) -> unit = "free"
```

The caller must declare the real signature and respect ownership. In particular,
`i64` means C `int64_t`, not C `int` or Windows `long`. The current language does
not expose every C integer width.

## Supported boundary types

| PikaFlow type | C representation |
| --- | --- |
| `i64` | `int64_t` |
| `f64` | `double` |
| `bool` | `_Bool` / `bool`, including ABI extension attributes |
| `unit` return | `void` |
| `ptr[T]` | Raw native pointer; no ownership transfer is inferred |
| Flat tuple | Naturally aligned C struct with fields in tuple order |

Flat C records may contain `i64`, `f64`, `bool`, and raw pointers. Their memory
layout uses the target data layout, with one-byte C boolean fields. The compiler
copies fields between the managed PikaFlow tuple and a native temporary. It does
not pass a managed tuple pointer as though it were a C struct.

Nested records, unions, packed records, bitfields, C++ objects, managed-reference
fields, and arbitrary callback signatures are outside this subset and must not
be inferred from a tuple declaration.

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
register pressure, variadic promotions, and real libc/math aliases. Generated
execution is checked separately for JIT/native code and `-O0`/`-O2`.

Passing on one host establishes evidence for that host ABI only. Linux and
Windows results must be recorded separately; emitting IR alone does not verify
cross-platform ABI correctness.
