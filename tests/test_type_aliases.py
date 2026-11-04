"""Transparent aliases, bounded expansion, source identity and C wire metadata.

Arguments: compiler [C fixture shared library, fixture link/import library].
"""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import textwrap


CASES={
    "underscore_alias_value": ("""
        type _[Element]=[Element]
        fun _()->i64:
            return 42
        fun main():
            let values:_[i64]=[_()]
            print(values[0])
    ""","42\n"),
    "alias_parameter_shadow": ("""
        type T=c_i8
        type Identity[T]=T
        fun main():
            let value:Identity[bool]=true
            print(value)
    ""","true\n"),
    "array_generic": ("""
        export type Vec[T]=[T]
        type Numbers=Vec[i64]
        fun first[T](values:Vec[T])->T:
            return values[0]
        fun main():
            let values:Numbers=[20,22]
            values[0]+=22
            print(first(values))
    ""","42\n"),
    "record_receiver": ("""
        type IntPoint=Point[i64]
        type PointRef[T]=Point[T]
        record Point[T]:
            x:T
            y:T
        fun replace[T](point:PointRef[T],value:T)->T:
            let previous=point.x
            point.x=value
            return previous
        fun main():
            let point:IntPoint=Point(y:22,x:20)
            print(replace(point,42))
            print(point.x)
    ""","20\n42\n"),
    "recursive_nominal_boundary": ("""
        type Children[T]=[Node[T]]
        record Node[T]:
            value:T
            children:Children[T]
        fun count[T](node:Node[T])->i64:
            var total=1
            for child in node.children:
                total+=count(child)
            return total
        fun main():
            let leaf=Node(value:42,children:[])
            let root=Node(value:20,children:[leaf])
            print(count(root))
            root.children=[root]
            gc()
            print(root.children[0].value)
    ""","2\n20\n"),
    "generic_callable": ("""
        type Maker[T]=fn(T)->Cell[T]
        record Cell[T]:
            value:T
        fun make[T](value:T)->Cell[T]:
            return Cell(value:value)
        fun consume[T](maker:Maker[T],value:T)->T:
            let cell=maker(value)
            return cell.value
        fun main():
            let maker=make::[i64]
            print(consume(maker,42))
    ""","42\n"),
    "type_and_value_namespaces": ("""
        type Count=i64
        type T=Point[i64]
        record Point[T]:
            value:T
        fun Count(value:i64)->i64:
            return value+1
        fun keep[T](value:T)->T:
            return value
        fun main():
            let value:Count=Count(41)
            print(keep(value))
    ""","42\n"),
    "forward_chain_unit": ("""
        type A=B
        type B=C
        type C=i64
        type Nothing=unit
        fun noop()->Nothing:
            pass
        fun main()->Nothing:
            noop()
            let value:A=42
            print(value)
    ""","42\n"),
    "result_task_alias": ("""
        type Outcome[T]=Result[T,str]
        type Job[T]=task[T]
        fun accept(value:Outcome[i64])->Outcome[i64]:
            return ok(value?+1)
        fun main():
            let worker=fun()->Outcome[i64]:
                return accept(ok(41))
            let job:Job[Outcome[i64]]=spawn(worker)
            print(unwrap(join(job)))
    ""","42\n"),
    "nested_alias_args": ("""
        type Pair[A,B]=(A,B)
        type Number=i64
        type Vec[T]=[T]
        type Data=Pair[Vec[Number],bool]
        fun main():
            let data:Data=([20,22],true)
            print(data[0][0]+data[0][1])
            print(data[1])
    ""","42\ntrue\n"),
}

