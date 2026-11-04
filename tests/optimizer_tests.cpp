#include "pikaflow/optimizer.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace pika;
void require(bool value, const char* message) { if(!value) throw std::runtime_error(message); }
Instruction ins(Op op, int dst=-1, std::vector<int> args={}, int64_t value=0, int target=-1) {
    Instruction result(op); result.dst=dst; result.args=std::move(args); result.integer=value;
    result.target=target; result.location={"optimizer-test.pf",17,9}; return result;
}
Program single(std::vector<Type> registers, std::vector<Instruction> code) {
    Program program; program.sourceFile="optimizer-test.pf";
    Function function; function.name="main"; function.result=I64;
    function.registers=std::move(registers); function.code=std::move(code);
    function.location={program.sourceFile,1,1}; program.functions.push_back(std::move(function)); return program;
}
Program binary(Op op,int64_t a,int64_t b) {
    return single({I64,I64,I64},{ins(Op::ConstI64,0,{},a),ins(Op::ConstI64,1,{},b),
        ins(op,2,{0,1}),ins(Op::Return,-1,{2})});
}
void constantsAndTraps() {
    constexpr auto low=std::numeric_limits<int64_t>::min(),high=std::numeric_limits<int64_t>::max();
    struct Case { Op op; int64_t a,b,result; };
    for(const auto& test:std::vector<Case>{{Op::Add,20,22,42},{Op::Add,low,0,low},
            {Op::Sub,high,0,high},{Op::Mul,low,1,low},{Op::Mul,-7,-6,42},
            {Op::Div,-84,-2,42},{Op::Mod,low,1,0}}) {
        auto original=binary(test.op,test.a,test.b); OptimizationStats stats;
        auto optimized=optimizeIR(original,&stats);
        const auto& folded=optimized.functions[0].code[2];
        require(folded.op==Op::ConstI64 && folded.integer==test.result,"safe constant arithmetic was not folded");
        require(folded.location.file=="optimizer-test.pf" && folded.location.line==17 && folded.location.column==9,
                "constant folding changed source location");
        require(original.functions[0].code[2].op==test.op,"optimizer changed reference program");
        require(stats.constantsFolded==1,"constant-fold statistics incorrect");
    }
    for(const auto& test:std::vector<Case>{{Op::Add,high,1,0},{Op::Add,low,-1,0},
            {Op::Sub,low,1,0},{Op::Sub,high,-1,0},{Op::Mul,high,2,0},{Op::Mul,low,-1,0},
            {Op::Div,low,-1,0},{Op::Mod,low,-1,0},{Op::Div,42,0,0},{Op::Mod,42,0,0}}) {
        auto optimized=optimizeIR(binary(test.op,test.a,test.b));
        require(optimized.functions[0].code[2].op==test.op,"optimizer removed a checked failure");
    }
    auto negative=single({I64,I64},{ins(Op::ConstI64,0,{},low),ins(Op::Neg,1,{0}),ins(Op::Return,-1,{1})});
    require(optimizeIR(negative).functions[0].code[1].op==Op::Neg,"optimizer removed negation overflow");
    negative.functions[0].code[0].integer=-42;
    require(optimizeIR(negative).functions[0].code[1].integer==42,"safe negation did not fold");
}
void branchesAndEffects() {
    auto program=single({Bool,I64,Type::array(I64)},
        {ins(Op::ConstBool,0,{},1),ins(Op::JumpFalse,-1,{0},0,5),ins(Op::ConstI64,1,{},42),
         ins(Op::MakeArray,2,{1}),ins(Op::Jump,-1,{},0,7),ins(Op::ConstI64,1,{},99),
         ins(Op::Print,-1,{1}),ins(Op::Collect),ins(Op::Return,-1,{1})});
    OptimizationStats stats; auto optimized=optimizeIR(program,&stats);
    require(stats.branchesFolded==1 && stats.unreachableRemoved==2,"constant branch did not remove only its dead arm");
    bool allocation=false,collection=false;
    for(const auto& instruction:optimized.functions[0].code) {
        require(instruction.op!=Op::Print,"unreachable print survived");
        allocation|=instruction.op==Op::MakeArray;collection|=instruction.op==Op::Collect;
    }
    require(allocation&&collection,"optimizer erased reachable allocation or collection");
    require(interpret(program)==42 && interpret(optimized)==42,"branch rewrite changed reference result");
    require(dumpIR(optimizeIR(optimized))==dumpIR(optimized),"optimization is not idempotent");
}
void joinsAndLoops() {
    auto program=single({Bool,I64},{ins(Op::ConstBool,0,{},1),ins(Op::Call,1,{0}),ins(Op::Return,-1,{1})});
    program.functions[0].code[1].text="choose";
    Function choose;choose.name="choose";choose.parameters={Bool};choose.result=I64;
    choose.registers={Bool,I64,I64};choose.code={ins(Op::JumpFalse,-1,{0},0,3),ins(Op::ConstI64,1,{},42),
        ins(Op::Jump,-1,{},0,4),ins(Op::ConstI64,1,{},42),ins(Op::Move,2,{1}),ins(Op::Return,-1,{2})};
    program.functions.push_back(choose);
    auto optimized=optimizeIR(program);
    require(optimized.functions[1].code[4].op==Op::ConstI64,"equal constants at CFG join were not propagated");
    require(optimized.functions[1].code[0].op==Op::JumpFalse,"unknown parameter branch was folded");
    require(interpret(optimized)==42,"join propagation changed result");
    program.functions[1].code[3].integer=41;
    optimized=optimizeIR(program);
    require(optimized.functions[1].code[4].op==Op::Move,"different constants at CFG join were conflated");
    for(int64_t value: {int64_t(0),int64_t(1)}) {
        program.functions[0].code[0].integer=value;optimized=optimizeIR(program);
        require(interpret(program)==interpret(optimized),"unknown branch execution changed");
    }
    auto loop=single({I64,I64,I64,Bool},
        {ins(Op::ConstI64,0,{},0),ins(Op::ConstI64,1,{},1),ins(Op::ConstI64,2,{},3),
         ins(Op::Lt,3,{0,2}),ins(Op::JumpFalse,-1,{3},0,7),ins(Op::Add,0,{0,1}),
         ins(Op::Jump,-1,{},0,3),ins(Op::Return,-1,{0})});
    optimized=optimizeIR(loop);
    require(optimized.functions[0].code[4].op==Op::JumpFalse,"loop backedge did not invalidate first-iteration constant");
    require(interpret(loop)==3 && interpret(optimized)==3,"loop dataflow changed termination/result");
}
void metadataAndBounds() {
    auto program=binary(Op::Add,20,22);
    program.functions[0].registers.push_back(Type::cell(I64));
    program.functions[0].localNames[3]="kept_debug_name";
    program.nominalLayouts["Example"]={TypeKind::Tuple,{I64},"Example"};
    External foreign{"narrow",{I64},I64,false,"native_narrow"};
    foreign.abiParameters={{32,true,{}}};foreign.abiResult={32,true,{}};
    program.externals.push_back(foreign);
    Symbol symbol;symbol.id="binding";symbol.name="kept_debug_name";symbol.type=I64;
    symbol.definition={program.sourceFile,4,2};symbol.uses.push_back({program.sourceFile,9,3});
    program.symbols.push_back(symbol);
    auto optimized=optimizeIR(program);
    require(optimized.nominalLayouts==program.nominalLayouts,"nominal layout registry changed");
    require(optimized.externals[0].abiParameters==foreign.abiParameters && optimized.externals[0].abiResult==foreign.abiResult,
            "C ABI descriptors changed");
    require(optimized.functions[0].localNames==program.functions[0].localNames,"debug variable metadata changed");
    require(optimized.symbols.size()==1 && optimized.symbols[0].id==symbol.id && optimized.symbols[0].uses[0].line==9,
            "semantic source index metadata changed");
    auto invalid=program;invalid.functions[0].code[2].args={99,0};
    bool rejected=false;
    try { optimizeIR(invalid); } catch(const Error&) { rejected=true; }
    require(rejected,"optimizer accepted invalid input IR");
    auto large=single(std::vector<Type>(1100,I64),{});
    for(int pc=0;pc<1000;++pc)large.functions[0].code.push_back(ins(Op::Jump,-1,{},0,pc+1));
    large.functions[0].code.push_back(ins(Op::ConstI64,0,{},42));large.functions[0].code.push_back(ins(Op::Return,-1,{0}));
    OptimizationStats stats;optimized=optimizeIR(large,&stats);
    require(stats.functionsSkipped==1 && dumpIR(optimized)==dumpIR(large),"analysis bound must retain unchanged function");
}
void floatingValuesStayOpaque() {
    auto program=binary(Op::Add,20,22);
    Function floating;floating.name="floating";floating.result=F64;floating.registers={F64,F64,F64,Bool};
    floating.code={ins(Op::ConstF64,0),ins(Op::ConstF64,1),ins(Op::Add,2,{0,1}),
                   ins(Op::Eq,3,{0,1}),ins(Op::Return,-1,{2})};
    floating.code[0].real=-0.0;
    program.functions.push_back(floating);
    auto optimized=optimizeIR(program);
    require(optimized.functions[1].code[2].op==Op::Add && optimized.functions[1].code[3].op==Op::Eq,
            "typed optimizer changed floating-point operations");
    require(std::signbit(optimized.functions[1].code[0].real),"copy lost negative floating-point zero");
}
} // namespace
int main() {
    try {
        constantsAndTraps();branchesAndEffects();joinsAndLoops();metadataAndBounds();floatingValuesStayOpaque();
        std::cout<<"Typed IR optimization tests passed\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
