#include "pikaflow/optimizer.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
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
const Instruction& definition(const Function& function, int reg) {
    for(const auto& operation : function.code) if(operation.dst == reg) return operation;
    throw std::runtime_error("missing optimized definition");
}
size_t operations(const Function& function, Op op) {
    return size_t(std::count_if(function.code.begin(), function.code.end(),
        [&](const Instruction& operation) { return operation.op == op; }));
}
Program invoking(Function function, std::vector<int64_t> arguments) {
    auto program = single({}, {});
    auto& main = program.functions[0];
    for(size_t index = 0; index < arguments.size(); ++index) {
        main.registers.push_back(function.parameters[index]);
        main.code.push_back(ins(function.parameters[index] == Bool ? Op::ConstBool : Op::ConstI64,
                                int(index), {}, arguments[index]));
    }
    main.registers.push_back(I64);
    auto call = ins(Op::Call, int(arguments.size()));
    call.text = function.name;
    for(size_t index = 0; index < arguments.size(); ++index) call.args.push_back(int(index));
    main.code.push_back(call);
    main.code.push_back(ins(Op::Return, -1, {int(arguments.size())}));
    program.functions.push_back(std::move(function));
    return program;
}
Function arithmeticFunction(std::vector<Type> registers, std::vector<Instruction> code) {
    Function function;
    function.name = "calculate";
    function.parameters = {I64, I64};
    function.result = I64;
    function.registers = std::move(registers);
    function.code = std::move(code);
    return function;
}
void constantsAndTraps() {
    constexpr auto low=std::numeric_limits<int64_t>::min(),high=std::numeric_limits<int64_t>::max();
    struct Case { Op op; int64_t a,b,result; };
    for(const auto& test:std::vector<Case>{{Op::Add,20,22,42},{Op::Add,low,0,low},
            {Op::Sub,high,0,high},{Op::Mul,low,1,low},{Op::Mul,-7,-6,42},
            {Op::Div,-84,-2,42},{Op::Mod,low,1,0}}) {
        auto original=binary(test.op,test.a,test.b); OptimizationStats stats;
        auto optimized=optimizeIR(original,&stats);
        const auto& folded=definition(optimized.functions[0],2);
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
    require(definition(optimizeIR(negative).functions[0],1).integer==42,"safe negation did not fold");
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
    require(definition(optimized.functions[1],2).op==Op::ConstI64,"equal constants at CFG join were not propagated");
    require(optimized.functions[1].code[0].op==Op::JumpFalse,"unknown parameter branch was folded");
    require(interpret(optimized)==42,"join propagation changed result");
    program.functions[1].code[3].integer=41;
    optimized=optimizeIR(program);
    require(definition(optimized.functions[1],2).op==Op::Move,"different constants at CFG join were conflated");
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
void localExpressionsAndMutation() {
    for(Op op : {Op::Add, Op::Sub, Op::Mul, Op::Div, Op::Mod, Op::Neg}) {
        const std::vector<int> arguments = op==Op::Neg ? std::vector<int>{0} : std::vector<int>{0,1};
        auto function = arithmeticFunction({I64,I64,I64,I64,I64},
            {ins(op,2,arguments),ins(Op::Collect),ins(op,3,arguments),
             ins(Op::Add,4,{2,3}),ins(Op::Return,-1,{4})});
        auto original = invoking(function, {12,3});
        OptimizationStats stats;
        auto optimized = optimizeIR(original, &stats);
        const auto& reused = definition(optimized.functions[1],3);
        require(stats.commonExpressionsReused==1 && reused.op==Op::Move && reused.args==std::vector<int>{2},
                "duplicate checked expression was not reused");
        require(reused.location.line==17 && reused.location.column==9,"CSE lost source location");
        require(operations(optimized.functions[1],Op::Collect)==1,"CSE removed intervening collection");
        require(interpret(original)==interpret(optimized),"CSE changed dynamic arithmetic");
    }
    for(Op op : {Op::Eq,Op::Ne,Op::Lt,Op::Le,Op::Gt,Op::Ge,Op::Not}) {
        const auto input = op==Op::Not ? Bool : I64;
        const std::vector<int> arguments = op==Op::Not ? std::vector<int>{0} : std::vector<int>{0,1};
        auto function=arithmeticFunction({input,input,Bool,Bool,Bool,I64},
            {ins(op,2,arguments),ins(op,3,arguments),ins(Op::Eq,4,{2,3}),
             ins(Op::JumpFalse,-1,{4},0,6),ins(Op::ConstI64,5,{},42),ins(Op::Return,-1,{5}),
             ins(Op::ConstI64,5),ins(Op::Return,-1,{5})});
        function.parameters={input,input};
        auto original=invoking(function,op==Op::Not?std::vector<int64_t>{1,0}:std::vector<int64_t>{20,22});
        OptimizationStats stats;
        auto optimized=optimizeIR(original,&stats);
        require(stats.commonExpressionsReused==1 && interpret(original)==42 && interpret(optimized)==42,
                "scalar comparison or boolean CSE changed result");
    }
    auto copies = arithmeticFunction({I64,I64,I64,I64,I64,I64,I64},
        {ins(Op::Add,2,{0,1}),ins(Op::Move,3,{0}),ins(Op::Move,4,{1}),
         ins(Op::Add,5,{3,4}),ins(Op::Sub,6,{5,2}),ins(Op::Return,-1,{6})});
    OptimizationStats stats;
    auto optimized = optimizeIR(invoking(copies,{20,22}),&stats);
    require(stats.commonExpressionsReused==1 && stats.deadResultsRemoved==2,
            "value numbering did not follow scalar copies or delete their dead results");
    require(interpret(optimized)==0,"copy value numbering changed result");
    require(dumpIR(optimizeIR(optimized))==dumpIR(optimized),"CSE/DCE pipeline is not idempotent");

    for(bool overwriteOperand : {false,true}) {
        auto changed = arithmeticFunction({I64,I64,I64,I64,I64},
            {ins(Op::Add,2,{0,1}),ins(Op::ConstI64,overwriteOperand?0:2,{},99),
             ins(Op::Add,3,{0,1}),ins(Op::Sub,4,{3,2}),ins(Op::Return,-1,{4})});
        auto original = invoking(changed,{20,22});
        optimized = optimizeIR(original,&stats);
        require(stats.commonExpressionsReused==0,"CSE reused an overwritten operand or representative");
        require(interpret(original)==interpret(optimized),"mutable-register CSE changed result");
    }
    auto edges = arithmeticFunction({I64,I64,Bool,I64,I64,I64},
        {ins(Op::JumpFalse,-1,{2},0,3),ins(Op::Move,0,{1}),ins(Op::Jump,-1,{},0,3),
         ins(Op::Add,3,{0,1}),ins(Op::Add,4,{1,1}),ins(Op::Sub,5,{3,4}),ins(Op::Return,-1,{5})});
    edges.parameters.push_back(Bool);
    for(int64_t condition : {0,1}) {
        auto original = invoking(edges,{20,2,condition});
        optimized = optimizeIR(original,&stats);
        require(stats.commonExpressionsReused==0,"linear scan incorrectly equated incoming registers at a CFG join");
        require(interpret(original)==interpret(optimized),"edge-dependent register alias changed result");
    }
}
void scalarLivenessAndEffects() {
    auto dead = single({I64,I64,Bool,Bool},
        {ins(Op::ConstI64,0,{},42),ins(Op::ConstI64,1,{},99),ins(Op::Lt,2,{0,1}),
         ins(Op::Not,3,{2}),ins(Op::Return,-1,{0})});
    OptimizationStats stats;
    auto optimized = optimizeIR(dead,&stats);
    require(stats.deadResultsRemoved==3 && optimized.functions[0].code.size()==2,
            "dead scalar dependency chain was not removed");
    require(interpret(optimized)==42,"dead scalar deletion changed result");
    dead.functions[0].registers.push_back(Type::cell(I64));
    dead.functions[0].localNames[4]="debug_value";
    dead.functions[0].code.insert(dead.functions[0].code.begin()+2,ins(Op::MakeCell,4,{1}));
    optimized=optimizeIR(dead);
    require(definition(optimized.functions[0],4).op==Op::MakeCell &&
            optimized.functions[0].localNames==dead.functions[0].localNames,"named debug definitions were erased");

    auto loop = arithmeticFunction({I64,I64,I64,Bool,I64},
        {ins(Op::ConstI64,1),ins(Op::ConstI64,2,{},1),ins(Op::Lt,3,{1,0}),
         ins(Op::JumpFalse,-1,{3},0,7),ins(Op::ConstI64,4,{},99),ins(Op::Add,1,{1,2}),
         ins(Op::Jump,-1,{},0,2),ins(Op::Return,-1,{1})});
    loop.parameters={I64};
    auto original=invoking(loop,{10});
    optimized=optimizeIR(original,&stats);
    require(stats.deadResultsRemoved==1 && interpret(optimized)==10 && interpret(original)==10,
            "liveness failed across a loop backedge or target relocation");

    auto trap = arithmeticFunction({I64,I64,I64},
        {ins(Op::Div,2,{0,1}),ins(Op::Return,-1,{0})});
    optimized=optimizeIR(invoking(trap,{42,2}));
    require(operations(optimized.functions[1],Op::Div)==1,"dead checked operation lost its possible failure");
    auto heap=single({I64,Type::cell(I64),I64,Type::array(I64),I64,String,Bool},
        {ins(Op::ConstI64,0,{},42),ins(Op::MakeCell,1,{0}),ins(Op::LoadCell,2,{1}),
         ins(Op::LoadCell,2,{1}),ins(Op::MakeArray,3,{0}),ins(Op::ConstI64,4),
         ins(Op::Index,2,{3,4}),ins(Op::Length,2,{3}),ins(Op::Collect),
         ins(Op::ConstString,5),ins(Op::Eq,6,{5,5}),ins(Op::Print,-1,{0}),ins(Op::Return,-1,{0})});
    heap.functions[0].code[9].text="value";
    optimized=optimizeIR(heap,&stats);
    require(stats.commonExpressionsReused==0 && stats.deadResultsRemoved==0,
            "heap, string or effect operations participated in scalar elimination");

    auto bounded=single({I64},{ins(Op::ConstI64,0,{},42),ins(Op::Return,-1,{0})});
    Function stress;
    stress.name="bounded_liveness";
    stress.result=Unit;
    stress.registers.assign(1000,I64);
    stress.registers[0]=Bool;
    stress.parameters=stress.registers;
    for(int block=0;block<997;++block) {
        stress.code.push_back(ins(Op::Print,-1,{block+1}));
        stress.code.push_back(ins(Op::Jump,-1,{},0,int(stress.code.size()+1)));
    }
    stress.code.push_back(ins(Op::JumpFalse,-1,{0},0,int(stress.code.size()+2)));
    stress.code.push_back(ins(Op::Jump,-1,{},0,0));
    stress.code.push_back(ins(Op::Return));
    bounded.functions.push_back(std::move(stress));
    optimized=optimizeIR(bounded,&stats);
    require(stats.livenessSkipped==1 && dumpIR(optimized)==dumpIR(bounded),
            "liveness budget exhaustion must retain all instructions without partial deletion");
}
} // namespace
int main() {
    try {
        constantsAndTraps();branchesAndEffects();joinsAndLoops();metadataAndBounds();floatingValuesStayOpaque();
        localExpressionsAndMutation();scalarLivenessAndEffects();
        std::cout<<"Typed IR optimization tests passed\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
