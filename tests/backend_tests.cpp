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
}
int main() {
    try {
        require(runJIT(arithmetic(),0)==42,"O0 arithmetic JIT returned incorrect result");
        require(runJIT(arithmetic(),2)==42,"O2 arithmetic JIT returned incorrect result");
        require(runJIT(rootsAndClosure(),0)==42,"O0 GC roots/closures failed");
        require(runJIT(rootsAndClosure(),2)==42,"O2 GC roots/closures failed");
        auto plain=emitLLVM(identity(),0);
        auto optimized=emitLLVM(identity(),2);
        require(plain.find("call i64 @pf_checked_add")!=std::string::npos,"baseline checked addition missing");
        require(optimized.find("call i64 @pf_checked_add")==std::string::npos,"checked identity custom pass did not fold addition");
        require(emitLLVM(arithmetic(),0,true).find("!DICompileUnit")!=std::string::npos,"debug metadata missing");
        require(emitAssembly(arithmetic(),0).find("pf_fn_add")!=std::string::npos,"native assembly missing function");
        std::cout << "LLVM backend tests passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
