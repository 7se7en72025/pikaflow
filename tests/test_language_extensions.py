"""Source-language extensions checked through interpreter and available LLVM modes."""
import os
import pathlib
import subprocess
import sys
import tempfile
import textwrap


CASES = {
    "generic_higher_order": ('''
        fun apply[A, B](value: A, operation: fn(A) -> B) -> B:
            return operation(value)
        fun double(value: i64) -> i64:
            return value * 2
        fun empty[T]() -> [T]:
            return []
        fun main() -> unit:
            print(apply(21, double))
            let values: [i64] = empty()
            print(len(values))
    ''', "42\n0\n"),
    "generic_stack_word": ('''
        word duplicate[T](T) -> (T, T):
            dup
        fun main() -> unit:
            let values = stack(21) -> (i64, i64):
                duplicate
            print(values[0] + values[1])
    ''', "42\n"),
    "enum_recursive_list": ('''
        enum List[T]:
            Cons(value: T, rest: List[T])
            Empty
        fun sum(values: List[i64]) -> i64:
            match values:
                case Empty():
                    return 0
                case Cons(value, rest):
                    return value + sum(rest)
        fun main() -> unit:
            let values = Cons(20, Cons(22, Empty::[i64]()))
            gc()
            print(sum(values))
    ''', "42\n"),
    "enum_mutual_recursion": ('''
        enum First[T]:
            Finish(value: T)
            ToSecond(other: Second[T])
        enum Second[T]:
            ToFirst(other: First[T])
        fun score(value: First[i64]) -> i64:
            match value:
                case Finish(result):
                    return result
                case ToSecond(ToFirst(other)):
                    return score(other)
        fun main() -> unit:
            print(score(ToSecond(ToFirst(Finish(42)))))
    ''', "42\n"),
    "enum_recursive_gc_cycle": ('''
        enum Node:
            Link(children: [Node])
        fun main() -> unit:
            let children: [Node] = [Link([])]
            let root = Link(children)
            children[0] = root
            gc()
            match root:
                case Link(values):
                    print(len(values) + 41)
    ''', "42\n"),
    "enum_nested_exhaustiveness": ('''
        enum Decision[T]:
            Present(flag: bool, value: T)
            Absent
        fun score(value: Decision[i64]) -> i64:
            match value:
                case Present(true, value):
                    return value
                case Present(false, _):
                    return 0
                case Absent():
                    return -1
        fun main() -> unit:
            print(score(Present(true, 42)))
            print(score(Present(false, 42)))
            print(score(Absent::[i64]()))
    ''', "42\n0\n-1\n"),
    "enum_plain": ('''
        enum Color:
            Red
            RGB(red: i64, green: i64, blue: i64)
        fun score(color: Color) -> i64:
            match color:
                case Red():
                    return 0
                case RGB(red, green, blue):
                    return red + green + blue
        fun main() -> unit:
            print(score(Red()))
            print(score(RGB(20, 20, 2)))
    ''', "0\n42\n"),
    "enum_generic": ('''
        enum Option[T]:
            Some(value: T)
            None
        fun value_or[T](value: Option[T], fallback: T) -> T:
            match value:
                case Some(result):
                    return result
                case None():
                    return fallback
        fun main() -> unit:
            print(value_or(Some(42), 0))
            let missing: Option[i64] = None()
            print(value_or(missing, 42))
            print(value_or(None::[str](), "fallback"))
            gc()
            print(value_or(Some([20, 22]), [0])[1])
    ''', "42\n42\nfallback\n22\n"),
    "enum_nested": ('''
        enum Box[T]:
            Boxed(value: T)
        enum Outer[T]:
            Wrapped(value: Box[T])
            Missing
        fun score(value: Outer[i64]) -> i64:
            match value:
                case Wrapped(inner):
                    match inner:
                        case Boxed(result):
                            return result
                case Missing():
                    return 0
        fun main() -> unit:
            print(score(Wrapped(Boxed(42))))
            print(score(Missing::[i64]()))
    ''', "42\n0\n"),
    "enum_result": ('''
        enum Option[T]:
            Some(value: T)
            None
        fun read() -> Result[Option[i64], str]:
            return ok(Some(42))
        fun main() -> unit:
            match unwrap(read()):
                case Some(value) if value < 0:
                    print(-1)
                case Some(value):
                    print(value)
                case None():
                    print(0)
    ''', "42\n"),
    "generic_values": ('''
        fun identity[T](value: T) -> T:
            return value
        fun main() -> unit:
            print(identity(42))
            print(identity(2.5))
            print(identity("hello"))
            print(identity([20, 22])[1])
            let explicit = identity::[i64]
            print(explicit(42))
    ''', "42\n2.5\nhello\n22\n42\n"),
    "generic_nested_types": ('''
        fun count[T](values: [T]) -> i64:
            return len(values)
        fun first[T](values: [T]) -> T:
            return values[0]
        fun main() -> unit:
            print(count::[i64]([]))
            print(first([(20, 22)])[1])
            let values: [i64] = []
            print(count(values))
    ''', "0\n22\n0\n"),
    "generic_closure": ('''
        fun constant[T](value: T) -> fn() -> T:
            fun inner() -> T:
                return value
            return inner
        fun main() -> unit:
            let answer = constant(42)
            gc()
            print(answer())
            print(constant("captured")())
    ''', "42\ncaptured\n"),
    "generic_recursion": ('''
        fun last[T](values: [T], index: i64) -> T:
            if index + 1 == len(values):
                return values[index]
            return last(values, index + 1)
        fun main() -> unit:
            print(last([1, 20, 42], 0))
            print(last(["first", "last"], 0))
    ''', "42\nlast\n"),
    "generic_word": ('''
        word duplicate[T](T) -> (T, T):
            dup
        fun main() -> unit:
            let result = duplicate::[i64](21)
            print(result[0] + result[1])
    ''', "42\n"),
    "propagation": ('''
        fun source(good: bool) -> Result[i64, str]:
            if good:
                return ok(41)
            return err("stopped")
        fun increment(good: bool) -> Result[i64, str]:
            let value = source(good)?
            return ok(value + 1)
        fun main() -> unit:
            print(unwrap(increment(true)))
            print(error(increment(false)))
    ''', "42\nstopped\n"),
    "propagation_changes_success_type": ('''
        fun read() -> Result[i64, str]:
            return err("no value")
        fun describe() -> Result[str, str]:
            let value = read()?
            print(value)
            return ok("unreachable")
        fun main() -> unit:
            print(error(describe()))
    ''', "no value\n"),
    "guards": ('''
        fun describe(value: (i64, i64)) -> i64:
            match value:
                case (left, right) if left > right:
                    return 1
                case _ if false:
                    return 0
                case (left, right):
                    return left + right
        fun main() -> unit:
            print(describe((20, 22)))
            print(describe((22, 20)))
    ''', "42\n1\n"),
    "guard_effects": ('''
        fun main() -> unit:
            var count = 0
            fun predicate(value: i64) -> bool:
                count = count + 1
                return value > 5
            match 42:
                case 0 if predicate(42):
                    print(0)
                case value if predicate(value):
                    print(value)
                case _:
                    print(-1)
            print(count)
    ''', "42\n1\n"),
}

