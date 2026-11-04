# Inspect an optimization without timing guesses

From the repository root, use an LLVM-enabled build:

```powershell
build/native/pikaflow.exe interpret examples/optimization_lab/main.pf
build/native/pikaflow.exe opt-report examples/optimization_lab/main.pf
build/native/pikaflow.exe dump-ir examples/optimization_lab/main.pf -O0
build/native/pikaflow.exe dump-ir examples/optimization_lab/main.pf -O2
build/native/pikaflow.exe emit-ir examples/optimization_lab/main.pf -O2
build/native/pikaflow.exe run examples/optimization_lab/main.pf -O2
```

The program prints `84`, `48` and `0`. The stack word `repeated_sum` adds the
same two register values twice. Local value numbering reuses the first
successful result, and liveness removes its discarded comparison. The first
addition still runs and can trap for an excessive input.

The structured function illustrates a boundary: locals live in mutable cells,
so its separate cell loads remain opaque to scalar value numbering. Both branch
arms also remain reachable for arbitrary function arguments. Inspect the typed
IR and counters to see which operations change before examining LLVM's separate
transformations.

`opt-report` compiles and analyzes the program without running it. Its JSON
counts describe the custom typed IR, not machine instructions, LLVM pass counts
or a speedup. It also works with an LLVM-disabled compiler. See the
[report contract](../../docs/optimization.md) and
[standalone LLVM pass](../../docs/llvm-pass-plugin.md) for the two analysis levels.
