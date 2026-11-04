"""Seeded structured-program checks against independent Python expectations.

This bounded template generator is not coverage-guided fuzzing. LLVM builds run
every case through the interpreter and O0/O2 JIT, plus O0/O2 native execution for
the first case of each template. Interpreter-only compilers remain supported.
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import math
import os
import pathlib
import random
import subprocess
import tempfile
import textwrap


@dataclasses.dataclass(frozen=True)
class Program:
    template: str
    parameters: dict
    source: str
    expected: str


def program(name, parameters, source, outputs):
    return Program(name, parameters, textwrap.dedent(source).lstrip(),
                   "".join(str(value) + "\n" for value in outputs))


def arrays(rng):
    values = [rng.randint(-12, 12) for _ in range(rng.randint(3, 7))]
    rounds, delta = rng.randint(1, 5), rng.randint(-5, 5)
    expected = values.copy()
    for _ in range(rounds):
        for index in range(len(expected)):
            expected[index] += delta if index % 2 == 0 else -delta
    return program("array_loops", dict(values=values, rounds=rounds, delta=delta), f"""
        fun main()->unit:
            let values={values}
            var round=0
            while round<{rounds}:
                for index in range(0,len(values)):
                    if index%2==0:
                        values[index]+={delta}
                    else:
                        values[index]-={delta}
                round+=1
            var total=0
            for value in values:
                total+=value
            gc()
            print(values)
            print(total)
    """, [expected, sum(expected)])


def closures(rng):
    initial, delta = rng.randint(-10, 10), rng.randint(-4, 5)
    factor, count = rng.randint(2, 3), rng.randint(3, 7)
    value, outputs = initial, []
    for index in range(count):
        value = value + delta if index % 2 == 0 else value * factor
        outputs.append(value)
    outputs.append(value)
    return program("closure_cells", dict(initial=initial, delta=delta, factor=factor, count=count), f"""
        fun main()->unit:
            var value={initial}
            let add=fun()->i64:
                value+={delta}
                return value
            let scale=fun()->i64:
                value*={factor}
                return value
            for index in range(0,{count}):
                if index%2==0:
                    print(add())
                else:
                    print(scale())
                gc()
            print(value)
    """, outputs)


def records(rng):
    initial, delta = rng.randint(-10, 10), rng.randint(-4, 5)
    factor, count = rng.randint(1, 3), rng.randint(1, 5)
    value = initial
    for _ in range(count):
        value = (value + delta) * factor
    return program("generic_record_aliases", dict(initial=initial, delta=delta, factor=factor, count=count), f"""
        record Box[T]:
            value:T
        record Pair[T]:
            left:Box[T]
            right:Box[T]
        fun selected[T](pair:Pair[T],log:[i64])->Box[T]:
            log[0]+=1
            return pair.left
        fun main()->unit:
            let box=Box(value:{initial})
            let pair=Pair(left:box,right:box)
            let log=[0]
            for index in range(0,{count}):
                selected(pair,log).value+={delta}
                pair.right.value*={factor}
            gc()
            print(pair.left.value)
            print(pair.right.value)
            print(log[0])
    """, [value, value, count])


def stacks(rng):
    first, second = rng.randint(-10, 10), rng.randint(-10, 10)
    offset, factor, bias = rng.randint(-4, 5), rng.randint(1, 4), rng.randint(-6, 6)
    count = rng.randint(1, 8)
    answer = (first + offset) ** 2 * factor + bias
    return program("stack_words_pipeline", dict(first=first, second=second, offset=offset,
                   factor=factor, bias=bias, count=count), f"""
        word exchange(i64,i64)->(i64,i64):
            swap
        word shift(i64)->i64:
            {offset} +
        fun affine(value:i64)->i64:
            return value*{factor}+{bias}
        fun main()->unit:
            let (left,right)=stack({first},{second})->(i64,i64):
                exchange
            print(left)
            print(right)
            let squared=stack({first})->i64:
                shift dup *
            squared |> affine |> print
            stack(0):
                pass
            for number in range(1,{count + 1}):
                stack:
                    number +
            let total=stack()->i64:
                pass
            print(total)
    """, [second, first, answer, sum(range(1, count + 1))])


def tasks(rng):
    values = [rng.randint(-10, 10) for _ in range(rng.randint(2, 5))]
    delta, later_delta = rng.randint(-5, 5), rng.randint(-5, 5)
    factor, replacement = rng.randint(1, 4), rng.randint(-10, 10)
    first = values.copy()
    first[0] += delta
    first[1] *= factor
    parent = values.copy()
    parent[0] = replacement
    second = parent.copy()
    second[0] += later_delta
    second[1] *= factor
    repeated = first.copy()
    repeated[0] += 100
    return program("task_snapshots_gc", dict(values=values, delta=delta, later_delta=later_delta,
                   factor=factor, replacement=replacement), f"""
        record Cell[T]:
            value:T
        fun main()->unit:
            let cell=Cell(value:{values})
            var delta={delta}
            let worker=fun()->Cell[[i64]]:
                cell.value[0]+=delta
                cell.value[1]*={factor}
                gc()
                return cell
            let first=spawn(worker)
            cell.value[0]={replacement}
            delta={later_delta}
            let second=spawn(worker)
            let left=join(first)
            let right=join(second)
            gc()
            print(left.value)
            print(right.value)
            print(cell.value)
            left.value[0]+=100
            gc()
            print(join(first).value)
            print(right.value)
            print(cell.value)
    """, [first, second, parent, repeated, second, parent])


GENERATORS = (arrays, closures, records, stacks, tasks)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--cases", type=int, default=10)
    parser.add_argument("--seed", type=int, default=20261105)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--artifacts", type=pathlib.Path, default=pathlib.Path("program-failures"))
    options = parser.parse_args()
    if not 1 <= options.cases <= 1000:
        parser.error("--cases must be between 1 and 1000")
    if not math.isfinite(options.timeout) or not 0 < options.timeout <= 600:
        parser.error("--timeout must be finite, greater than zero and at most 600 seconds")
    compiler = str(options.compiler.resolve())
    rng = random.Random(options.seed)
    executions = dict(interpreter=0, jit_O0=0, jit_O2=0, native_O0=0, native_O2=0)
    templates, representatives = {}, set()
    builds = 0

    def execute(command, expected, mode, case=None, index=None):
        try:
            result = subprocess.run(command, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=options.timeout,
                                    shell=False)
            actual = result.stdout.replace("\r\n", "\n")
            failure = result.returncode != 0 or (expected is not None and actual != expected)
            details = dict(returncode=result.returncode, stdout=actual, stderr=result.stderr, timeout=False)
        except (subprocess.TimeoutExpired, OSError) as error:
            def text(value):
                return value.decode("utf-8", errors="replace") if isinstance(value, bytes) else value or ""
            failure = True
            details = dict(returncode=None, stdout=text(getattr(error, "stdout", None)),
                           stderr=text(getattr(error, "stderr", None)),
                           timeout=isinstance(error, subprocess.TimeoutExpired), error=str(error))
        if failure:
            options.artifacts.mkdir(parents=True, exist_ok=True)
            directory = pathlib.Path(tempfile.mkdtemp(prefix=f"seed{options.seed}-case{index}-{mode}-",
                                                     dir=options.artifacts.resolve()))
            if case is not None:
                (directory / "program.pf").write_text(case.source, encoding="utf-8")
            replay = [command]
            if case is not None:
                saved_source = str(directory / "program.pf")
                if mode == "interpreter":
                    replay = [[compiler, "interpret", saved_source]]
                elif mode.startswith("jit_"):
                    replay = [[compiler, "run", saved_source, "-" + mode.split("_", 1)[1]]]
                else:
                    output = str(directory / ("reproduce.exe" if os.name == "nt" else "reproduce"))
                    replay = [[compiler, "build", saved_source, "-" + mode.split("_", 1)[1], "-o", output]]
                    if mode.startswith("native_"):
                        replay.append([output])
            report = dict(seed=options.seed, case=index, mode=mode, command=command,
                          expected=expected, timeout_seconds=options.timeout,
                          reproduce_commands=replay,
                          template=case.template if case else None,
                          parameters=case.parameters if case else {}, **details)
            (directory / "failure.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            raise AssertionError(f"{mode} failed (seed={options.seed}, case={index}); artifacts: {directory}")
        return result

    version = execute([compiler, "--version"], None, "version")
    llvm = "LLVM enabled" in version.stdout
    with tempfile.TemporaryDirectory(prefix="pikaflow generated programs ") as temporary:
        directory = pathlib.Path(temporary)
        for index in range(options.cases):
            case = GENERATORS[index % len(GENERATORS)](rng)
            case = dataclasses.replace(case, source=f"# seed={options.seed} case={index} template={case.template}\n" + case.source)
            templates[case.template] = templates.get(case.template, 0) + 1
            source = directory / f"case {index} {case.template}.pf"
            source.write_text(case.source, encoding="utf-8")
            execute([compiler, "interpret", str(source)], case.expected, "interpreter", case, index)
            executions["interpreter"] += 1
            if llvm:
                for optimization in ("O0", "O2"):
                    execute([compiler, "run", str(source), "-" + optimization], case.expected,
                            "jit_" + optimization, case, index)
                    executions["jit_" + optimization] += 1
                    if case.template not in representatives:
                        output = directory / (f"case {index} {optimization}" + (".exe" if os.name == "nt" else ""))
                        execute([compiler, "build", str(source), "-" + optimization, "-o", str(output)],
                                None, "build_" + optimization, case, index)
                        builds += 1
                        execute([str(output)], case.expected, "native_" + optimization, case, index)
                        executions["native_" + optimization] += 1
                representatives.add(case.template)
    print(json.dumps(dict(seed=options.seed, programs=options.cases, templates=templates,
                          executions=executions, native_builds=builds), sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        raise SystemExit(str(error))
