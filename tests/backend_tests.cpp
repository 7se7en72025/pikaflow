#include "pikaflow/backend.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
using namespace pika;
Instruction operation(Op op,int dst=-1,std::vector<int> args={},int64_t integer=0,std::string text={}) {
    Instruction result{op}; result.dst=dst; result.args=std::move(args);
    result.integer=integer; result.text=std::move(text); result.location={"backend-test.pf",1,1};
    return result;
}
void require(bool condition,const char* message) {
    if(!condition) throw std::runtime_error(message);
}
Program arithmetic() {
    Program program; program.sourceFile="backend-test.pf";
    Function add; add.name="add"; add.parameters={I64,I64}; add.parameterNames={"a","b"};
    add.result=I64; add.registers={I64,I64,I64}; add.location={program.sourceFile,1,1};
    add.code={operation(Op::Add,2,{0,1}),operation(Op::Return,-1,{2})};
    Function main; main.name="main"; main.result=I64; main.registers={I64,I64,I64}; main.location={program.sourceFile,4,1};
    main.code={operation(Op::ConstI64,0,{},20),operation(Op::ConstI64,1,{},22),
        operation(Op::Call,2,{0,1},0,"add"),operation(Op::Return,-1,{2})};
    program.functions={add,main}; return program;
}
Program rootsAndClosure() {
    Program program; program.sourceFile="backend-test.pf";
    Function closure; closure.name="captured"; closure.parameters={I64}; closure.parameterNames={"delta"};
    closure.result=I64; closure.registers={I64,Type::cell(I64),I64,I64}; closure.location={program.sourceFile,1,1};
    closure.code={operation(Op::Capture,1,{},0),operation(Op::LoadCell,2,{1}),
        operation(Op::Add,3,{0,2}),operation(Op::Return,-1,{3})};
    Function main; main.name="main"; main.result=I64; main.location={program.sourceFile,6,1};
    main.registers={I64,Type::cell(I64),Type::function({I64},I64),I64,I64,Type::array(I64),I64,I64};
    main.localNames[1]="captured_value";
    main.code={operation(Op::ConstI64,0,{},40),operation(Op::MakeCell,1,{0}),
        operation(Op::MakeClosure,2,{1},0,"captured"),operation(Op::Collect),
        operation(Op::ConstI64,3,{},2),operation(Op::CallIndirect,4,{2,3}),
        operation(Op::MakeArray,5,{4}),operation(Op::Collect),
        operation(Op::ConstI64,6,{},0),operation(Op::Index,7,{5,6}),operation(Op::Return,-1,{7})};
    program.functions={closure,main}; return program;
}
Program identity() {
    auto program=arithmetic();
    auto& add=program.functions[0];
    add.parameters={I64}; add.parameterNames={"a"}; add.registers={I64,I64,I64};
    add.code={operation(Op::ConstI64,1,{},0),operation(Op::Add,2,{0,1}),operation(Op::Return,-1,{2})};
    auto& main=program.functions[1];
    main.code={operation(Op::ConstI64,0,{},42),operation(Op::Call,2,{0},0,"add"),operation(Op::Return,-1,{2})};
    return program;
}
Program rawMemory() {
    Program program; program.sourceFile="backend-test.pf";
    Function main; main.name="main"; main.result=I64; main.location={program.sourceFile,1,1};
    main.registers={I64,Type{TypeKind::Pointer,{I64}},I64,I64,I64};
    main.code={operation(Op::ConstI64,0,{},1),operation(Op::RawAlloc,1,{0}),
        operation(Op::ConstI64,2,{},0),operation(Op::ConstI64,3,{},42),
        operation(Op::RawStore,-1,{1,2,3}),operation(Op::RawLoad,4,{1,2}),
        operation(Op::RawFree,-1,{1}),operation(Op::Return,-1,{4})};
    program.functions={main}; return program;
}
Program managedResult(bool error) {
    Program program; program.sourceFile="backend-test.pf";
    Function main; main.name="main"; main.result=I64; main.location={program.sourceFile,1,1};
    Type array=Type::array(I64);
    Type result{TypeKind::Result,error?std::vector<Type>{String,array}:std::vector<Type>{array,String}};
    main.registers={Bool,array,String,result,array,I64,I64};
    main.code={operation(Op::ConstI64,5,{},42),operation(Op::MakeArray,1,{5}),
        operation(Op::ConstBool,0,{},error?0:1),operation(Op::Zero,2),
        operation(Op::MakeTuple,3,error?std::vector<int>{0,2,1}:std::vector<int>{0,1,2}),
        operation(Op::Collect),operation(error?Op::ResultError:Op::Unwrap,4,{3}),
        operation(Op::ConstI64,5,{},0),operation(Op::Index,6,{4,5}),operation(Op::Return,-1,{6})};
    program.functions={main}; return program;
}
Program isolatedTask() {
    Program program; program.sourceFile="backend-test.pf";
    Type array=Type::array(I64),cell=Type::cell(array),closure=Type::function({},array);
    Function worker;worker.name="worker";worker.result=array;worker.location={program.sourceFile,1,1};
    worker.registers={cell,array,I64,I64,I64,I64,array};
    worker.code={operation(Op::Capture,0,{},0),operation(Op::LoadCell,1,{0}),operation(Op::Collect),
        operation(Op::ConstI64,2,{},0),operation(Op::Index,3,{1,2}),operation(Op::ConstI64,4,{},35),
        operation(Op::Add,5,{3,4}),operation(Op::MakeArray,6,{5}),operation(Op::Return,-1,{6})};
    Function main;main.name="main";main.result=I64;main.location={program.sourceFile,5,1};
    main.registers={I64,array,cell,closure,Type{TypeKind::Task,{array}},I64,I64,array,I64};
    main.code={operation(Op::ConstI64,0,{},7),operation(Op::MakeArray,1,{0}),operation(Op::MakeCell,2,{1}),
        operation(Op::MakeClosure,3,{2},0,"worker"),operation(Op::Spawn,4,{3}),
        operation(Op::ConstI64,5,{},0),operation(Op::ConstI64,6,{},99),operation(Op::SetIndex,-1,{1,5,6}),
        operation(Op::Collect),operation(Op::Join,7,{4}),operation(Op::Collect),
        operation(Op::Index,8,{7,5}),operation(Op::Return,-1,{8})};
    program.functions={worker,main};return program;
}
class ForeignTest {
public:
    Program program;
    Function function;
    ForeignTest() {
        program.sourceFile="ffi-test.pf";function.name="main";function.result=I64;
        function.location={program.sourceFile,1,1};
    }
    int emit(Op op,Type type,std::vector<int> args={}) {
        int destination=type==Unit?-1:int(function.registers.size());
        if(destination>=0)function.registers.push_back(type);
        function.code.push_back(operation(op,destination,std::move(args)));
        return destination;
    }
    int integer(int64_t value) {int r=emit(Op::ConstI64,I64);function.code.back().integer=value;return r;}
    int floating(double value) {int r=emit(Op::ConstF64,F64);function.code.back().real=value;return r;}
    int boolean(bool value) {int r=emit(Op::ConstBool,Bool);function.code.back().integer=value;return r;}
    int tuple(std::vector<int> fields) {
        std::vector<Type> types;for(int r:fields)types.push_back(function.registers[r]);
        return emit(Op::MakeTuple,Type{TypeKind::Tuple,std::move(types)},std::move(fields));
    }
    int index(int tuple,unsigned n) {
        auto type=function.registers[tuple].args.at(n);
        int which=integer(n);return emit(Op::Index,type,{tuple,which});
    }
    int asInteger(int reg) {return function.registers[reg]==I64?reg:emit(Op::Convert,I64,{reg});}
    int sum(int tuple) {
        size_t count=function.registers[tuple].args.size();int result=integer(0);
        for(size_t n=0;n<count;++n)result=emit(Op::Add,I64,{result,asInteger(index(tuple,unsigned(n)))});
        return result;
    }
    int call(std::string name,Type result,std::vector<int> arguments,size_t fixed=SIZE_MAX) {
        std::vector<Type> parameters;
        size_t count=fixed==SIZE_MAX?arguments.size():fixed;
        for(size_t n=0;n<count;++n)parameters.push_back(function.registers[arguments[n]]);
        program.externals.push_back({name,std::move(parameters),result,fixed!=SIZE_MAX});
        int reg=emit(Op::ExternalCall,result,std::move(arguments));function.code.back().text=std::move(name);return reg;
    }
    Program finish(int result) {
        function.code.push_back(operation(Op::Return,-1,{result}));program.functions.push_back(std::move(function));return std::move(program);
    }
};
Program aggregateCase(unsigned choice) {
    ForeignTest b;
    const Type pair{TypeKind::Tuple,{I64,I64}},mixed{TypeKind::Tuple,{F64,I64}},
        doubles{TypeKind::Tuple,{F64,F64}},big{TypeKind::Tuple,{I64,I64,I64}};
    int result=-1;
    if(choice==0) {
        int tuple=b.tuple({b.integer(10),b.integer(20)});
        result=b.sum(b.call("pf_ffi_pair",pair,{tuple,b.integer(6)}));
    } else if(choice==1) {
        int tuple=b.tuple({b.floating(10),b.integer(20)});
        result=b.sum(b.call("pf_ffi_mixed",mixed,{tuple,b.floating(12)}));
    } else if(choice==2) {
        int tuple=b.tuple({b.boolean(true),b.floating(40),b.boolean(true)});
        result=b.call("pf_ffi_padded",I64,{tuple});
    } else if(choice==3) {
        int tuple=b.tuple({b.boolean(true),b.boolean(true),b.boolean(true)});
        result=b.call("pf_ffi_tiny",I64,{tuple});
    } else if(choice==4) {
        int tuple=b.tuple({b.floating(10),b.floating(20)});
        result=b.sum(b.call("pf_ffi_doubles",doubles,{tuple,b.floating(6)}));
    } else if(choice==5) {
        int tuple=b.tuple({b.integer(1),b.integer(2),b.integer(3)});
        result=b.sum(b.call("pf_ffi_big",big,{tuple,b.integer(12)}));
    } else if(choice==6) {
        int tuple=b.tuple({b.integer(10),b.integer(11)});
        result=b.call("pf_ffi_pressure",I64,{b.integer(1),b.integer(2),b.integer(3),b.integer(4),b.integer(5),tuple,b.integer(6)});
    } else if(choice==7) {
        int tuple=b.tuple({b.floating(20),b.floating(22)});
        result=b.asInteger(b.call("pf_ffi_sse_pressure",F64,{b.floating(0),b.floating(0),b.floating(0),b.floating(0),b.floating(0),b.floating(0),b.floating(0),tuple,b.floating(0)}));
    } else if(choice==8) {
        int tuple=b.tuple({b.integer(10),b.integer(20)});
        result=b.call("pf_ffi_varargs",I64,{b.integer(1),b.boolean(true),b.floating(10),tuple},1);
    } else if(choice==9) {
        int pointer=b.emit(Op::RawAlloc,Type{TypeKind::Pointer,{I64}},{b.integer(1)});
        b.emit(Op::RawStore,Unit,{pointer,b.integer(0),b.integer(42)});
        int tuple=b.tuple({pointer,b.boolean(true)});
        result=b.call("pf_ffi_pointer",I64,{tuple});b.emit(Op::RawFree,Unit,{pointer});
    } else if(choice==10) {
        Type one{TypeKind::Tuple,{I64}};
        result=b.index(b.call("pf_ffi_one",one,{b.tuple({b.integer(20)})}),0);
    } else {
        Type small{TypeKind::Tuple,{Bool,Bool}};
        int value=b.call("pf_ffi_small",small,{b.tuple({b.boolean(false),b.boolean(true)})});
        result=b.call("pf_ffi_small_score",I64,{value});
    }
    return b.finish(result);
}
Program narrowRecord() {
    ForeignTest b;
    auto inner=b.tuple({b.integer(-4),b.integer(65000),b.integer(-65000)});
    auto tuple=b.tuple({inner,b.integer(39)});
    Type record=b.function.registers[tuple];
    int returned=b.call("pf_ffi_nested_widths",record,{tuple});
    CABIType innerABI{0,true,{{8,true,{}},{16,false,{}},{32,true,{}}}};
    CABIType recordABI{0,true,{innerABI,{8,false,{}}}};
    b.program.externals.back().abiParameters={recordABI};
    b.program.externals.back().abiResult=recordABI;
    int total=b.sum(b.index(returned,0));
    return b.finish(b.emit(Op::Add,I64,{total,b.index(returned,1)}));
}
}
int main() {
    try {
        require(runJIT(arithmetic(),0)==42,"O0 arithmetic JIT returned incorrect result");
        require(runJIT(arithmetic(),2)==42,"O2 arithmetic JIT returned incorrect result");
        require(runJIT(arithmetic(),1)==42,"O1 arithmetic JIT returned incorrect result");
        require(runJIT(arithmetic(),3)==42,"O3 arithmetic JIT returned incorrect result");
        require(runJIT(rootsAndClosure(),0)==42,"O0 GC roots/closures failed");
        require(runJIT(rootsAndClosure(),2)==42,"O2 GC roots/closures failed");
        require(runJIT(rawMemory(),0)==42,"O0 raw memory failed");
        require(runJIT(rawMemory(),2)==42,"O2 raw memory failed");
        require(runJIT(managedResult(false),0)==42,"managed success Result failed");
        require(runJIT(managedResult(false),2)==42,"optimized managed success Result failed");
        require(runJIT(managedResult(true),0)==42,"managed error Result failed");
        require(runJIT(managedResult(true),2)==42,"optimized managed error Result failed");
        require(runJIT(isolatedTask(),0)==42,"O0 isolated task capture/result failed");
        require(runJIT(isolatedTask(),2)==42,"O2 isolated task capture/result failed");
        auto plain=emitLLVM(identity(),0);
        auto optimized=emitLLVM(identity(),2);
        require(plain.find("call i64 @pf_checked_add")!=std::string::npos,"baseline checked addition missing");
        require(optimized.find("call i64 @pf_checked_add")==std::string::npos,"checked identity custom pass did not fold addition");
        require(emitLLVM(arithmetic(),0,true).find("!DICompileUnit")!=std::string::npos,"debug metadata missing");
        require(emitLLVM(rootsAndClosure(),0,true).find("name: \"captured_value\"")!=std::string::npos,"source variable debug metadata missing");
        require(emitLLVM(rootsAndClosure(),0,true).find("DW_OP_deref")!=std::string::npos,"debugger payload address must have stable storage");
        require(emitAssembly(arithmetic(),0).find("pf_fn_add")!=std::string::npos,"native assembly missing function");
#ifdef PF_FFI_FIXTURE
        loadLibrary(PF_FFI_FIXTURE);
        require(runJIT(narrowRecord(),0)==42,"O0 nested narrow C record failed");
        require(runJIT(narrowRecord(),2)==42,"O2 nested narrow C record failed");
#ifndef _WIN32
        require(emitLLVM(aggregateCase(0),0).find("declare { i64, i64 } @pf_ffi_pair(i64, i64, i64)")!=std::string::npos,
            "SysV C POD pair must use register arguments and return values");
        require(emitLLVM(narrowRecord(),0).find("alloca [16 x i8]")!=std::string::npos,
            "SysV record coercion storage must include the rounded register padding");
#endif
        for(unsigned choice=0;choice<12;++choice) {
            require(runJIT(aggregateCase(choice),0)==42,"O0 C aggregate ABI mismatch");
            require(runJIT(aggregateCase(choice),2)==42,"O2 C aggregate ABI mismatch");
        }
#endif
        std::cout << "LLVM backend tests passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
