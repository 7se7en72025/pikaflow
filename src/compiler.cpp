#include "pikaflow/ir.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace pika {
namespace {
struct Binding { int cell; Type type; bool mutableValue; };
struct Declaration { StmtPtr ast; std::string irName; std::string file; Type type; };
struct Loop { int head; std::vector<int> carry; std::vector<size_t> breaks; std::vector<int> exit; };
struct Context {
    size_t function;
    std::vector<std::map<std::string, Binding>> scopes;
    std::vector<int> stack;
    std::vector<Loop> loops;
    bool stackMode = false;
};
class Compiler {
    Program program;
    std::vector<Declaration> declarations;
    std::map<std::string, size_t> byModule;
    std::map<std::string, std::vector<size_t>> exported;
    std::map<std::string, External> externals;
    std::map<std::string,std::set<std::string>> imports;
    std::unique_ptr<Context> context;
    unsigned serial = 0;

    Function& fn() { return program.functions[context->function]; }
    const Type& type(int reg) { return reg < 0 ? Unit : fn().registers.at(reg); }
    [[noreturn]] void fail(const Location& loc, const std::string& message) { throw Error(loc, message); }
    int reg(Type t) { fn().registers.push_back(std::move(t)); return static_cast<int>(fn().registers.size() - 1); }
    int emit(Op op, Type t, std::vector<int> args = {}, Location loc = {}) {
        Instruction instruction{op}; instruction.dst = t == Unit ? -1 : reg(std::move(t));
        instruction.args = std::move(args); instruction.location = std::move(loc);
        int result = instruction.dst; fn().code.push_back(std::move(instruction)); return result;
    }
    size_t jump(Op op, int value, Location loc) {
        Instruction ins{op}; if (value >= 0) ins.args = {value}; ins.location = std::move(loc);
        fn().code.push_back(std::move(ins)); return fn().code.size() - 1;
    }
    void patch(size_t index, int target) { fn().code.at(index).target = target; }
    int position() { return static_cast<int>(fn().code.size()); }
    int integer(int64_t value, Location loc = {}) { int r = emit(Op::ConstI64, I64, {}, loc); fn().code.back().integer = value; return r; }
    int boolean(bool value, Location loc = {}) { int r = emit(Op::ConstBool, Bool, {}, loc); fn().code.back().integer = value; return r; }
    int string(const std::string& value, Location loc = {}) { int r = emit(Op::ConstString, String, {}, loc); fn().code.back().text = value; return r; }
    void require(const Type& actual, const Type& expected, Location loc, const std::string& what) {
        if (actual != expected) fail(loc, what + " requires " + expected.str() + ", received " + actual.str());
    }
    Binding* binding(const std::string& name) {
        for (auto it = context->scopes.rbegin(); it != context->scopes.rend(); ++it) {
            auto found = it->find(name); if (found != it->end()) return &found->second;
        }
        return nullptr;
    }
    Declaration* declaration(const std::string& name, const Location& loc) {
        auto local = byModule.find(loc.file + "::" + name);
        if (local != byModule.end()) return &declarations[local->second];
        auto global = exported.find(name);
        if (global == exported.end()) return nullptr;
        Declaration* result=nullptr;
        for(size_t index:global->second)if(imports[loc.file].count(declarations[index].file)) {
            if(result)fail(loc,"ambiguous imported symbol '"+name+"'");result=&declarations[index];
        }
        return result;
    }
    void define(const std::string& name, int value, bool mut, Location loc) {
        if (context->scopes.back().count(name)) fail(loc, "duplicate local name '" + name + "'");
        if (value < 0) fail(loc, "cannot bind a unit value");
        Type t = type(value); int cell = emit(Op::MakeCell, Type::cell(t), {value}, loc);
        context->scopes.back()[name] = {cell, t, mut};
    }
    std::vector<std::pair<std::string, Binding>> captures() {
        std::map<std::string, Binding> visible;
        for (auto& scope : context->scopes) for (auto& item : scope) visible[item.first] = item.second;
        return {visible.begin(), visible.end()};
    }
    bool returns(const std::vector<StmtPtr>& body) {
        for (auto& statement : body) {
            if (statement->kind == Stmt::Kind::Return) return true;
            if (statement->kind == Stmt::Kind::If && !statement->elseBody.empty() && returns(statement->body) && returns(statement->elseBody)) return true;
            if(statement->kind==Stmt::Kind::Match){bool wildcard=false,all=true;for(auto& arm:statement->body){wildcard=wildcard || arm->name=="_";all=all && returns(arm->body);}if(wildcard && all)return true;}
        }
        return false;
    }
    std::vector<Type> outputs(const Type& result) {
        if (result == Unit) return {};
        if (result.kind == TypeKind::Tuple) return result.args;
        return {result};
    }
    int takeOutputs(const Type& result, Location loc) {
        auto expected = outputs(result);
        if (context->stack.size() < expected.size()) fail(loc, "stack underflow: block result needs " + std::to_string(expected.size()) + " values");
        std::vector<int> values(context->stack.end() - expected.size(), context->stack.end());
        for (size_t i = 0; i < values.size(); ++i) require(type(values[i]), expected[i], loc, "stack output " + std::to_string(i + 1));
        context->stack.resize(context->stack.size() - expected.size());
        if (result == Unit) return -1;
        if (result.kind == TypeKind::Tuple) return emit(Op::MakeTuple, result, values, loc);
        return values.front();
    }
    int pop(Location loc) {
        if (context->stack.empty()) fail(loc, "stack underflow");
        int value = context->stack.back(); context->stack.pop_back(); return value;
    }
    void sameStack(const std::vector<int>& a, const std::vector<int>& b, Location loc) {
        if (a.size() != b.size()) fail(loc, "control-flow paths leave different stack heights");
        for (size_t i = 0; i < a.size(); ++i) require(type(a[i]), type(b[i]), loc, "stack slot " + std::to_string(i));
    }
    void copyStack(const std::vector<int>& from, const std::vector<int>& to, Location loc) {
        sameStack(from, to, loc);
        std::vector<int> temps;
        for (int value : from) temps.push_back(emit(Op::Move, type(value), {value}, loc));
        for (size_t i = 0; i < to.size(); ++i) {
            Instruction move{Op::Move}; move.dst = to[i]; move.args = {temps[i]}; move.location = loc; fn().code.push_back(move);
        }
    }
    std::vector<int> freshStack(const std::vector<int>& values, Location loc) {
        std::vector<int> result; for (int value : values) result.push_back(emit(Op::Move, type(value), {value}, loc)); return result;
    }
    int binary(const std::string& operation, int left, int right, Location loc) {
        if (left < 0 || right < 0) fail(loc, "operator cannot use unit values");
        Type t = type(left); require(type(right), t, loc, "operator '" + operation + "'");
        static const std::map<std::string, Op> operators = {
            {"+",Op::Add},{"-",Op::Sub},{"*",Op::Mul},{"/",Op::Div},{"%",Op::Mod},
            {"==",Op::Eq},{"=",Op::Eq},{"!=",Op::Ne},{"<",Op::Lt},{"<=",Op::Le},{">",Op::Gt},{">=",Op::Ge}
        };
        auto found = operators.find(operation);
        if (found == operators.end()) fail(loc, "unsupported operator '" + operation + "'");
        bool compare = found->second >= Op::Eq && found->second <= Op::Ge;
        if (compare) {
            if (!(t.numeric() || t == Bool || t == String || t.kind == TypeKind::Pointer)) fail(loc, "this type does not support comparison");
            if ((t == Bool || t == String || t.kind == TypeKind::Pointer) && operation != "==" && operation != "=" && operation != "!=") fail(loc, "this type supports equality comparison only");
        } else if (!t.numeric() || (operation == "%" && t != I64)) fail(loc, "arithmetic requires matching numeric operands");
        return emit(found->second, compare ? Bool : t, {left,right}, loc);
    }
    int invoke(const std::string& name, std::vector<int> arguments, Location loc, const Type* expected = nullptr) {
        if (name == "print" || name == "dump") {
            for (int value : arguments) { if (value < 0) fail(loc, "cannot print unit"); emit(Op::Print, Unit, {value}, loc); }
            return -1;
        }
        if (name == "gc") {
            if (!arguments.empty()) fail(loc, "gc expects no arguments"); emit(Op::Collect, Unit, {}, loc); return -1;
        }
        if (name == "panic") {
            if (arguments.size() != 1) fail(loc, "panic expects one string"); require(type(arguments[0]), String, loc, "panic"); emit(Op::Panic, Unit, arguments, loc); return -1;
        }
        if (name == "len") {
            if (arguments.size() != 1) fail(loc, "len expects one argument");
            Type t = type(arguments[0]); if (t.kind != TypeKind::Array && t.kind != TypeKind::Tuple && t != String) fail(loc, "len requires array, tuple, or string");
            return emit(Op::Length, I64, arguments, loc);
        }
        if (name == "i64" || name == "f64") {
            if (arguments.size() != 1 || !type(arguments[0]).numeric()) fail(loc, "numeric conversion expects one numeric argument");
            Type target = name == "i64" ? I64 : F64;
            return type(arguments[0]) == target ? arguments[0] : emit(Op::Convert, target, arguments, loc);
        }
        if (name == "ok" || name == "err") {
            if (arguments.size() != 1) fail(loc, name + " expects one argument");
            Type result = expected && expected->kind == TypeKind::Result ? *expected : Type{TypeKind::Result,{type(arguments[0]),String}};
            if (name == "err" && (!expected || expected->kind != TypeKind::Result)) fail(loc, "err needs an expected Result[T,E] type (annotate the binding or return type)");
            int yes = boolean(name == "ok",loc);
            int good = -1, bad = -1;
            auto zeroValue = [&](const Type& target) {
                if (target == I64) return integer(0,loc);
                if (target == Bool) return boolean(false,loc);
                if (target == F64) return emit(Op::ConstF64,F64,{},loc);
                if (target == String) return string("",loc);
                fail(loc,"Result placeholder currently requires scalar or string alternatives"); return -1;
            };
            if (name == "ok") { require(type(arguments[0]),result.args[0],loc,"ok value"); good=arguments[0]; bad=zeroValue(result.args[1]); }
            else { require(type(arguments[0]),result.args[1],loc,"err value"); good=zeroValue(result.args[0]); bad=arguments[0]; }
            return emit(Op::MakeTuple,result,{yes,good,bad},loc);
        }
        if (name == "is_ok" || name == "unwrap" || name == "error") {
            if (arguments.size()!=1 || type(arguments[0]).kind!=TypeKind::Result) fail(loc,name+" requires Result[T,E]");
            Type t=type(arguments[0]);
            if(name=="unwrap") return emit(Op::Unwrap,t.args[0],arguments,loc);
            return emit(Op::Index,name=="is_ok"?Bool:t.args[1],{arguments[0],integer(name=="is_ok"?0:2,loc)},loc);
        }
        Binding* local = binding(name);
        if (local) {
            Binding value = *local;
            if (value.type.kind != TypeKind::Function) fail(loc,"'"+name+"' is not callable");
            int closure = emit(Op::LoadCell,value.type,{value.cell},loc);
            return indirect(closure,std::move(arguments),loc);
        }
        if (auto* target = declaration(name,loc)) {
            auto params = target->type.args; Type result=params.back(); params.pop_back();
            if(arguments.size()!=params.size()) fail(loc,"function '"+name+"' expects "+std::to_string(params.size())+" arguments");
            for(size_t i=0;i<params.size();++i) require(type(arguments[i]),params[i],loc,"argument "+std::to_string(i+1)+" of '"+name+"'");
            int value=emit(Op::Call,result,arguments,loc); fn().code.back().text=target->irName; return value;
        }
        auto external=externals.find(name);
        if(external!=externals.end()) {
            auto& target=external->second;
            if(arguments.size()<target.parameters.size() || (!target.variadic && arguments.size()!=target.parameters.size())) fail(loc,"wrong argument count for extern '"+name+"'");
            for(size_t i=0;i<target.parameters.size();++i) require(type(arguments[i]),target.parameters[i],loc,"extern argument "+std::to_string(i+1));
            for(size_t i=target.parameters.size();i<arguments.size();++i) if(type(arguments[i])!=I64 && type(arguments[i])!=F64) fail(loc,"variadic arguments must be explicitly promoted to i64 or f64");
            int value=emit(Op::ExternalCall,target.result,arguments,loc); fn().code.back().text=name; return value;
        }
        fail(loc,"unknown function '"+name+"'");
    }
    int indirect(int closure,std::vector<int> arguments,Location loc) {
        Type callable=type(closure); if(callable.kind!=TypeKind::Function || callable.args.empty()) fail(loc,"expression is not callable");
        if(arguments.size()+1!=callable.args.size()) fail(loc,"wrong number of closure arguments");
        for(size_t i=0;i<arguments.size();++i) require(type(arguments[i]),callable.args[i],loc,"closure argument "+std::to_string(i+1));
        arguments.insert(arguments.begin(),closure); return emit(Op::CallIndirect,callable.args.back(),arguments,loc);
    }
    int closure(const Expr& expression) {
        auto captured=captures();
        std::string name="lambda_"+std::to_string(serial++);
        std::vector<Type> params; for(auto& p:expression.parameters) params.push_back(p.type);
        Type callable=Type::function(params,expression.returnType);
        compileFunction(name,expression.parameters,expression.returnType,expression.body,captured,false,expression.location);
        std::vector<int> cells; for(auto& item:captured) cells.push_back(item.second.cell);
        int value=emit(Op::MakeClosure,callable,cells,expression.location); fn().code.back().text=name; return value;
    }
    int expr(const ExprPtr& expression,const Type* expected=nullptr) {
        if(!expression) return -1;
        const auto& e=*expression; auto loc=e.location;
        switch(e.kind) {
        case Expr::Kind::Integer: return integer(e.integer,loc);
        case Expr::Kind::Float: { int r=emit(Op::ConstF64,F64,{},loc); fn().code.back().real=e.real; return r; }
        case Expr::Kind::Boolean: return boolean(e.integer!=0 || e.text=="true",loc);
        case Expr::Kind::String: return string(e.text,loc);
        case Expr::Kind::Name: {
            if(auto* local=binding(e.text)) { Binding v=*local; return emit(Op::LoadCell,v.type,{v.cell},loc); }
            if(auto* target=declaration(e.text,loc)) { int r=emit(Op::MakeClosure,target->type,{},loc); fn().code.back().text=target->irName; return r; }
            fail(loc,"unknown name '"+e.text+"'");
        }
        case Expr::Kind::Unary: {
            int value=expr(e.items.at(0));
            if(e.text=="not" || e.text=="!") { require(type(value),Bool,loc,"not"); return emit(Op::Not,Bool,{value},loc); }
            if(!type(value).numeric()) fail(loc,"unary arithmetic requires numeric value");
            return e.text=="+"?value:emit(Op::Neg,type(value),{value},loc);
        }
        case Expr::Kind::Binary: {
            int left=expr(e.items.at(0));
            if(e.text=="|>") {
                auto right=e.items.at(1); std::vector<int> arguments{left};
                ExprPtr callee=right;
                if(right->kind==Expr::Kind::Call) callee=right->items.at(0);
                int callable=-1;
                if(callee->kind!=Expr::Kind::Name)callable=expr(callee);
                else if(auto* local=binding(callee->text)){Binding b=*local;callable=emit(Op::LoadCell,b.type,{b.cell},loc);}
                if(right->kind==Expr::Kind::Call)for(size_t i=1;i<right->items.size();++i)arguments.push_back(expr(right->items[i]));
                if(callable>=0)return indirect(callable,arguments,loc);
                return invoke(callee->text,arguments,loc,expected);
            }
            if(e.text=="and" || e.text=="or" || e.text=="&&" || e.text=="||") {
                require(type(left),Bool,loc,"boolean operation");
                auto carry=freshStack(context->stack,loc);context->stack=carry;
                int result=emit(Op::Move,Bool,{left},loc);
                bool conjunction=e.text=="and" || e.text=="&&";
                int condition=conjunction?left:emit(Op::Not,Bool,{left},loc);
                size_t skip=jump(Op::JumpFalse,condition,loc);
                int right=expr(e.items[1]); require(type(right),Bool,loc,"boolean operation");
                copyStack(context->stack,carry,loc);context->stack=carry;
                Instruction move{Op::Move}; move.dst=result; move.args={right}; move.location=loc; fn().code.push_back(move);
                patch(skip,position()); return result;
            }
            return binary(e.text,left,expr(e.items.at(1)),loc);
        }
        case Expr::Kind::Call: {
            auto callee=e.items.at(0); std::vector<int> arguments;
            if(callee->kind==Expr::Kind::Name) {
                if(auto* local=binding(callee->text)) {
                    Binding b=*local;int callable=emit(Op::LoadCell,b.type,{b.cell},loc);
                    for(size_t i=1;i<e.items.size();++i)arguments.push_back(expr(e.items[i]));
                    return indirect(callable,arguments,loc);
                }
                for(size_t i=1;i<e.items.size();++i) arguments.push_back(expr(e.items[i]));
                return invoke(callee->text,arguments,loc,expected);
            }
            int callable=expr(callee); for(size_t i=1;i<e.items.size();++i) arguments.push_back(expr(e.items[i]));
            return indirect(callable,arguments,loc);
        }
        case Expr::Kind::Array: {
            std::vector<int> values; Type element;
            if(expected && expected->kind==TypeKind::Array) element=expected->args.at(0);
            for(auto& item:e.items) { int value=expr(item); if(value<0)fail(item->location,"array cannot contain unit");if(values.empty() && element==Unit) element=type(value); require(type(value),element,item->location,"array element"); values.push_back(value); }
            if(element==Unit) fail(loc,"empty array requires an explicit element type");
            return emit(Op::MakeArray,Type::array(element),values,loc);
        }
        case Expr::Kind::Tuple: {
            std::vector<int> values; std::vector<Type> types;
            for(auto& item:e.items) { int value=expr(item); if(value<0) fail(loc,"tuple cannot contain unit"); values.push_back(value); types.push_back(type(value)); }
            return emit(Op::MakeTuple,Type{TypeKind::Tuple,types},values,loc);
        }
        case Expr::Kind::Index: {
            int base=expr(e.items[0]); int index=expr(e.items[1]); require(type(index),I64,loc,"index"); Type t=type(base), result;
            if(t.kind==TypeKind::Array) result=t.args.at(0);
            else if(t==String) result=I64;
            else if(t.kind==TypeKind::Tuple) {
                if(e.items[1]->kind!=Expr::Kind::Integer) fail(loc,"tuple index must be an integer literal");
                int64_t i=e.items[1]->integer; if(i<0 || static_cast<size_t>(i)>=t.args.size()) fail(loc,"tuple index out of bounds"); result=t.args[i];
            } else fail(loc,"indexing requires array, tuple, or string");
            return emit(Op::Index,result,{base,index},loc);
        }
        case Expr::Kind::Lambda: return closure(e);
        case Expr::Kind::Stack: {
            for(auto& item:e.items) { int value=expr(item); if(value<0) fail(loc,"cannot push unit onto stack"); context->stack.push_back(value); }
            bool previous=context->stackMode;context->stackMode=true;
            block(e.body,false);context->stackMode=previous;return takeOutputs(e.returnType,loc);
        }
        }
        fail(loc,"unsupported expression");
    }
    void stackOperation(const std::string& name,Location loc) {
        if(name=="dup") { int a=pop(loc); context->stack.push_back(a); context->stack.push_back(a); }
        else if(name=="swap") { int b=pop(loc),a=pop(loc); context->stack.push_back(b);context->stack.push_back(a); }
        else if(name=="drop") { pop(loc); }
        else if(name=="over") { int b=pop(loc),a=pop(loc); context->stack.push_back(a);context->stack.push_back(b);context->stack.push_back(a); }
        else if(name=="rot") { int c=pop(loc),b=pop(loc),a=pop(loc); context->stack.push_back(b);context->stack.push_back(c);context->stack.push_back(a); }
        else if(name=="print" || name=="dump") emit(Op::Print,Unit,{pop(loc)},loc);
        else if(name=="gc") emit(Op::Collect,Unit,{},loc);
        else if(name=="and" || name=="or") {
            int b=pop(loc),a=pop(loc);require(type(a),Bool,loc,name);require(type(b),Bool,loc,name);
            int result=emit(Op::Move,Bool,{a},loc);int condition=name=="and"?a:emit(Op::Not,Bool,{a},loc);
            size_t skip=jump(Op::JumpFalse,condition,loc);Instruction move{Op::Move};move.dst=result;move.args={b};move.location=loc;fn().code.push_back(move);patch(skip,position());context->stack.push_back(result);
        }
        else if(name=="len") { int value=pop(loc); context->stack.push_back(invoke("len",{value},loc)); }
        else if(name=="neg" || name=="not") {
            int a=pop(loc); if(name=="not") require(type(a),Bool,loc,"not"); else if(!type(a).numeric()) fail(loc,"neg requires numeric value");
            context->stack.push_back(emit(name=="not"?Op::Not:Op::Neg,type(a),{a},loc));
        } else {
            auto* target=declaration(name,loc);
            if(target && target->ast->kind==Stmt::Kind::Word) {
                size_t count=target->ast->parameters.size(); if(context->stack.size()<count) fail(loc,"stack underflow calling word '"+name+"'");
                std::vector<int> args(context->stack.end()-count,context->stack.end()); context->stack.resize(context->stack.size()-count);
                int value=invoke(name,args,loc);
                if(target->ast->returnType.kind==TypeKind::Tuple) for(size_t i=0;i<target->ast->returnType.args.size();++i) context->stack.push_back(emit(Op::Index,target->ast->returnType.args[i],{value,integer(static_cast<int64_t>(i),loc)},loc));
                else if(value>=0) context->stack.push_back(value);
            } else {
                int b=pop(loc),a=pop(loc); context->stack.push_back(binary(name,a,b,loc));
            }
        }
    }
    void conditional(const Stmt& s) {
        int condition=s.expression?expr(s.expression):pop(s.location); require(type(condition),Bool,s.location,"if condition");
        auto initial=context->stack; size_t otherwise=jump(Op::JumpFalse,condition,s.location);
        block(s.body,true); auto left=context->stack; bool leftReturns=returns(s.body);
        std::vector<int> joined; if(!leftReturns) joined=freshStack(left,s.location);
        size_t finish=leftReturns?static_cast<size_t>(-1):jump(Op::Jump,-1,s.location);
        patch(otherwise,position()); context->stack=initial; block(s.elseBody,true); bool rightReturns=returns(s.elseBody);
        if(!leftReturns && !rightReturns) copyStack(context->stack,joined,s.location);
        else if(leftReturns && !rightReturns) joined=context->stack;
        if(finish!=static_cast<size_t>(-1))patch(finish,position()); context->stack=joined;
    }
    void whileStatement(const Stmt& s) {
        if(!s.elseBody.empty())fail(s.location,"while/else is not part of the implemented language");
        auto carry=freshStack(context->stack,s.location); context->stack=carry;
        int head=position(); int condition=s.expression?expr(s.expression):pop(s.location); require(type(condition),Bool,s.location,"while condition");
        auto exitStack=carry;if(!s.expression)exitStack.pop_back();
        sameStack(context->stack,exitStack,s.location);
        size_t finish=jump(Op::JumpFalse,condition,s.location);
        context->loops.push_back({head,carry,{},exitStack}); block(s.body,true);
        if(!returns(s.body)) { copyStack(context->stack,carry,s.location); size_t back=jump(Op::Jump,-1,s.location); patch(back,head); }
        auto loop=context->loops.back(); context->loops.pop_back();
        int end=position(); patch(finish,end); for(auto index:loop.breaks) patch(index,end); context->stack=exitStack;
    }
    void forStatement(const Stmt& s) {
        if(!s.elseBody.empty())fail(s.location,"for/else is not part of the implemented language");
        bool range=s.expression && s.expression->kind==Expr::Kind::Call && !s.expression->items.empty() && s.expression->items[0]->kind==Expr::Kind::Name && s.expression->items[0]->text=="range";
        int start=integer(0,s.location),end=-1,step=integer(1,s.location),sequence=-1;
        Type element=I64;
        if(range) {
            auto& args=s.expression->items; if(args.size()<2 || args.size()>4) fail(s.location,"range expects end, start/end, or start/end/step");
            std::vector<int> values; for(size_t i=1;i<args.size();++i) { int value=expr(args[i]); require(type(value),I64,args[i]->location,"range argument"); values.push_back(value); }
            if(values.size()==1) end=values[0]; else {start=values[0];end=values[1];} if(values.size()==3) step=values[2];
            int nonzero=binary("!=",step,integer(0,s.location),s.location); size_t bad=jump(Op::JumpFalse,nonzero,s.location); size_t good=jump(Op::Jump,-1,s.location);
            patch(bad,position()); emit(Op::Panic,Unit,{string("range step cannot be zero",s.location)},s.location); patch(good,position());
        } else {
            sequence=expr(s.expression); Type t=type(sequence);
            if(t.kind==TypeKind::Array) element=t.args[0]; else if(t==String) element=I64; else fail(s.location,"for requires an array, string, or range(...)");
            end=emit(Op::Length,I64,{sequence},s.location);
        }
        int cursor=emit(Op::Move,I64,{start},s.location);
        auto carry=freshStack(context->stack,s.location); context->stack=carry;
        int head=position(); int condition=-1;
        if(range) {
            int positive=binary(">",step,integer(0,s.location),s.location);
            size_t negative=jump(Op::JumpFalse,positive,s.location);
            condition=emit(Op::Move,Bool,{binary("<",cursor,end,s.location)},s.location);
            size_t join=jump(Op::Jump,-1,s.location); patch(negative,position());
            int other=binary(">",cursor,end,s.location); Instruction move{Op::Move};move.dst=condition;move.args={other};move.location=s.location;fn().code.push_back(move);patch(join,position());
        } else condition=binary("<",cursor,end,s.location);
        size_t finish=jump(Op::JumpFalse,condition,s.location);
        context->scopes.emplace_back(); int value=range?cursor:emit(Op::Index,element,{sequence,cursor},s.location); define(s.name,value,false,s.location);
        // Continue targets the increment, patched after the body is compiled.
        context->loops.push_back({-1,carry,{},carry});
        block(s.body,false); auto loop=context->loops.back();context->loops.pop_back();context->scopes.pop_back();
        int increment=position();
        for(size_t i=head;i<fn().code.size();++i) if(fn().code[i].op==Op::Jump && fn().code[i].target==-2) patch(i,increment);
        if(!returns(s.body)) copyStack(context->stack,carry,s.location);
        int next=binary("+",cursor,step,s.location); Instruction move{Op::Move};move.dst=cursor;move.args={next};move.location=s.location;fn().code.push_back(move);
        size_t back=jump(Op::Jump,-1,s.location);patch(back,head);
        int done=position();patch(finish,done);for(auto index:loop.breaks)patch(index,done);context->stack=carry;
    }
    void statement(const StmtPtr& statementValue) {
        const auto& s=*statementValue;
        switch(s.kind) {
        case Stmt::Kind::Let: case Stmt::Kind::Var: {
            int value=expr(s.expression,s.annotated?&s.type:nullptr);
            if(s.annotated) { if(value<0) fail(s.location,"cannot bind unit"); require(type(value),s.type,s.location,"initializer"); }
            if(s.target && s.target->kind==Expr::Kind::Tuple) {
                if(type(value).kind!=TypeKind::Tuple || type(value).args.size()!=s.target->items.size()) fail(s.location,"tuple destructuring shape mismatch");
                for(size_t i=0;i<s.target->items.size();++i) { auto& item=s.target->items[i]; if(item->kind!=Expr::Kind::Name)fail(item->location,"destructuring requires names"); int part=emit(Op::Index,type(value).args[i],{value,integer(static_cast<int64_t>(i),s.location)},s.location);define(item->text,part,s.kind==Stmt::Kind::Var,item->location); }
            } else define(s.name,value,s.kind==Stmt::Kind::Var,s.location);
            break;
        }
        case Stmt::Kind::Assign: {
            auto target=s.target;
            if(!target && !s.name.empty()) { target=std::make_shared<Expr>(Expr::Kind::Name,s.location);target->text=s.name; }
            if(!target) fail(s.location,"missing assignment target");
            if(target->kind==Expr::Kind::Name) {
                auto* found=binding(target->text);if(!found)fail(s.location,"unknown assignment target '"+target->text+"'");Binding b=*found;
                if(!b.mutableValue)fail(s.location,"cannot assign immutable binding '"+target->text+"'; use var");
                int value=expr(s.expression,&b.type);require(type(value),b.type,s.location,"assignment");emit(Op::StoreCell,Unit,{b.cell,value},s.location);
            } else if(target->kind==Expr::Kind::Index) {
                int base=expr(target->items[0]),index=expr(target->items[1]);require(type(index),I64,s.location,"index");
                if(type(base).kind!=TypeKind::Array)fail(s.location,"indexed assignment requires an array");
                Type element=type(base).args[0];int value=expr(s.expression,&element);require(type(value),element,s.location,"array assignment");emit(Op::SetIndex,Unit,{base,index,value},s.location);
            } else fail(s.location,"invalid assignment target");
            break;
        }
        case Stmt::Kind::Expression: {
            if(context->stackMode && s.expression && s.expression->kind==Expr::Kind::Name) {
                auto* word=declaration(s.expression->text,s.location);
                if(word && word->ast->kind==Stmt::Kind::Word){stackOperation(s.expression->text,s.location);break;}
            }
            int value=expr(s.expression);if(context->stackMode && value>=0)context->stack.push_back(value);break;
        }
        case Stmt::Kind::StackOp: stackOperation(s.name,s.location); break;
        case Stmt::Kind::Return: {
            Type result=fn().result;int value=expr(s.expression,&result);
            if(value<0)require(Unit,result,s.location,"return");else require(type(value),result,s.location,"return");
            if(!context->stack.empty())fail(s.location,"structured function return leaves values on the shared operand stack");
            emit(Op::Return,Unit,value<0?std::vector<int>{}:std::vector<int>{value},s.location);break;
        }
        case Stmt::Kind::If: conditional(s);break;
        case Stmt::Kind::While:whileStatement(s);break;
        case Stmt::Kind::For:forStatement(s);break;
        case Stmt::Kind::Break: case Stmt::Kind::Continue: {
            if(context->loops.empty())fail(s.location,"loop control outside a loop");
            auto& loop=context->loops.back();copyStack(context->stack,s.kind==Stmt::Kind::Break?loop.exit:loop.carry,s.location);
            size_t branch=jump(Op::Jump,-1,s.location);
            if(s.kind==Stmt::Kind::Break)loop.breaks.push_back(branch);else patch(branch,loop.head<0?-2:loop.head);
            break;
        }
        case Stmt::Kind::Function: {
            Expr lambda(Expr::Kind::Lambda,s.location);lambda.parameters=s.parameters;lambda.returnType=s.returnType;lambda.body=s.body;
            int value=closure(lambda);define(s.name,value,false,s.location);break;
        }
        case Stmt::Kind::Word:fail(s.location,"word declarations must be at module scope");
        case Stmt::Kind::Extern:case Stmt::Kind::Import:fail(s.location,"imports and extern declarations must be at module scope");
        case Stmt::Kind::Match: {
            int subject=expr(s.expression);auto initial=context->stack;std::vector<size_t> exits;std::vector<int> joined;bool haveJoin=false,wildcard=false;
            for(auto& arm:s.body) {
                context->stack=initial;size_t next=0;bool isWildcard=arm->name=="_";
                if(wildcard)fail(arm->location,"unreachable case after wildcard");
                if(!isWildcard){int value=expr(arm->expression);next=jump(Op::JumpFalse,binary("==",subject,value,arm->location),arm->location);}else wildcard=true;
                block(arm->body,true);
                if(!returns(arm->body)){if(!haveJoin){joined=freshStack(context->stack,arm->location);haveJoin=true;}else copyStack(context->stack,joined,arm->location);exits.push_back(jump(Op::Jump,-1,arm->location));}
                if(!isWildcard)patch(next,position());
            }
            if(!wildcard) {if(haveJoin)copyStack(initial,joined,s.location);else joined=initial;}
            int done=position();for(auto exit:exits)patch(exit,done);context->stack=joined;break;
        }
        }
    }
    void block(const std::vector<StmtPtr>& body,bool scope) {
        if(scope)context->scopes.emplace_back();
        for(auto& item:body){statement(item);if(item->kind==Stmt::Kind::Return || item->kind==Stmt::Kind::Break || item->kind==Stmt::Kind::Continue)break;}
        if(scope)context->scopes.pop_back();
    }
    void compileFunction(const std::string& name,const std::vector<Parameter>& parameters,const Type& result,const std::vector<StmtPtr>& body,const std::vector<std::pair<std::string,Binding>>& captured,bool word,Location loc) {
        auto previous=std::move(context);size_t index=program.functions.size();
        Function function;function.name=name;function.result=result;function.location=loc;
        for(auto& parameter:parameters){function.parameters.push_back(parameter.type);function.parameterNames.push_back(parameter.name);function.registers.push_back(parameter.type);}
        program.functions.push_back(std::move(function));context=std::make_unique<Context>();context->function=index;context->scopes.emplace_back();
        context->stackMode=word;
        for(size_t i=0;i<captured.size();++i){auto& item=captured[i];int value=emit(Op::Capture,Type::cell(item.second.type),{},loc);fn().code.back().integer=static_cast<int64_t>(i);context->scopes.back()[item.first]={value,item.second.type,item.second.mutableValue};}
        context->scopes.emplace_back();
        for(size_t i=0;i<parameters.size();++i)define(parameters[i].name,static_cast<int>(i),false,parameters[i].location);
        if(word)for(size_t i=0;i<parameters.size();++i)context->stack.push_back(static_cast<int>(i));
        block(body,false);
        if(word){int value=takeOutputs(result,loc);if(!context->stack.empty())fail(loc,"word leaves undeclared stack outputs");emit(Op::Return,Unit,value<0?std::vector<int>{}:std::vector<int>{value},loc);}
        else if(!returns(body)) {
            if(result!=Unit)fail(loc,"function '"+name+"' does not return a value on every path");
            if(!context->stack.empty())fail(loc,"function leaves values on shared operand stack");
            emit(Op::Return,Unit,{},loc);
        }
        context=std::move(previous);
    }
public:
    Program run(const SyntaxTree& tree) {
        program.sourceFile=tree.filename;
        for(auto& statement:tree.statements) {
            auto& s=*statement;
            if(s.kind==Stmt::Kind::Import){imports[s.location.file].insert(s.name);continue;}
            if(s.kind==Stmt::Kind::Extern){External ext;ext.name=s.name;ext.result=s.returnType;ext.variadic=s.variadic;for(auto& p:s.parameters)ext.parameters.push_back(p.type);if(externals.count(ext.name))fail(s.location,"duplicate extern declaration");externals[ext.name]=ext;program.externals.push_back(ext);continue;}
            if(s.kind!=Stmt::Kind::Function && s.kind!=Stmt::Kind::Word)fail(s.location,"module scope accepts functions, words, imports and extern declarations");
            std::string key=s.location.file+"::"+s.name;if(byModule.count(key))fail(s.location,"duplicate function or word '"+s.name+"'");
            std::vector<Type> params;for(auto& parameter:s.parameters)params.push_back(parameter.type);
            std::string irName="m"+std::to_string(declarations.size())+"_"+s.name;
            if(s.name=="main" && s.location.file==tree.filename){irName="main";if(!params.empty() || (s.returnType!=Unit && s.returnType!=I64))fail(s.location,"main must take no parameters and return unit or i64");}
            size_t index=declarations.size();byModule[key]=index;if(s.exported)exported[s.name].push_back(index);
            declarations.push_back({statement,irName,s.location.file,Type::function(params,s.returnType)});
        }
        if(!byModule.count(tree.filename+"::main"))fail({tree.filename,1,1},"program requires fun main() -> unit or i64");
        for(auto& d:declarations)compileFunction(d.irName,d.ast->parameters,d.ast->returnType,d.ast->body,{},d.ast->kind==Stmt::Kind::Word,d.ast->location);
        verify(program);return std::move(program);
    }
};
}
Program compile(const SyntaxTree& tree) { return Compiler().run(tree); }
}
