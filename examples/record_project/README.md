# Records, aliases, isolated tasks and project tests

From the repository root, run:

```powershell
python tools/pikaflow.py --compiler build/native/pikaflow.exe run --project examples/record_project -O2
python tools/pikaflow.py --compiler build/native/pikaflow.exe test --project examples/record_project --backend native -O2 --jobs 2
```

The program prints `42`, `43`, `52`, `36`. The task receives its own record graph:
its increment cannot change the parent's later value. The three tests cover
generic fields, alias annotations and snapshot isolation. Each test runs in a
separate process and does not call the application entry point.

Use `--backend interpret` without LLVM or `--backend jit` for ORC execution.
Compiler builds, assertions and report formats are described in
[project testing](../../docs/testing.md).
