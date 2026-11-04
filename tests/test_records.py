"""Nominal records, source field identity and cross-mode mutation semantics."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import textwrap


CASES = {
    "written_order_single_base": ("""
        record Pair:
            x:i64
            y:i64
        fun mark(log:[i64],value:i64)->i64:
            log[0]=log[0]*10+value
            return value
        fun selected(log:[i64],pair:Pair)->Pair:
            mark(log,3)
            return pair
        fun main():
            let log=[0]
            let pair=Pair(y:mark(log,2),x:mark(log,1))
            print(log[0])
            selected(log,pair).x += mark(log,4)
            print(log[0])
            print(pair.x)
            print(pair.y)
    """, "21\n2134\n5\n2\n"),
    "generic_fields": ("""
        record Cell[T]:
            value:T
        fun identity[T](value:T)->T:
            return value
        fun replace[T](cell:Cell[T],value:T)->T:
            let previous=identity(cell).value
            cell.value=value
            return previous
        fun wrap[T](value:T)->T:
            let cell=Cell(value:value)
            return cell.value
        fun main():
            let cell=Cell(value:20)
            print(replace(cell,42))
            print(cell.value)
            print(wrap(true))
            let arrays:Cell[[i64]]=Cell(value:[])
            arrays.value=[20,22]
            gc()
            print(arrays.value)
    """, "20\n42\ntrue\n[20, 22]\n"),
    "recursive_cycle": ("""
        record Node[T]:
            value:T
            children:[Node[T]]
        fun count[T](node:Node[T])->i64:
            var result=1
            for child in node.children:
                result+=count(child)
            return result
        fun main():
            let leaf=Node(value:42,children:[])
            let root=Node(value:20,children:[leaf])
            print(count(root))
            root.children=[root]
            gc()
            print(root.children[0].value)
    """, "2\n20\n"),
    "nested_fields_array": ("""
        record Pair:
            x:i64
            y:i64
        record Box:
            item:Pair
        fun main():
            let pair=Pair(x:20,y:22)
            let boxes=[Box(item:pair)]
            boxes[0].item.x*=2
            boxes[0].item.y-=20
            gc()
            print(pair.x+pair.y)
    """, "42\n"),
    "patterns": ("""
        record Pair[T]:
            x:T
            y:T
        fun score(pair:Pair[i64])->i64:
            match pair:
                case Pair(y:right,x:left) if left>0:
                    return left+right
                case Pair(left,right):
                    return right-left
        fun main():
            print(score(Pair(y:22,x:20)))
            print(score(Pair::[i64](x:-20,y:22)))
    """, "42\n42\n"),
    "callable_field_pipeline": ("""
        record Operation:
            apply:fn(i64)->i64
        fun double(value:i64)->i64:
            return value*2
        fun main():
            let operation=Operation(apply:double)
            print(operation.apply(21))
            print(21 |> operation.apply)
    """, "42\n42\n"),
    "task_isolation": ("""
        record Cell[T]:
            value:T
        fun main():
            let cell=Cell(value:[20,22])
            let worker=fun()->Cell[[i64]]:
                cell.value[0]=42
                gc()
                return cell
            let job=spawn(worker)
            let result=join(job)
            print(result.value)
            print(cell.value)
    """, "[42, 22]\n[20, 22]\n"),
    "empty_record_underscore": ("""
        record Empty:
            pass
        record _:
            _:i64
        fun main():
            let empty=Empty()
            match empty:
                case Empty():
                    print(42)
            let value=_(_:42)
            print(value._)
            match value:
                case _(_:result):
                    print(result)
    """, "42\n42\n42\n"),
}

PAIR="record Pair:\n    x:i64\n    y:i64\n"
NEGATIVE = {
    "duplicate_field": ("record Pair:\n    x:i64\n    x:i64\nfun main():\n    pass\n", "duplicate record field"),
    "missing_field": (PAIR+"fun main():\n    Pair(x:1)\n", "constructor arguments"),
    "duplicate_label": (PAIR+"fun main():\n    Pair(x:1,x:2)\n", "duplicate record constructor field"),
    "unknown_label": (PAIR+"fun main():\n    Pair(x:1,z:2)\n", "has no field"),
    "positional_constructor": (PAIR+"fun main():\n    Pair(1,2)\n", "require named fields"),
    "mixed_constructor": (PAIR+"fun main():\n    Pair(x:1,2)\n", "require named fields"),
    "plain_named_call": ("fun value(x:i64)->i64:\n    return x\nfun main():\n    value(x:1)\n", "named arguments are only"),
    "unknown_member": (PAIR+"fun main():\n    let p=Pair(x:1,y:2)\n    print(p.z)\n", "has no field"),
    "member_type": (PAIR+"fun main():\n    let p=Pair(x:1,y:2)\n    p.x=true\n", "record field assignment"),
    "tuple_member": ("fun main():\n    let p=(1,2)\n    print(p.x)\n", "nominal record"),
    "representation_private": (PAIR+"fun main():\n    let p=Pair(x:1,y:2)\n    print(p[0])\n", "indexing requires"),
    "nominal_mismatch": (PAIR+"record Other:\n    x:i64\n    y:i64\nfun read(p:Pair):\n    pass\nfun main():\n    read(Other(x:1,y:2))\n", "does not belong"),
    "generic_unconstrained": ("fun read[T](value:T)->i64:\n    return value.x\nfun main():\n    pass\n", "generic field receivers"),
    "generic_nested_unconstrained": ("record Box[T]:\n    value:T\nfun read[T](box:Box[T])->i64:\n    return box.value.x\nfun main():\n    pass\n", "generic field receivers"),
    "pattern_missing": (PAIR+"fun main():\n    match Pair(x:1,y:2):\n        case Pair(x:a):\n            print(a)\n", "must name every field"),
    "pattern_duplicate": (PAIR+"fun main():\n    match Pair(x:1,y:2):\n        case Pair(x:a,x:b):\n            print(a)\n", "duplicate record pattern field"),
    "pattern_mixed": (PAIR+"fun main():\n    match Pair(x:1,y:2):\n        case Pair(a,y:b):\n            print(a)\n", "cannot mix positional and named"),
    "non_callable_field": (PAIR+"fun main():\n    let p=Pair(x:1,y:2)\n    p.x()\n", "not callable"),
    "record_c_abi": (PAIR+"extern fun native(value:Pair)->unit\nfun main():\n    pass\n", "C-compatible"),
}


def main():
    compiler=str(pathlib.Path(sys.argv[1]).resolve())
    def run(arguments, expected=None, diagnostic=None):
        result=subprocess.run(arguments,capture_output=True,text=True,encoding="utf-8",timeout=60)
        if diagnostic is not None:
            assert result.returncode != 0 and diagnostic in result.stderr,(arguments,result.returncode,result.stderr)
        else:
            assert result.returncode == 0,(arguments,result.stderr)
            if expected is not None: assert result.stdout.replace("\r\n","\n")==expected,(arguments,result.stdout,expected)
        return result
    native="LLVM enabled" in run([compiler,"--version"]).stdout
    count=0
    with tempfile.TemporaryDirectory(prefix="pikaflow records ") as temporary:
        root=pathlib.Path(temporary)
        for name,(source,expected) in CASES.items():
            path=root/(name+".pf");path.write_text(textwrap.dedent(source).lstrip(),encoding="utf-8")
            run([compiler,"interpret",str(path)],expected);count+=1
            if native:
                for optimization in ("-O0","-O2"):
                    run([compiler,"run",str(path),optimization],expected);count+=1
                    executable=root/(name+optimization+(".exe" if os.name=="nt" else ""))
                    run([compiler,"build",str(path),optimization,"-o",str(executable)])
                    run([str(executable)],expected);count+=1
        for name,(source,diagnostic) in NEGATIVE.items():
            path=root/(name+".pf");path.write_text(source,encoding="utf-8")
            commands=["check","symbols"]
            if name.startswith("generic_"): commands += ["interpret"] + (["run"] if native else [])
            for command in commands:
                run([compiler,command,str(path)],diagnostic=diagnostic);count+=1
        (root/"library.pf").write_text("export record Cell[T]:\n    value:T\nrecord Secret:\n    value:i64\nexport fun secret()->Secret:\n    return Secret(value:42)\n",encoding="utf-8")
        path=root/"module.pf";path.write_text('import "library.pf" as lib\nfun main():\n    let cell:lib.Cell[i64]=lib.Cell(value:42)\n    cell.value+=1\n    print(cell.value)\n',encoding="utf-8")
        run([compiler,"interpret",str(path)],"43\n");count+=1
        for expression in ("lib.Secret(value:42)","lib.secret().value"):
            path.write_text('import "library.pf" as lib\nfun main():\n    '+expression+'\n',encoding="utf-8")
            run([compiler,"check",str(path)],diagnostic="private");count+=1
        source=root/"symbols.pf"
        source.write_text("record Cell[T]:\n    value:T\nfun get[T](cell:Cell[T])->T:\n    return cell.value\nfun main():\n    let cell:Cell[i64]=Cell(value:42)\n    cell.value+=1\n    match cell:\n        case Cell(value:result):\n            print(result)\n",encoding="utf-8")
        records=json.loads(run([compiler,"symbols",str(source)]).stdout)["symbols"]
        record=next(symbol for symbol in records if symbol["kind"]=="record")
        field=next(symbol for symbol in records if symbol["kind"]=="field")
        assert record["generic"] is True and record["name"]=="Cell" and {location["line"] for location in record["uses"]}=={3,6,9},record
        assert field["name"]=="value" and {location["line"] for location in field["uses"]}=={4,6,7,9},field
        count+=1
        # The metadata must preserve execution after an independent source edit.
        def renamed(symbol, replacement):
            original=source.read_text(encoding="utf-8")
            lines=original.splitlines(keepends=True)
            edits=[]
            for location in [symbol["definition"],*symbol["uses"]]:
                offset=sum(len(line) for line in lines[:location["line"]-1])+location["column"]-1
                assert original[offset:offset+len(symbol["name"])]==symbol["name"]
                edits.append(offset)
            for offset in sorted(set(edits),reverse=True):
                original=original[:offset]+replacement+original[offset+len(symbol["name"]):]
            target=root/(replacement+".pf");target.write_text(original,encoding="utf-8")
            run([compiler,"interpret",str(target)],"43\n")
        renamed(field,"content");count+=1
        renamed(record,"Container");count+=1
        source.write_text("record Cell:\n    value:i64\nfun read(cell:Cell)->i64:\n    return cell.value\n    print(cell.value)\nfun main():\n    print(read(Cell(value:42)))\n",encoding="utf-8")
        records=json.loads(run([compiler,"symbols",str(source)]).stdout)["symbols"]
        field=next(symbol for symbol in records if symbol["kind"]=="field")
        assert field["renameable"] and {location["line"] for location in field["uses"]}=={4,5,7},field
        run([compiler,"interpret",str(source)],"42\n");count+=1
        source.write_text("record Cell:\n    value:i64\nenum Wrapper:\n    Wrapped(cell:Cell)\nfun unused(wrapper:Wrapper):\n    return\n    match wrapper:\n        case Wrapped(cell):\n            print(cell.value)\nfun main():\n    print(Cell(value:42).value)\n",encoding="utf-8")
        run([compiler,"check",str(source)]);run([compiler,"interpret",str(source)],"42\n")
        records=json.loads(run([compiler,"symbols",str(source)]).stdout)["symbols"]
        field=next(symbol for symbol in records if symbol["kind"]=="field")
        assert field["renameable"] is False and "unresolved" in field["detail"],field
        count+=1
    print(f"{count} record checks passed ({'LLVM enabled' if native else 'interpreter'})")


if __name__=="__main__":
    main()
