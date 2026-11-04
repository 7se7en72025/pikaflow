import subprocess
import sys

cases = {
    "outside_scope": "active structured task scope",
    "null_entry": "no worker entry point",
    "pointer_result": "result type cannot contain",
    "pointer_capture": "snapshots cannot contain",
    "task_capture": "snapshots cannot contain",
    "nested_pointer_result": "snapshots cannot contain",
    "worker_panic": "worker failed",
    "limit": "64 outstanding tasks",
    "scope_order": "reverse order",
    "invalid_join": "incompatible type",
}
for case, expected in cases.items():
    result = subprocess.run([sys.argv[1], case], capture_output=True, text=True, timeout=10)
    assert result.returncode == 1, (case, result.returncode, result.stderr)
    assert expected in result.stderr, (case, result.stderr)
print(f"{len(cases)} structured concurrency failure cases passed")