NEGATIVE={
    "self_cycle":("type Loop=Loop\nfun main():\n    pass\n","cyclic type alias"),
    "indirect_cycle":("type First=[Second]\ntype Second=(First,i64)\nfun main():\n    pass\n","cyclic type alias"),
    "generic_cycle":("type Grow[T]=Grow[[T]]\nfun main():\n    pass\n","cyclic type alias"),
    "unknown_unused":("type Missing=Unknown\nfun main():\n    pass\n","unknown type"),
    "unused_generic_wire":("type Byte=c_i8\nfun unused[T](value:Byte):\n    pass\nfun main():\n    pass\n","only valid in extern"),
    "unused_generic_arity":("type Vec[T]=[T]\nfun unused[U](value:Vec[U,U]):\n    pass\nfun main():\n    pass\n","type alias arguments"),
    "missing_arity":("type Vec[T]=[T]\nfun use(value:Vec):\n    pass\nfun main():\n    pass\n","type alias arguments"),
    "extra_arity":("type Number=i64\nfun use(value:Number[i64]):\n    pass\nfun main():\n    pass\n","type alias arguments"),
    "generic_rhs_arity":("type Vec[T]=[T]\ntype Bad=Vec[i64,bool]\nfun main():\n    pass\n","type alias arguments"),
    "nominal_rhs_arity":("record Point[T]:\n    value:T\ntype Bad=Point\nfun main():\n    pass\n","nominal type arguments"),
    "duplicate":("type Number=i64\ntype Number=f64\nfun main():\n    pass\n","duplicate or reserved type alias"),
    "nominal_collision":("type Point=i64\nrecord Point:\n    value:i64\nfun main():\n    pass\n","duplicate or reserved"),
    "reserved_legacy":("type int=i64\nfun main():\n    pass\n","reserved type alias"),
    "reserved_wire":("type c_i8=i64\nfun main():\n    pass\n","reserved type alias"),
    "nested_alias":("fun main():\n    type Local=i64\n","module scope"),
    "unit_array":("type Bad=[unit]\nfun main():\n    pass\n","unit has no value"),
    "unit_function_parameter":("type Bad=fn(unit)->i64\nfun main():\n    pass\n","unit has no value"),
    "wire_ordinary":("type Byte=c_u8\nfun use(value:Byte):\n    pass\nfun main():\n    pass\n","only valid in extern"),
    "wire_in_record":("type Byte=c_u8\nrecord Value:\n    value:Byte\nfun main():\n    pass\n","only valid in extern"),
    "wire_pointer":("type Byte=c_u8\nextern fun use(value:ptr[Byte])->unit\nfun main():\n    pass\n","wire widths require scalar or plain-tuple positions"),
    "wire_conflict":('type Byte=c_u8\nextern fun one(value:Byte)->Byte="same_symbol"\nextern fun two(value:c_i8)->c_i8="same_symbol"\nfun main():\n    pass\n',"conflicting declarations"),
    "record_alias_c_abi":("record Point:\n    value:i64\ntype Alias=Point\nextern fun native(value:Alias)->unit\nfun main():\n    pass\n","C-compatible"),
    "not_constructor":("record Point:\n    value:i64\ntype Alias=Point\nfun main():\n    Alias(value:42)\n","named arguments are only"),
    "generic_shadow_field":("type T=Point\nrecord Point:\n    value:i64\nfun read[T](value:T)->i64:\n    return value.value\nfun main():\n    pass\n","generic field receivers"),
}
NEGATIVE["alias_count"]=("".join(f"type A{i}=i64\n" for i in range(1025))+"fun main():\n    pass\n","alias declaration limit")
NEGATIVE["alias_depth"]=("".join(f"type A{i}=A{i+1}\n" for i in range(66))+"type A66=i64\nfun main():\n    pass\n","alias expansion depth limit")
NEGATIVE["alias_growth"]=("type Pair[T]=(T,T)\ntype Huge="+"Pair["*14+"i64"+"]"*14+"\nfun main():\n    pass\n","complexity limit")


