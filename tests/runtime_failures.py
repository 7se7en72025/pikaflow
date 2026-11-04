"""Check fatal runtime errors in subprocesses, including their diagnostics."""
import pathlib
import subprocess
import sys


def main():
    executable = pathlib.Path(sys.argv[1]).resolve()
    cases = {
        "add-overflow": "addition overflow",
        "sub-overflow": "subtraction overflow",
        "mul-overflow": "multiplication overflow",
        "div-zero": "division by zero",
        "div-overflow": "division overflow",
        "mod-overflow": "remainder overflow",
        "neg-overflow": "negation overflow",
        "float-overflow": "outside i64 range",
        "float-nan": "outside i64 range",
        "c-int-signed-high": "declared signed width",
        "c-int-signed-low": "declared signed width",
        "c-int-unsigned-high": "declared unsigned width",
        "c-int-unsigned-negative": "declared unsigned width",
        "c-int-width": "invalid C integer width",
        "c-int-signedness": "invalid C integer width",
        "bounds": "out of bounds",
        "negative-index": "out of bounds",
        "type-mismatch": "incompatible type",
        "negative-size": "negative allocation size",
        "invalid-reference": "invalid or expired",
        "root-order": "reverse order",
        "panic": "explicit panic",
        "result-failure": "operation failed",
        "raw-bounds": "out of bounds",
        "raw-interior-bounds": "out of bounds",
        "raw-double-free": "already been freed",
        "raw-type": "element type mismatch",
        "raw-negative": "negative raw allocation size",
        "raw-size-overflow": "size is too large",
        "raw-null": "null raw pointer",
        "raw-boolean": "invalid boolean representation",
        "raw-offset-overflow": "offset overflow",
        "raw-managed-type": "only i64, f64, bool, or native pointers",
        "offset-owned-bounds": "out of bounds",
        "offset-before-owned": "out of bounds",
        "offset-onepast-load": "out of bounds",
        "offset-null": "null raw pointer",
        "offset-underflow": "offset overflow",
        "offset-overflow": "offset overflow",
        "offset-minimum": "offset overflow",
        "invalid-pin": "invalid or released persistent root",
        "released-pin": "invalid or released persistent root",
        "result-nonstring": "cannot unwrap an error Result",
        "result-success-error": "cannot extract an error from a successful Result",
        "source-location": "example.pf:12:7: runtime error:",
        "source-location-copy": "copied.pf:8:3: runtime error:",
        "source-location-explicit": "new.pf:5:1: runtime error:",
    }
    for case, expected in cases.items():
        result = subprocess.run(
            [str(executable), case], capture_output=True, text=True, timeout=10
        )
        if result.returncode != 1 or "runtime error:" not in result.stderr or expected not in result.stderr:
            raise AssertionError(
                f"{case}: exit={result.returncode}\nstdout={result.stdout}\nstderr={result.stderr}"
            )
    print(f"{len(cases)} runtime failure cases passed")


if __name__ == "__main__":
    main()