NEGATIVE = {
    "abi_width_conflict": ('extern fun one(value:c_i8)->c_i8 = "same_symbol"\nextern fun two(value:c_i16)->c_i16 = "same_symbol"\nfun main():\n    pass\n', "conflicting declarations"),
    "abi_sign_conflict": ('extern fun one(value:c_i8)->c_i8 = "same_symbol"\nextern fun two(value:c_u8)->c_u8 = "same_symbol"\nfun main():\n    pass\n', "conflicting declarations"),
    "abi_source_carrier": ('fun main():\n    let value:c_i8=1\n', "unknown type"),
    "abi_pointer_width": ('extern fun one(value:ptr[c_i8])->unit\nfun main():\n    pass\n', "unknown type"),
    "task_recursive_pointer": ("enum Node:\n    Link(children:[Node])\n    Foreign(memory:ptr[i64])\nfun worker()->Node:\n    return Link([])\nfun main():\n    spawn(worker)\n", "result cannot contain"),
    "enum_exhaustiveness": ("enum Option[T]:\n    Some(value:T)\n    None\nfun main():\n    match Some(42):\n        case Some(x):\n            print(x)\n", "non-exhaustive enum match"),
    "enum_guard_exhaustiveness": ("enum Flag:\n    Yes\n    No\nfun main():\n    match Yes():\n        case Yes() if true:\n            pass\n        case No():\n            pass\n", "non-exhaustive enum match"),
    "enum_payload_exhaustiveness": ("enum Flag:\n    Value(flag:bool)\nfun main():\n    match Value(true):\n        case Value(true):\n            pass\n", "non-exhaustive enum match"),
    "enum_nominal": ("enum A:\n    First(x:i64)\nenum B:\n    Second(x:i64)\nfun f(x:A):\n    pass\nfun main():\n    f(Second(42))\n", "constructor does not belong"),
    "enum_constructor_arity": ("enum Option[T]:\n    Some(value:T)\n    None\nfun main():\n    Some()\n", "constructor arguments"),
    "enum_nonregular_recursion": ("enum Grow[T]:\n    More(value:Grow[[T]])\n    Empty\nfun main():\n    let x:Grow[i64]=Empty()\n", "limit"),
    "enum_representation_private": ("enum Color:\n    Red\nfun main():\n    print(Red()[0])\n", "indexing requires"),
    "generic_conflict": ("fun same[T](a:T,b:T)->T:\n    return a\nfun main():\n    same(1,true)\n", "type parameter"),
    "generic_uninferred": ("fun empty[T]()->[T]:\n    return []\nfun main():\n    empty()\n", "cannot infer type parameter"),
    "generic_arity": ("fun id[T](x:T)->T:\n    return x\nfun main():\n    id::[i64,bool](1)\n", "type arguments"),
    "generic_not_generic": ("fun id(x:i64)->i64:\n    return x\nfun main():\n    id::[i64](1)\n", "generic function"),
    "generic_body_type": ("fun add[T](x:T)->T:\n    return x+1\nfun main():\n    add(true)\n", "requires bool"),
    "generic_depth": ("fun grow[T](x:T)->i64:\n    return grow([x])\nfun main():\n    grow(1)\n", "limit"),
    "generic_exponential_growth": ("fun grow[T](x:T)->i64:\n    return grow((x,x))\nfun main():\n    grow(1)\n", "type complexity limit"),
    "generic_nominal_name_growth": ("enum Pair[A,B]:\n    PairValue(first:A, second:B)\nfun grow[T](x:T)->i64:\n    return grow(PairValue(x,x))\nfun main():\n    grow(1)\n", "nominal type name size limit"),
    "enum_exponential_growth": ("enum Grow[T]:\n    More(value:Grow[(T,T)])\n    Empty\nfun main():\n    let x:Grow[i64]=Empty()\n", "type complexity limit"),
    "guard_type": ("fun main():\n    match 42:\n        case x if x:\n            print(x)\n", "match guard requires bool"),
    "guard_exhaustiveness": ("fun f(x:bool)->i64:\n    match x:\n        case true if false:\n            return 1\n        case false:\n            return 2\nfun main():\n    pass\n", "does not return"),
    "guard_binding_scope": ("fun main():\n    match 1:\n        case x if true:\n            print(x)\n    print(x)\n", "unknown name"),
    "propagate_nonresult": ("fun main():\n    let x = 42?\n", "Result propagation"),
    "propagate_error_type": ("fun f()->Result[i64,i64]:\n    let x:Result[i64,str]=err(\"x\")\n    return ok(x?)\nfun main():\n    pass\n", "propagated error"),
}