def main():
    compiler=str(pathlib.Path(sys.argv[1]).resolve())
    fixture=str(pathlib.Path(sys.argv[2]).resolve()) if len(sys.argv)>2 else None
    link=str(pathlib.Path(sys.argv[3]).resolve()) if len(sys.argv)>3 else fixture
    environment=os.environ.copy()
    if fixture:
        variable="PATH" if os.name=="nt" else "LD_LIBRARY_PATH"
        environment[variable]=str(pathlib.Path(fixture).parent)+os.pathsep+environment.get(variable,"")
    def run(args,expected=None,diagnostic=None):
        result=subprocess.run(args,capture_output=True,text=True,encoding="utf-8",timeout=60,env=environment)
        if diagnostic is not None:
            assert result.returncode!=0 and diagnostic in result.stderr,(args,result.returncode,result.stderr)
        else:
            assert result.returncode==0,(args,result.stderr)
            if expected is not None: assert result.stdout.replace("\r\n","\n")==expected,(args,result.stdout,expected)
        return result
    native="LLVM enabled" in run([compiler,"--version"]).stdout
    count=0;ffi_count=0
    with tempfile.TemporaryDirectory(prefix="pikaflow type aliases ") as temporary:
        root=pathlib.Path(temporary)
        if fixture and os.name=="nt": shutil.copy2(fixture,root/pathlib.Path(fixture).name)
        for name,(source,expected) in CASES.items():
            path=root/(name+".pf");path.write_text(textwrap.dedent(source).lstrip(),encoding="utf-8")
            run([compiler,"interpret",str(path)],expected);count+=1
            if native:
                for level in ("-O0","-O2"):
                    run([compiler,"run",str(path),level],expected);count+=1
                    output=root/(name+level+(".exe" if os.name=="nt" else ""))
                    run([compiler,"build",str(path),level,"-o",str(output)])
                    run([str(output)],expected);count+=1
        for name,(source,diagnostic) in NEGATIVE.items():
            path=root/(name+".pf");path.write_text(source,encoding="utf-8")
            for command in ("check","symbols"):
                run([compiler,command,str(path)],diagnostic=diagnostic);count+=1
        entry=root/"entry.pf"
        entry.write_text("type Void=unit\nfun test_alias()->Void:\n    print(42)\n",encoding="utf-8")
        run([compiler,"interpret",str(entry),"--entry","test_alias"],"42\n");count+=1
        entry_symbols=json.loads(run([compiler,"symbols",str(entry),"--library"]).stdout)["symbols"]
        assert next(symbol for symbol in entry_symbols if symbol["name"]=="test_alias")["type"]=="fn()->unit"
        count+=1
        library=root/"library.pf"
        library.write_text("type Private=i64\nexport type Public=Private\nexport record Cell[T]:\n    value:T\nexport type IntCell=Cell[i64]\nrecord Secret:\n    value:i64\nexport type Handle=Secret\nexport fun secret()->Handle:\n    return Secret(value:42)\n",encoding="utf-8")
        source=root/"modules.pf"
        source.write_text('import "library.pf" as lib\nfun accept(value:lib.Handle)->i64:\n    return 42\nfun main():\n    let value:lib.Public=42\n    let cell:lib.IntCell=lib.Cell(value:value)\n    print(cell.value)\n    print(accept(lib.secret()))\n',encoding="utf-8")
        run([compiler,"interpret",str(source)],"42\n42\n");count+=1
        module_symbols=json.loads(run([compiler,"symbols",str(source)]).stdout)["symbols"]
        text_files={str(path.resolve()):path.read_bytes().splitlines() for path in (source,library)}
        for symbol in module_symbols:
            if symbol["kind"] not in ("typeAlias","module"): continue
            for location in [symbol["definition"],*symbol["uses"]]:
                line=text_files[str(pathlib.Path(location["file"]).resolve())][location["line"]-1]
                offset=location["column"]-1
                assert line[offset:offset+len(symbol["name"])]==symbol["name"].encode(),(symbol,location,line)
        public=next(symbol for symbol in module_symbols if symbol["name"]=="Public" and symbol["kind"]=="typeAlias")
        assert len(public["uses"])==1 and pathlib.Path(public["uses"][0]["file"])==source
        count+=1
        for body in ("let value:lib.Private=42","print(lib.secret().value)"):
            source.write_text('import "library.pf" as lib\nfun main():\n    '+body+'\n',encoding="utf-8")
            run([compiler,"check",str(source)],diagnostic="private");count+=1
        (root/"other.pf").write_text("export record Public:\n    value:i64\n",encoding="utf-8")
        source.write_text('import "library.pf"\nimport "other.pf"\nfun use(value:Public):\n    pass\nfun main():\n    pass\n',encoding="utf-8")
        run([compiler,"check",str(source)],diagnostic="ambiguous imported type");count+=1
        source=root/"symbols.pf"
        source.write_text("type Count=i64\ntype Vec[T]=[T]\ntype Counts=Vec[Count]\nfun Count(value:i64)->i64:\n    return value+1\nfun first[T](values:Vec[T])->T:\n    return values[0]\nfun main():\n    let values:Counts=[Count(41)]\n    print(first(values))\n",encoding="utf-8")
        records=json.loads(run([compiler,"symbols",str(source)]).stdout)["symbols"]
        aliases={symbol["name"]:symbol for symbol in records if symbol["kind"]=="typeAlias"}
        assert set(aliases)=={"Count","Vec","Counts"},aliases
        assert aliases["Vec"]["generic"] and "[T] = [T]" in aliases["Vec"]["detail"]
        assert {use["line"] for use in aliases["Count"]["uses"]}=={3}
        assert {use["line"] for use in aliases["Vec"]["uses"]}=={3,6}
        parameters=[symbol for symbol in records if symbol["kind"]=="typeParameter" and symbol["definition"]["line"]==2]
        assert len(parameters)==1 and len(parameters[0]["uses"])==1,parameters
        original=source.read_text(encoding="utf-8");lines=original.splitlines(keepends=True)
        def renamed(symbol,replacement):
            edits=[]
            for location in [symbol["definition"],*symbol["uses"]]:
                offset=sum(len(line) for line in lines[:location["line"]-1])+location["column"]-1
                assert original[offset:offset+len(symbol["name"])]==symbol["name"]
                edits.append(offset)
            candidate=original
            for offset in sorted(set(edits),reverse=True): candidate=candidate[:offset]+replacement+candidate[offset+len(symbol["name"]):]
            target=root/(replacement+".pf");target.write_text(candidate,encoding="utf-8")
            run([compiler,"interpret",str(target)],"42\n")
        renamed(aliases["Count"],"Total");renamed(aliases["Vec"],"Items");renamed(parameters[0],"Element");count+=4
        wire=root/"wire.pf"
        wire.write_text('type Byte=c_i8\ntype Fields[T]=(Byte,c_u16,T)\ntype Wire=(Fields[c_i32],c_u8)\nextern fun convert(value:Wire)->Wire="pf_ffi_nested_widths"\nfun main():\n    unsafe:\n        let value=convert(((-4,65000,-65000),39))\n        print(value[0][0]+value[0][1]+value[0][2]+value[1])\n',encoding="utf-8")
        ir=run([compiler,"dump-ir",str(wire)]).stdout
        for annotation in ("@c_i8","@c_u16","@c_i32","@c_u8"): assert annotation in ir,(annotation,ir)
        count+=1
        if native and fixture:
            for level in ("-O0","-O2"):
                run([compiler,"run",str(wire),level,"--load",fixture],"42\n");ffi_count+=1
                output=root/("wire"+level+(".exe" if os.name=="nt" else ""))
                run([compiler,"build",str(wire),level,"--link",link,"-o",str(output)])
                run([str(output)],"42\n");ffi_count+=1
            wire.write_text('type Narrow=c_i8\nextern fun convert(value:Narrow)->Narrow="pf_ffi_i8"\nfun main():\n    unsafe:\n        print(convert(128))\n',encoding="utf-8")
            for level in ("-O0","-O2"):
                output=root/("range"+level+(".exe" if os.name=="nt" else ""))
                run([compiler,"build",str(wire),level,"--link",link,"-o",str(output)])
                for command in ([compiler,"run",str(wire),level,"--load",fixture],[str(output)]):
                    result=run(command,diagnostic="signed width")
                    assert "wire.pf:5:" in result.stderr,result.stderr
                    ffi_count+=1
    print(f"{count} type alias checks and {ffi_count} C wire alias executions/traps passed ({'LLVM enabled' if native else 'interpreter'})")


if __name__=="__main__":
    main()