def main():
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    def run(arguments, expected=None, diagnostic=None):
        result = subprocess.run(arguments, text=True, encoding="utf-8", capture_output=True, timeout=60)
        if diagnostic is not None:
            assert result.returncode != 0 and diagnostic in result.stderr, (arguments, result.stderr)
        else:
            assert result.returncode == 0, (arguments, result.stderr)
            if expected is not None:
                assert result.stdout.replace("\r\n", "\n") == expected, (arguments, result.stdout, expected)
        return result
    native = "LLVM enabled" in run([compiler, "--version"]).stdout
    count = 0
    with tempfile.TemporaryDirectory(prefix="pikaflow language ") as temporary:
        root = pathlib.Path(temporary)
        for name, (source, expected) in CASES.items():
            path = root / (name + ".pf")
            path.write_text(textwrap.dedent(source).lstrip(), encoding="utf-8")
            run([compiler, "interpret", str(path)], expected); count += 1
            if native:
                for optimization in ("-O0", "-O2"):
                    run([compiler, "run", str(path), optimization], expected); count += 1
                    executable = root / (name + optimization + (".exe" if os.name == "nt" else ""))
                    run([compiler, "build", str(path), optimization, "-o", str(executable)])
                    run([str(executable)], expected); count += 1
        for name, (source, diagnostic) in NEGATIVE.items():
            path = root / (name + ".pf"); path.write_text(source, encoding="utf-8")
            run([compiler, "check", str(path)], diagnostic=diagnostic); count += 1
        (root / "library.pf").write_text("export fun identity[T](x:T)->T:\n    return helper(x)\nfun helper[T](x:T)->T:\n    return x\n", encoding="utf-8")
        path = root / "module.pf"
        path.write_text('import "library.pf"\nfun main():\n    print(identity(42))\n', encoding="utf-8")
        run([compiler, "interpret", str(path)], "42\n"); count += 1
        path.write_text('import "library.pf"\nfun main():\n    helper(42)\n', encoding="utf-8")
        run([compiler, "check", str(path)], diagnostic="unknown function"); count += 1
        (root / "types.pf").write_text("export enum Box[T]:\n    Boxed(value:T)\nexport fun unwrap_box[T](box:Box[T])->T:\n    match box:\n        case Boxed(value):\n            return value\n", encoding="utf-8")
        path.write_text('import "types.pf"\nfun main():\n    let value:Box[i64]=Boxed(42)\n    print(unwrap_box(value))\n', encoding="utf-8")
        run([compiler, "interpret", str(path)], "42\n"); count += 1
        (root / "private.pf").write_text("enum Secret:\n    Hidden(value:i64)\nexport fun answer()->i64:\n    return 42\n", encoding="utf-8")
        path.write_text('import "private.pf"\nfun main():\n    Hidden(42)\n', encoding="utf-8")
        run([compiler, "check", str(path)], diagnostic="unknown function"); count += 1
        for name, source in {
            "abi_widths": 'extern fun narrow(value:c_i8, nested:(c_u16,(c_i32,c_u8)))->c_u32\nfun main():\n    unsafe:\n        narrow(42,(42,(42,42)))\n',
            "abi_alias_64": 'extern fun one(value:i64)->i64 = "same_symbol"\nextern fun two(value:c_u64)->c_i64 = "same_symbol"\nfun main():\n    pass\n',
            "abi_nested_alias_64": 'extern fun one(value:(i64,(i64,bool)))->i64 = "same_symbol"\nextern fun two(value:(c_u64,(c_i64,bool)))->c_u64 = "same_symbol"\nfun main():\n    pass\n',
        }.items():
            path = root / (name + ".pf"); path.write_text(source, encoding="utf-8")
            run([compiler, "check", str(path)]); count += 1
        showcase = pathlib.Path(__file__).resolve().parents[1] / "examples/showcase/main.pf"
        expected = "43\n36\nnegative seed\n"
        run([compiler, "interpret", str(showcase)], expected); count += 1
        if native:
            for optimization in ("-O0", "-O2"):
                run([compiler, "run", str(showcase), optimization], expected); count += 1
                executable = root / ("showcase" + optimization + (".exe" if os.name == "nt" else ""))
                run([compiler, "build", str(showcase), optimization, "-o", str(executable)])
                run([str(executable)], expected); count += 1
    print(f"{count} language extension checks passed ({'LLVM enabled' if native else 'interpreter'})")


if __name__ == "__main__":
    main()
