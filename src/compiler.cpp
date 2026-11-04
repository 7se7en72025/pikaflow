#include "pikaflow/ir.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace pika {
namespace {
struct Binding {
    int cell;
    Type type;
    bool mutableValue;
};
struct Declaration {
    StmtPtr ast;
    std::string irName;
    std::string file;
    Type type;
};
struct Loop {
    int head;
    std::vector<int> carry;
    std::vector<size_t> breaks;
    std::vector<int> exit;
};
struct Context {
    size_t function;
    std::vector<std::map<std::string, Binding>> scopes;
    std::vector<int> stack;
    std::vector<Loop> loops;
    bool stackMode = false;
    bool unsafeMode = false;
};
class Compiler {
    struct EnumInstance {
        StmtPtr declaration;
        std::vector<Type> arguments;
        std::vector<size_t> offsets;
        Type type;
    };
    Program program;
    std::vector<Declaration> declarations;
    std::map<std::string, size_t> byModule;
    std::map<std::string, std::vector<size_t>> exported;
    std::map<std::string, External> externals;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> exportedExternals;
    std::map<std::string, std::set<std::string>> imports;
    std::map<std::string, std::map<std::string, std::string>> moduleAliases;
    std::set<std::string> exportedExternalKeys;
    bool indexSymbols = false;
    std::unique_ptr<Context> context;
    unsigned serial = 0;
    std::map<std::string, Declaration> specializations;
    unsigned instantiationDepth = 0;
    size_t typeExpansionBudget = 1000000;
    size_t typeNodes(const Type &type, Location loc) {
        size_t count = 0;
        std::vector<const Type *> pending{&type};
        while (!pending.empty()) {
            const Type *next = pending.back(); pending.pop_back();
            if (next->name.size() > 16384) fail(loc, "nominal type name size limit exceeded");
            if (++count > 4096) fail(loc, "type complexity limit (4096 nodes) exceeded");
            for (const auto &argument : next->args) pending.push_back(&argument);
        }
        return count;
    }
    std::map<std::string, StmtPtr> enumDeclarations;
    std::map<std::string, std::vector<std::string>> exportedEnums;
    std::map<std::string, std::pair<std::string, size_t>> constructors;
    std::map<std::string, std::vector<std::string>> exportedConstructors;
    std::map<std::string, EnumInstance> enumInstances;

    std::pair<std::string, std::string> qualifiedModule(const std::string &name, Location loc) {
        auto dot = name.find('.');
        if (dot == std::string::npos) return {};
        if (name.find('.', dot + 1) != std::string::npos) fail(loc, "module qualification requires alias.member");
        std::string alias = name.substr(0, dot);
        auto found = moduleAliases[loc.file].find(alias);
        if (found == moduleAliases[loc.file].end()) fail(loc, "unknown module alias '" + alias + "'");
        return {found->second, name.substr(dot + 1)};
    }

    StmtPtr enumDeclaration(const std::string &name, Location loc) {
        auto qualified = qualifiedModule(name, loc);
        if (!qualified.first.empty()) {
            auto found = enumDeclarations.find(qualified.first + "::" + qualified.second);
            if (found == enumDeclarations.end()) return {};
            if (!found->second->exported) fail(loc, "module type '" + name + "' is private");
            return found->second;
        }
        auto own = enumDeclarations.find(loc.file + "::" + name);
        if (own != enumDeclarations.end()) return own->second;
        StmtPtr result;
        for (const auto &key : exportedEnums[name]) {
            auto candidate = enumDeclarations.at(key);
            if (imports[loc.file].count(candidate->location.file)) {
                if (result) fail(loc, "ambiguous imported enum type '" + name + "'");
                result = candidate;
            }
        }
        return result;
    }
    std::pair<StmtPtr, size_t> constructor(const std::string &name, Location loc) {
        auto qualified = qualifiedModule(name, loc);
        if (!qualified.first.empty()) {
            auto found = constructors.find(qualified.first + "::" + qualified.second);
            if (found == constructors.end()) return {};
            auto definition = enumDeclarations.at(found->second.first);
            if (!definition->exported) fail(loc, "module constructor '" + name + "' is private");
            return {definition, found->second.second};
        }
        const auto localKey = loc.file + "::" + name;
        auto own = constructors.find(localKey);
        if (own != constructors.end()) return {enumDeclarations.at(own->second.first), own->second.second};
        if (byModule.count(localKey) || externals.count(localKey)) return {};
        std::pair<StmtPtr, size_t> result;
        for (const auto &key : exportedConstructors[name]) {
            auto candidate = constructors.at(key);
            auto definition = enumDeclarations.at(candidate.first);
            if (imports[loc.file].count(definition->location.file)) {
                if (result.first) fail(loc, "ambiguous imported constructor '" + name + "'");
                result = {definition, candidate.second};
            }
        }
        if (result.first) {
            for (size_t index : exported[name])
                if (imports[loc.file].count(declarations[index].file)) fail(loc, "ambiguous imported callable '" + name + "'");
            for (const auto &item : exportedExternals[name])
                if (imports[loc.file].count(item.first)) fail(loc, "ambiguous imported callable '" + name + "'");
        }
        return result;
    }
    Type enumType(const StmtPtr &definition, const std::vector<Type> &arguments, Location loc, unsigned depth) {
        if (arguments.size() != definition->typeParameters.size()) fail(loc, "wrong number of enum type arguments");
        std::string key = definition->location.file + "::" + definition->name + "[";
        std::map<std::string, Type> bindings;
        for (size_t i = 0; i < arguments.size(); ++i) {
            validateType(arguments[i], loc);
            if (i) key += ",";
            key += arguments[i].str(); bindings[definition->typeParameters[i]] = arguments[i];
            if (key.size() > 16384) fail(loc, "nominal type name size limit exceeded");
        }
        key += "]";
        auto cached = enumInstances.find(key);
        if (cached != enumInstances.end()) return cached->second.type;
        if (enumInstances.size() >= 256) fail(loc, "enum specialization limit (256) exceeded");
        if (depth > 64) fail(loc, "enum layout expansion limit exceeded");
        Type reference{TypeKind::Tuple, {}, key};
        enumInstances.emplace(key, EnumInstance{definition, arguments, {}, reference});
        Type layout{TypeKind::Tuple, {I64}, key};
        auto &result = enumInstances.at(key);
        for (auto &variant : definition->body) {
            result.offsets.push_back(layout.args.size());
            for (auto &field : variant->parameters) {
                Type concrete = substitute(field.type, bindings, field.location, depth + 1);
                validateType(concrete, field.location);
                layout.args.push_back(std::move(concrete));
            }
        }
        program.nominalLayouts[key] = std::move(layout);
        return reference;
    }

    bool unresolved(const Type &t) {
        if (t.kind == TypeKind::Named) return true;
        for (const auto &argument : t.args) if (unresolved(argument)) return true;
        return false;
    }
    Type substitute(Type t, const std::map<std::string, Type> &bindings, Location loc,
                    unsigned depth = 0) {
        if (!typeExpansionBudget--) fail(loc, "compiler type expansion budget exceeded");
        if (depth > 64) fail(loc, "type expansion exceeds the supported limit");
        if (t.kind == TypeKind::Named) {
            auto found = bindings.find(t.name);
            if (found != bindings.end() && t.args.empty()) {
                size_t nodes = typeNodes(found->second, loc);
                if (nodes > typeExpansionBudget) fail(loc, "compiler type expansion budget exceeded");
                typeExpansionBudget -= nodes;
                return found->second;
            }
            auto definition = enumDeclaration(t.name, loc);
            if (definition) {
                std::vector<Type> arguments;
                for (const auto &argument : t.args) arguments.push_back(substitute(argument, bindings, loc, depth + 1));
                return enumType(definition, arguments, loc, depth + 1);
            }
            fail(loc, "unknown type '" + t.str() + "'");
        }
        for (auto &argument : t.args) argument = substitute(argument, bindings, loc, depth + 1);
        return t;
    }
    Type partialType(Type t, const std::map<std::string, Type> &bindings, Location loc) {
        if (t.kind == TypeKind::Named && t.args.empty() && bindings.count(t.name)) return bindings.at(t.name);
        for (auto &argument : t.args) argument = partialType(argument, bindings, loc);
        if (t.kind == TypeKind::Named && enumDeclaration(t.name, loc)) {
            bool concrete = true;
            for (const auto &argument : t.args) concrete = concrete && !unresolved(argument);
            if (concrete) return substitute(t, bindings, loc);
        }
        return t;
    }
    ExprPtr cloneExpr(const ExprPtr &original, const std::map<std::string, Type> &bindings) {
        if (!original) return {};
        auto result = std::make_shared<Expr>(*original);
        result->returnType = substitute(result->returnType, bindings, result->location);
        for (auto &argument : result->typeArguments) argument = substitute(argument, bindings, result->location);
        for (auto &parameter : result->parameters) parameter.type = substitute(parameter.type, bindings, parameter.location);
        result->items.clear(); result->body.clear();
        for (auto &item : original->items) result->items.push_back(cloneExpr(item, bindings));
        for (auto &statement : original->body) result->body.push_back(cloneStmt(statement, bindings));
        return result;
    }
    StmtPtr cloneStmt(const StmtPtr &original, const std::map<std::string, Type> &bindings) {
        auto result = std::make_shared<Stmt>(*original);
        result->type = substitute(result->type, bindings, result->location);
        result->returnType = substitute(result->returnType, bindings, result->location);
        for (auto &parameter : result->parameters) parameter.type = substitute(parameter.type, bindings, parameter.location);
        result->expression = cloneExpr(original->expression, bindings);
        result->target = cloneExpr(original->target, bindings);
        result->other = cloneExpr(original->other, bindings);
        result->body.clear(); result->elseBody.clear();
        for (auto &item : original->body) result->body.push_back(cloneStmt(item, bindings));
        for (auto &item : original->elseBody) result->elseBody.push_back(cloneStmt(item, bindings));
        return result;
    }
    void infer(const Type &pattern, const Type &actual, const std::set<std::string> &variables,
               std::map<std::string, Type> &bindings, Location loc) {
        if (pattern.kind == TypeKind::Named && pattern.args.empty() && variables.count(pattern.name)) {
            auto found = bindings.find(pattern.name);
            if (found != bindings.end()) require(actual, found->second, loc, "type parameter '" + pattern.name + "'");
            else bindings[pattern.name] = actual;
            return;
        }
        if (pattern.kind == TypeKind::Named && !actual.name.empty() && enumInstances.count(actual.name)) {
            const auto &instance = enumInstances.at(actual.name);
            auto definition = enumDeclaration(pattern.name, loc);
            if (!definition || definition != instance.declaration || pattern.args.size() != instance.arguments.size())
                fail(loc, "generic enum argument does not match " + pattern.str());
            for (size_t i = 0; i < pattern.args.size(); ++i) infer(pattern.args[i], instance.arguments[i], variables, bindings, loc);
            return;
        }
        if (pattern.kind != actual.kind || pattern.name != actual.name || pattern.args.size() != actual.args.size())
            fail(loc, "generic argument does not match " + pattern.str() + ": received " + actual.str());
        for (size_t i = 0; i < pattern.args.size(); ++i) infer(pattern.args[i], actual.args[i], variables, bindings, loc);
    }
    Declaration specialize(const Declaration &generic, const std::vector<Type> &explicitTypes,
                           const std::vector<int> *arguments, const Type *expected, Location loc) {
        auto definition = generic.ast;
        const auto variables = std::set<std::string>(definition->typeParameters.begin(), definition->typeParameters.end());
        std::map<std::string, Type> bindings;
        if (!explicitTypes.empty()) {
            if (explicitTypes.size() != definition->typeParameters.size()) fail(loc, "wrong number of generic type arguments");
            for (size_t i = 0; i < explicitTypes.size(); ++i) bindings[definition->typeParameters[i]] = explicitTypes[i];
        }
        if (arguments) {
            if (arguments->size() != definition->parameters.size()) fail(loc, "wrong number of generic function arguments");
            for (size_t i = 0; i < arguments->size(); ++i) infer(definition->parameters[i].type, type((*arguments)[i]), variables, bindings, definition->parameters[i].location);
        }
        if (expected) infer(definition->returnType, *expected, variables, bindings, definition->location);
        std::string key = generic.irName;
        for (auto &variable : definition->typeParameters) {
            if (!bindings.count(variable)) fail(loc, "cannot infer type parameter '" + variable + "'; use explicit ::[types]");
            validateType(bindings.at(variable), loc);
            key += "[" + bindings.at(variable).str() + "]";
            if (key.size() > 65536) fail(loc, "generic specialization key size limit exceeded");
        }
        auto cached = specializations.find(key);
        if (cached != specializations.end()) return cached->second;
        if (specializations.size() >= 256) fail(loc, "generic specialization limit (256) exceeded");
        if (instantiationDepth >= 64) fail(loc, "generic instantiation depth limit (64) exceeded");
        auto concrete = cloneStmt(definition, bindings);
        concrete->typeParameters.clear();
        std::vector<Type> parameters;
        for (const auto &parameter : concrete->parameters) parameters.push_back(parameter.type);
        Declaration result{concrete, "generic_" + std::to_string(specializations.size()) + "_" + definition->name,
                           generic.file, Type::function(parameters, concrete->returnType)};
        specializations.emplace(key, result);
        ++instantiationDepth;
        compileFunction(result.irName, concrete->parameters, concrete->returnType, concrete->body, {},
                        concrete->kind == Stmt::Kind::Word, concrete->location);
        --instantiationDepth;
        return result;
    }

    Function &fn() {
        return program.functions[context->function];
    }
    const Type &type(int reg) {
        return reg < 0 ? Unit : fn().registers.at(reg);
    }
    [[noreturn]] void fail(const Location &loc, const std::string &message) {
        throw Error(loc, message);
    }
    int reg(Type t) {
        fn().registers.push_back(std::move(t));
        return static_cast<int>(fn().registers.size() - 1);
    }
    int emit(Op op, Type t, std::vector<int> args = {}, Location loc = {}) {
        Instruction instruction{op};
        instruction.dst = t == Unit ? -1 : reg(std::move(t));
        instruction.args = std::move(args);
        instruction.location = std::move(loc);
        int result = instruction.dst;
        fn().code.push_back(std::move(instruction));
        return result;
    }
    bool reserved(const std::string &name) {
        static const std::set<std::string> names = {
            "print",  "dump",     "gc",     "panic",     "len",       "i64",        "f64",
            "bool",   "unit",     "str",    "string",    "Result",    "ptr",        "stack",
            "fun",    "fn",       "word",   "let",       "var",       "if",         "elif",
            "else",   "while",    "for",    "in",        "match",     "case",       "return",
            "break",  "continue", "unsafe", "export",    "extern",    "import",     "pass",
            "true",   "false",    "and",    "or",        "not",       "dup",        "swap",
            "drop",   "over",     "rot",    "neg",       "ok",        "err",        "is_ok",
            "unwrap", "error",    "range",  "alloc_i64", "alloc_f64", "alloc_bool", "free",
            "load",   "store",    "cstr",   "pin",       "unpin", "enum", "task", "spawn", "join", "c_callback", "offset"};
        return names.count(name) != 0;
    }
    bool cScalar(const Type &t) {
        return t == I64 || t == F64 || t == Bool || t.kind == TypeKind::Pointer;
    }
    bool cType(const Type &t) {
        if (!t.name.empty()) return false;
        if (cScalar(t))
            return true;
        if (t.kind != TypeKind::Tuple || t.args.empty())
            return false;
        for (const auto &field : t.args)
            if (!cType(field))
                return false;
        return true;
    }
    std::pair<Type, CABIType> externType(const Type &source, Location loc) {
        if (source.kind == TypeKind::Named) {
            static const std::map<std::string, std::pair<unsigned, bool>> widths = {
                {"c_i8", {8, true}}, {"c_u8", {8, false}}, {"c_i16", {16, true}},
                {"c_u16", {16, false}}, {"c_i32", {32, true}}, {"c_u32", {32, false}},
                {"c_i64", {64, true}}, {"c_u64", {64, false}}};
            auto width = widths.find(source.name);
            if (width != widths.end()) {
                if (!source.args.empty()) fail(loc, "C integer width annotations do not take type arguments");
                return {I64, CABIType{width->second.first, width->second.second, {}}};
            }
        }
        if (source.kind == TypeKind::Tuple && source.name.empty()) {
            Type type{TypeKind::Tuple}; CABIType abi;
            for (const auto &field : source.args) {
                auto normalized = externType(field, loc);
                type.args.push_back(normalized.first); abi.fields.push_back(normalized.second);
            }
            return {type, abi};
        }
        return {substitute(source, {}, loc), {}};
    }
    CABIType canonicalABI(const Type &type, CABIType descriptor) {
        if (type == I64) {
            if (!descriptor.bits) descriptor.bits = 64;
            if (descriptor.bits == 64) descriptor.isSigned = true;
        } else if (type.kind == TypeKind::Tuple) {
            if (descriptor.fields.empty()) descriptor.fields.resize(type.args.size());
            for (size_t i = 0; i < type.args.size(); ++i) descriptor.fields[i] = canonicalABI(type.args[i], descriptor.fields[i]);
        }
        return descriptor;
    }
    bool sameExternalABI(const External &left, const External &right) {
        if (left.parameters != right.parameters || left.result != right.result || left.variadic != right.variadic) return false;
        if (canonicalABI(left.result, left.abiResult) != canonicalABI(right.result, right.abiResult)) return false;
        for (size_t i = 0; i < left.parameters.size(); ++i) {
            CABIType a = left.abiParameters.empty() ? CABIType{} : left.abiParameters[i];
            CABIType b = right.abiParameters.empty() ? CABIType{} : right.abiParameters[i];
            if (canonicalABI(left.parameters[i], a) != canonicalABI(right.parameters[i], b)) return false;
        }
        return true;
    }
    void validateType(const Type &t, Location loc, bool allowUnit = false, unsigned depth = 0) {
        if (depth == 0) (void)typeNodes(t, loc);
        if (depth > 64)
            fail(loc, "type nesting exceeds the supported limit");
        if (t.kind == TypeKind::Named) fail(loc, "unresolved type '" + t.str() + "'");
        if (t == Unit) {
            if (!allowUnit)
                fail(loc,
                     "unit has no value and cannot be a parameter, binding, or container element");
            return;
        }
        if (t.kind == TypeKind::Function) {
            if (t.args.empty())
                fail(loc, "function type requires a result type");
            for (size_t i = 0; i + 1 < t.args.size(); ++i)
                validateType(t.args[i], loc, false, depth + 1);
            validateType(t.args.back(), loc, true, depth + 1);
            return;
        }
        if (t.kind == TypeKind::Pointer) {
            if (t.args.size() != 1)
                fail(loc, "pointer type requires one element type");
            validateType(t.args[0], loc, true, depth + 1);
            return;
        }
        if (t.kind == TypeKind::Task) {
            if (t.args.size() != 1) fail(loc, "task type requires one result type");
            validateType(t.args[0], loc, true, depth + 1); return;
        }
        for (const auto &element : t.args)
            validateType(element, loc, false, depth + 1);
    }
    size_t jump(Op op, int value, Location loc) {
        Instruction ins{op};
        if (value >= 0)
            ins.args = {value};
        ins.location = std::move(loc);
        fn().code.push_back(std::move(ins));
        return fn().code.size() - 1;
    }
    void patch(size_t index, int target) {
        fn().code.at(index).target = target;
    }
    int position() {
        return static_cast<int>(fn().code.size());
    }
    int integer(int64_t value, Location loc = {}) {
        int r = emit(Op::ConstI64, I64, {}, loc);
        fn().code.back().integer = value;
        return r;
    }
    int boolean(bool value, Location loc = {}) {
        int r = emit(Op::ConstBool, Bool, {}, loc);
        fn().code.back().integer = value;
        return r;
    }
    int string(const std::string &value, Location loc = {}) {
        int r = emit(Op::ConstString, String, {}, loc);
        fn().code.back().text = value;
        return r;
    }
    void require(const Type &actual, const Type &expected, Location loc, const std::string &what) {
        if (actual != expected)
            fail(loc, what + " requires " + expected.str() + ", received " + actual.str());
    }
    Binding *binding(const std::string &name) {
        for (auto it = context->scopes.rbegin(); it != context->scopes.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end())
                return &found->second;
        }
        return nullptr;
    }
    Declaration *declaration(const std::string &name, const Location &loc) {
        auto qualified = qualifiedModule(name, loc);
        if (!qualified.first.empty()) {
            auto found = byModule.find(qualified.first + "::" + qualified.second);
            if (found == byModule.end()) return nullptr;
            auto &result = declarations[found->second];
            if (!result.ast->exported) fail(loc, "module member '" + name + "' is private");
            return &result;
        }
        auto local = byModule.find(loc.file + "::" + name);
        if (local != byModule.end())
            return &declarations[local->second];
        if (externals.count(loc.file + "::" + name))
            return nullptr;
        auto global = exported.find(name);
        Declaration *result = nullptr;
        if (global != exported.end())
            for (size_t index : global->second)
                if (imports[loc.file].count(declarations[index].file)) {
                    if (result)
                        fail(loc, "ambiguous imported symbol '" + name + "'");
                    result = &declarations[index];
                }
        auto native = exportedExternals.find(name);
        size_t nativeCount = 0;
        if (native != exportedExternals.end())
            for (const auto &item : native->second)
                if (imports[loc.file].count(item.first))
                    ++nativeCount;
        if (nativeCount > 1 || (result && nativeCount))
            fail(loc, "ambiguous imported callable '" + name + "'");
        return result;
    }
    External *external(const std::string &name, const Location &loc) {
        auto qualified = qualifiedModule(name, loc);
        if (!qualified.first.empty()) {
            auto key = qualified.first + "::" + qualified.second;
            auto found = externals.find(key);
            if (found == externals.end()) return nullptr;
            if (!exportedExternalKeys.count(key)) fail(loc, "module extern '" + name + "' is private");
            return &found->second;
        }
        auto local = externals.find(loc.file + "::" + name);
        if (local != externals.end())
            return &local->second;
        auto exportedNative = exportedExternals.find(name);
        if (exportedNative == exportedExternals.end())
            return nullptr;
        External *result = nullptr;
        for (const auto &item : exportedNative->second)
            if (imports[loc.file].count(item.first)) {
                if (result)
                    fail(loc, "ambiguous imported extern '" + name + "'");
                result = &externals.at(item.second);
            }
        return result;
    }
    void define(const std::string &name, int value, bool mut, Location loc) {
        if (reserved(name))
            fail(loc, "'" + name + "' is reserved for the language or runtime");
        if (moduleAliases[loc.file].count(name)) fail(loc, "binding name '" + name + "' conflicts with a module alias");
        if (context->scopes.back().count(name))
            fail(loc, "duplicate local name '" + name + "'");
        if (value < 0)
            fail(loc, "cannot bind a unit value");
        Type t = type(value);
        int cell = emit(Op::MakeCell, Type::cell(t), {value}, loc);
        fn().localNames[cell] = name;
        context->scopes.back()[name] = {cell, t, mut};
    }
    std::vector<std::pair<std::string, Binding>> captures() {
        std::map<std::string, Binding> visible;
        for (auto &scope : context->scopes)
            for (auto &item : scope)
                visible[item.first] = item.second;
        return {visible.begin(), visible.end()};
    }
    bool returns(const std::vector<StmtPtr> &body, bool loopExit = false) {
        for (auto &statement : body) {
            if (statement->kind == Stmt::Kind::Return)
                return true;
            if (loopExit &&
                (statement->kind == Stmt::Kind::Break || statement->kind == Stmt::Kind::Continue))
                return true;
            if (statement->kind == Stmt::Kind::Expression && statement->expression) {
                const auto &e = *statement->expression;
                if (e.kind == Expr::Kind::Call && !e.items.empty() &&
                    e.items[0]->kind == Expr::Kind::Name && e.items[0]->text == "panic")
                    return true;
                if (e.kind == Expr::Kind::Binary && e.text == "|>" && e.items.size() == 2) {
                    const auto &callee = e.items[1];
                    if (callee->kind == Expr::Kind::Name && callee->text == "panic")
                        return true;
                }
            }
            if (statement->kind == Stmt::Kind::Unsafe && returns(statement->body, loopExit))
                return true;
            if (statement->kind == Stmt::Kind::If && !statement->elseBody.empty() &&
                returns(statement->body, loopExit) && returns(statement->elseBody, loopExit))
                return true;
            if (statement->kind == Stmt::Kind::Match) {
                bool wildcard = false, all = true, yes = false, no = false, ok = false, err = false;
                for (auto &arm : statement->body) {
                    wildcard = wildcard || (!arm->other && (arm->name == "_" || irrefutable(arm->expression)));
                    all = all && returns(arm->body, loopExit);
                    if (!arm->other && arm->expression && arm->expression->kind == Expr::Kind::Boolean) {
                        yes = yes || arm->expression->integer != 0;
                        no = no || arm->expression->integer == 0;
                    }
                    if (!arm->other && arm->expression && arm->expression->kind == Expr::Kind::Call &&
                        arm->expression->items.size() == 2 &&
                        arm->expression->items[0]->kind == Expr::Kind::Name &&
                        irrefutable(arm->expression->items[1])) {
                        ok = ok || arm->expression->items[0]->text == "ok";
                        err = err || arm->expression->items[0]->text == "err";
                    }
                }
                if ((statement->exhaustive || wildcard || (yes && no) || (ok && err)) && all)
                    return true;
            }
        }
        return false;
    }
    bool irrefutable(const ExprPtr &patternValue) {
        if (!patternValue)
            return false;
        if (patternValue->kind == Expr::Kind::Name)
            return true;
        if (patternValue->kind != Expr::Kind::Tuple)
            return false;
        for (auto &item : patternValue->items)
            if (!irrefutable(item))
                return false;
        return true;
    }
    bool exhaustiveMatrix(const std::vector<std::vector<ExprPtr>> &rows,
                          const std::vector<Type> &columns, Location loc,
                          unsigned &budget, unsigned depth = 0) {
        if (!budget-- || depth > 128) fail(loc, "pattern exhaustiveness analysis limit exceeded");
        if (rows.empty()) return false;
        if (columns.empty()) return true;
        auto wildcard = [](const ExprPtr &p) { return !p || p->kind == Expr::Kind::Name; };
        for (const auto &row : rows) {
            bool all = true;
            for (const auto &p : row) all = all && wildcard(p);
            if (all) return true;
        }
        const Type &subject = columns[0];
        const EnumInstance *instance = nullptr;
        if (enumInstances.count(subject.name)) instance = &enumInstances.at(subject.name);
        const bool tuple = subject.kind == TypeKind::Tuple && subject.name.empty();
        const bool result = subject.kind == TypeKind::Result;
        const bool booleanType = subject == Bool;
        size_t variants = instance ? instance->declaration->body.size() : tuple ? 1 : (result || booleanType) ? 2 : 0;
        if (!variants) {
            std::vector<std::vector<ExprPtr>> remaining;
            for (const auto &row : rows) if (wildcard(row[0])) remaining.emplace_back(row.begin() + 1, row.end());
            return exhaustiveMatrix(remaining, {columns.begin() + 1, columns.end()}, loc, budget, depth + 1);
        }
        for (size_t variant = 0; variant < variants; ++variant) {
            std::vector<Type> fields;
            if (tuple) fields = subject.args;
            else if (result) fields = {subject.args[variant]};
            else if (instance) {
                size_t offset = instance->offsets[variant];
                const auto &layout = program.nominalLayouts.at(subject.name);
                for (size_t i = 0; i < instance->declaration->body[variant]->parameters.size(); ++i) fields.push_back(layout.args[offset + i]);
            }
            std::vector<std::vector<ExprPtr>> specialized;
            for (const auto &row : rows) {
                auto pattern = row[0];
                std::vector<ExprPtr> payload;
                bool matches = wildcard(pattern);
                if (matches) payload.resize(fields.size());
                else if (tuple && pattern->kind == Expr::Kind::Tuple) { matches = true; payload = pattern->items; }
                else if (booleanType && pattern->kind == Expr::Kind::Boolean) matches = (pattern->integer != 0) == (variant != 0);
                else if (pattern->kind == Expr::Kind::Call && !pattern->items.empty() && pattern->items[0]->kind == Expr::Kind::Name) {
                    auto name = pattern->items[0]->text;
                    if (result) matches = name == (variant == 0 ? "ok" : "err");
                    else if (instance) {
                        auto target = constructor(name, pattern->location);
                        matches = target.first == instance->declaration && target.second == variant;
                    }
                    if (matches) payload.assign(pattern->items.begin() + 1, pattern->items.end());
                }
                if (matches) {
                    payload.insert(payload.end(), row.begin() + 1, row.end());
                    specialized.push_back(std::move(payload));
                }
            }
            fields.insert(fields.end(), columns.begin() + 1, columns.end());
            if (!exhaustiveMatrix(specialized, fields, loc, budget, depth + 1)) return false;
        }
        return true;
    }
    std::vector<Type> outputs(const Type &result) {
        if (result == Unit)
            return {};
        if (result.kind == TypeKind::Tuple && result.name.empty())
            return result.args;
        return {result};
    }
    int takeOutputs(const Type &result, Location loc) {
        auto expected = outputs(result);
        if (context->stack.size() < expected.size())
            fail(loc, "stack underflow: block result needs " + std::to_string(expected.size()) +
                          " values");
        std::vector<int> values(context->stack.end() - expected.size(), context->stack.end());
        for (size_t i = 0; i < values.size(); ++i)
            require(type(values[i]), expected[i], loc, "stack output " + std::to_string(i + 1));
        context->stack.resize(context->stack.size() - expected.size());
        if (result == Unit)
            return -1;
        if (result.kind == TypeKind::Tuple && result.name.empty())
            return emit(Op::MakeTuple, result, values, loc);
        return values.front();
    }
    int pop(Location loc) {
        if (context->stack.empty())
            fail(loc, "stack underflow");
        int value = context->stack.back();
        context->stack.pop_back();
        return value;
    }
    void sameStack(const std::vector<int> &a, const std::vector<int> &b, Location loc) {
        if (a.size() != b.size())
            fail(loc, "control-flow paths leave different stack heights");
        for (size_t i = 0; i < a.size(); ++i)
            require(type(a[i]), type(b[i]), loc, "stack slot " + std::to_string(i));
    }
    void copyStack(const std::vector<int> &from, const std::vector<int> &to, Location loc) {
        sameStack(from, to, loc);
        std::vector<int> temps;
        for (int value : from)
            temps.push_back(emit(Op::Move, type(value), {value}, loc));
        for (size_t i = 0; i < to.size(); ++i) {
            Instruction move{Op::Move};
            move.dst = to[i];
            move.args = {temps[i]};
            move.location = loc;
            fn().code.push_back(move);
        }
    }
    std::vector<int> freshStack(const std::vector<int> &values, Location loc) {
        std::vector<int> result;
        for (int value : values)
            result.push_back(emit(Op::Move, type(value), {value}, loc));
        return result;
    }
    int binary(const std::string &operation, int left, int right, Location loc) {
        if (left < 0 || right < 0)
            fail(loc, "operator cannot use unit values");
        Type t = type(left);
        require(type(right), t, loc, "operator '" + operation + "'");
        static const std::map<std::string, Op> operators = {
            {"+", Op::Add}, {"-", Op::Sub}, {"*", Op::Mul}, {"/", Op::Div},
            {"%", Op::Mod}, {"==", Op::Eq}, {"=", Op::Eq},  {"!=", Op::Ne},
            {"<", Op::Lt},  {"<=", Op::Le}, {">", Op::Gt},  {">=", Op::Ge}};
        auto found = operators.find(operation);
        if (found == operators.end())
            fail(loc, "unsupported operator '" + operation + "'");
        bool compare = found->second >= Op::Eq && found->second <= Op::Ge;
        if (compare) {
            if (!(t.numeric() || t == Bool || t == String || t.kind == TypeKind::Pointer))
                fail(loc, "this type does not support comparison");
            if ((t == Bool || t == String || t.kind == TypeKind::Pointer) && operation != "==" &&
                operation != "=" && operation != "!=")
                fail(loc, "this type supports equality comparison only");
        } else if (!t.numeric() || (operation == "%" && t != I64))
            fail(loc, "arithmetic requires matching numeric operands");
        return emit(found->second, compare ? Bool : t, {left, right}, loc);
    }
    int invoke(const std::string &name, std::vector<int> arguments, Location loc,
               const Type *expected = nullptr) {
        if (name == "spawn") {
            if (arguments.size() != 1 || type(arguments[0]).kind != TypeKind::Function || type(arguments[0]).args.size() != 1)
                fail(loc, "spawn requires one zero-argument function");
            std::set<std::string> transferVisited;
            auto transferable = [&](const auto &self, const Type &t) -> bool {
                if (t.kind == TypeKind::Pointer || t.kind == TypeKind::Task) return false;
                if (!t.name.empty()) {
                    auto layout = program.nominalLayouts.find(t.name);
                    if (layout == program.nominalLayouts.end()) return false;
                    if (!transferVisited.insert(t.name).second) return true;
                    for (const auto &argument : layout->second.args) if (!self(self, argument)) return false;
                    return true;
                }
                for (const auto &argument : t.args) if (!self(self, argument)) return false;
                return true;
            };
            Type result = type(arguments[0]).args[0];
            if (!transferable(transferable, result)) fail(loc, "spawn result cannot contain raw pointers or tasks");
            std::set<int> examined;
            auto inspectClosure = [&](const auto &self, int value) -> void {
                if (!examined.insert(value).second) return;
                for (const auto &instruction : fn().code) {
                    if (instruction.dst == value) {
                        if (instruction.op == Op::MakeClosure) {
                            for (int capture : instruction.args) {
                                if (!transferable(transferable, type(capture))) fail(loc, "spawn captures cannot contain raw pointers or tasks");
                                self(self, capture);
                            }
                        } else if (instruction.op == Op::LoadCell || instruction.op == Op::MakeCell || instruction.op == Op::Move) {
                            for (int input : instruction.args) self(self, input);
                        }
                    }
                    if (instruction.op == Op::StoreCell && instruction.args.size() == 2 && instruction.args[0] == value) self(self, instruction.args[1]);
                }
            };
            inspectClosure(inspectClosure, arguments[0]);
            return emit(Op::Spawn, Type{TypeKind::Task, {result}}, arguments, loc);
        }
        if (name == "join") {
            if (arguments.size() != 1 || type(arguments[0]).kind != TypeKind::Task || type(arguments[0]).args.size() != 1)
                fail(loc, "join requires one task");
            return emit(Op::Join, type(arguments[0]).args[0], arguments, loc);
        }
        if (name == "alloc_i64" || name == "alloc_f64" || name == "alloc_bool" || name == "free" ||
            name == "load" || name == "store" || name == "cstr" || name == "pin" ||
            name == "unpin" || name == "offset") {
            if (!context->unsafeMode)
                fail(loc, "'" + name + "' requires an explicit unsafe: block");
            if (name == "alloc_i64" || name == "alloc_f64" || name == "alloc_bool") {
                if (arguments.size() != 1)
                    fail(loc, name + " expects an element count");
                require(type(arguments[0]), I64, loc, "allocation size");
                Type element = name == "alloc_i64" ? I64 : (name == "alloc_f64" ? F64 : Bool);
                return emit(Op::RawAlloc, Type{TypeKind::Pointer, {element}}, arguments, loc);
            }
            if (name == "cstr") {
                if (arguments.size() != 1)
                    fail(loc, "cstr expects one string");
                require(type(arguments[0]), String, loc, "cstr");
                return emit(Op::CString, Type{TypeKind::Pointer, {Unit}}, arguments, loc);
            }
            if (name == "pin") {
                if (arguments.size() != 1 || !type(arguments[0]).managed())
                    fail(loc, "pin requires one managed value");
                return emit(Op::Pin, Type{TypeKind::Pointer, {Unit}}, arguments, loc);
            }
            if (arguments.empty() || type(arguments[0]).kind != TypeKind::Pointer ||
                type(arguments[0]).args.size() != 1)
                fail(loc, name + " requires a pointer");
            if (name == "free" || name == "unpin") {
                if (arguments.size() != 1)
                    fail(loc, name + " expects one pointer");
                emit(name == "free" ? Op::RawFree : Op::Unpin, Unit, arguments, loc);
                return -1;
            }
            Type element = type(arguments[0]).args[0];
            if (name == "offset") {
                if (arguments.size() != 2) fail(loc, "offset expects a pointer and an element offset");
                require(type(arguments[1]), I64, loc, "pointer offset");
                if (element != Unit && element != I64 && element != F64 && element != Bool && element.kind != TypeKind::Pointer)
                    fail(loc, "offset requires a scalar pointer or ptr[unit] byte pointer");
                return emit(Op::RawOffset, type(arguments[0]), arguments, loc);
            }
            if (element != I64 && element != F64 && element != Bool &&
                element.kind != TypeKind::Pointer)
                fail(loc, "pointer load/store requires a concrete scalar element type");
            if (arguments.size() != (name == "load" ? 2 : 3))
                fail(loc, name + " has wrong argument count");
            require(type(arguments[1]), I64, loc, "pointer index");
            if (name == "load")
                return emit(Op::RawLoad, element, arguments, loc);
            require(type(arguments[2]), element, loc, "pointer store");
            emit(Op::RawStore, Unit, arguments, loc);
            return -1;
        }
        if (name == "print" || name == "dump") {
            for (int value : arguments) {
                if (value < 0)
                    fail(loc, "cannot print unit");
                emit(Op::Print, Unit, {value}, loc);
            }
            return -1;
        }
        if (name == "gc") {
            if (!arguments.empty())
                fail(loc, "gc expects no arguments");
            emit(Op::Collect, Unit, {}, loc);
            return -1;
        }
        if (name == "panic") {
            if (arguments.size() != 1)
                fail(loc, "panic expects one string");
            require(type(arguments[0]), String, loc, "panic");
            emit(Op::Panic, Unit, arguments, loc);
            return -1;
        }
        if (name == "len") {
            if (arguments.size() != 1)
                fail(loc, "len expects one argument");
            Type t = type(arguments[0]);
            if (!t.name.empty()) fail(loc, "len does not expose enum representation");
            if (t.kind != TypeKind::Array && t.kind != TypeKind::Tuple && t != String)
                fail(loc, "len requires array, tuple, or string");
            return emit(Op::Length, I64, arguments, loc);
        }
        if (name == "i64" || name == "f64") {
            if (arguments.size() != 1 || !type(arguments[0]).numeric())
                fail(loc, "numeric conversion expects one numeric argument");
            Type target = name == "i64" ? I64 : F64;
            return type(arguments[0]) == target ? arguments[0]
                                                : emit(Op::Convert, target, arguments, loc);
        }
        if (name == "ok" || name == "err") {
            if (arguments.size() != 1)
                fail(loc, name + " expects one argument");
            Type result = expected && expected->kind == TypeKind::Result
                              ? *expected
                              : Type{TypeKind::Result, {type(arguments[0]), String}};
            if (name == "err" && (!expected || expected->kind != TypeKind::Result))
                fail(
                    loc,
                    "err needs an expected Result[T,E] type (annotate the binding or return type)");
            int yes = boolean(name == "ok", loc);
            int good = -1, bad = -1;
            auto zeroValue = [&](const Type &target) {
                if (target == I64)
                    return integer(0, loc);
                if (target == Bool)
                    return boolean(false, loc);
                if (target == F64)
                    return emit(Op::ConstF64, F64, {}, loc);
                if (target == String)
                    return string("", loc);
                if (target == Unit)
                    fail(loc, "Result alternatives cannot be unit; use bool to represent an empty "
                              "success/error");
                return emit(Op::Zero, target, {}, loc);
            };
            if (name == "ok") {
                require(type(arguments[0]), result.args[0], loc, "ok value");
                good = arguments[0];
                bad = zeroValue(result.args[1]);
            } else {
                require(type(arguments[0]), result.args[1], loc, "err value");
                good = zeroValue(result.args[0]);
                bad = arguments[0];
            }
            return emit(Op::MakeTuple, result, {yes, good, bad}, loc);
        }
        if (name == "is_ok" || name == "unwrap" || name == "error") {
            if (arguments.size() != 1 || type(arguments[0]).kind != TypeKind::Result)
                fail(loc, name + " requires Result[T,E]");
            Type t = type(arguments[0]);
            if (name == "unwrap")
                return emit(Op::Unwrap, t.args[0], arguments, loc);
            if (name == "error")
                return emit(Op::ResultError, t.args[1], arguments, loc);
            return emit(Op::Index, Bool, {arguments[0], integer(0, loc)}, loc);
        }
        Binding *local = binding(name);
        if (local) {
            Binding value = *local;
            if (value.type.kind != TypeKind::Function)
                fail(loc, "'" + name + "' is not callable");
            int closure = emit(Op::LoadCell, value.type, {value.cell}, loc);
            return indirect(closure, std::move(arguments), loc);
        }
        if (auto *target = declaration(name, loc)) {
            Declaration resolved = *target;
            if (!resolved.ast->typeParameters.empty()) resolved = specialize(resolved, {}, &arguments, expected, loc);
            auto params = resolved.type.args;
            Type result = params.back();
            params.pop_back();
            if (arguments.size() != params.size())
                fail(loc, "function '" + name + "' expects " + std::to_string(params.size()) +
                              " arguments");
            for (size_t i = 0; i < params.size(); ++i)
                require(type(arguments[i]), params[i], loc,
                        "argument " + std::to_string(i + 1) + " of '" + name + "'");
            int value = emit(Op::Call, result, arguments, loc);
            fn().code.back().text = resolved.irName;
            return value;
        }
        if (auto *resolved = external(name, loc)) {
            if (!context->unsafeMode)
                fail(loc, "extern call '" + name + "' requires an explicit unsafe: block");
            auto &target = *resolved;
            if (arguments.size() < target.parameters.size() ||
                (!target.variadic && arguments.size() != target.parameters.size()))
                fail(loc, "wrong argument count for extern '" + name + "'");
            for (size_t i = 0; i < target.parameters.size(); ++i)
                require(type(arguments[i]), target.parameters[i], loc,
                        "extern argument " + std::to_string(i + 1));
            for (size_t i = target.parameters.size(); i < arguments.size(); ++i)
                if (!cType(type(arguments[i])))
                    fail(loc, "variadic argument requires a C-compatible scalar, pointer or flat "
                              "tuple record");
            int value = emit(Op::ExternalCall, target.result, arguments, loc);
            fn().code.back().text = target.name;
            return value;
        }
        fail(loc, "unknown function '" + name + "'");
    }
    int indirect(int closure, std::vector<int> arguments, Location loc) {
        Type callable = type(closure);
        if (callable.kind != TypeKind::Function || callable.args.empty())
            fail(loc, "expression is not callable");
        if (arguments.size() + 1 != callable.args.size())
            fail(loc, "wrong number of closure arguments");
        for (size_t i = 0; i < arguments.size(); ++i)
            require(type(arguments[i]), callable.args[i], loc,
                    "closure argument " + std::to_string(i + 1));
        arguments.insert(arguments.begin(), closure);
        return emit(Op::CallIndirect, callable.args.back(), arguments, loc);
    }
    int closure(const Expr &expression) {
        auto captured = captures();
        std::string name = "lambda_" + std::to_string(serial++);
        std::vector<Type> params;
        for (auto &p : expression.parameters)
            params.push_back(p.type);
        Type callable = Type::function(params, expression.returnType);
        compileFunction(name, expression.parameters, expression.returnType, expression.body,
                        captured, false, expression.location);
        std::vector<int> cells;
        for (auto &item : captured)
            cells.push_back(item.second.cell);
        int value = emit(Op::MakeClosure, callable, cells, expression.location);
        fn().code.back().text = name;
        return value;
    }
    std::vector<Type> parameterTypes(const std::string &name, Location loc, const Type *expected) {
        if ((name == "ok" || name == "err") && expected && expected->kind == TypeKind::Result)
            return {expected->args[name == "ok" ? 0 : 1]};
        if (auto *local = binding(name)) {
            auto result = local->type.args;
            if (local->type.kind == TypeKind::Function && !result.empty())
                result.pop_back();
            return result;
        }
        if (auto *target = declaration(name, loc)) {
            if (!target->ast->typeParameters.empty()) return {};
            auto result = target->type.args;
            result.pop_back();
            return result;
        }
        if (auto *native = external(name, loc))
            return native->parameters;
        return {};
    }
    int construct(const std::pair<StmtPtr, size_t> &target, const std::vector<Type> &explicitTypes,
                  const std::vector<ExprPtr> &arguments, const Type *expected, Location loc) {
        const auto &definition = target.first;
        const auto &variant = definition->body.at(target.second);
        if (arguments.size() != variant->parameters.size()) fail(loc, "wrong number of constructor arguments for '" + variant->name + "'");
        std::set<std::string> variables(definition->typeParameters.begin(), definition->typeParameters.end());
        std::map<std::string, Type> bindings;
        if (expected && enumInstances.count(expected->name)) {
            const auto &instance = enumInstances.at(expected->name);
            if (instance.declaration != definition) fail(loc, "constructor does not belong to the expected enum");
            for (size_t i = 0; i < instance.arguments.size(); ++i) bindings[definition->typeParameters[i]] = instance.arguments[i];
        }
        if (!explicitTypes.empty()) {
            if (explicitTypes.size() != definition->typeParameters.size()) fail(loc, "wrong number of enum type arguments");
            for (size_t i = 0; i < explicitTypes.size(); ++i) {
                const auto &name = definition->typeParameters[i];
                if (bindings.count(name)) require(explicitTypes[i], bindings.at(name), loc, "enum type argument");
                bindings[name] = explicitTypes[i];
            }
        }
        std::vector<int> values;
        for (size_t i = 0; i < arguments.size(); ++i) {
            const auto &parameter = variant->parameters[i];
            Type hint = partialType(parameter.type, bindings, parameter.location);
            values.push_back(expr(arguments[i], unresolved(hint) ? nullptr : &hint));
            infer(parameter.type, type(values.back()), variables, bindings, parameter.location);
        }
        std::vector<Type> types;
        for (const auto &name : definition->typeParameters) {
            if (!bindings.count(name)) fail(loc, "cannot infer enum type parameter '" + name + "'; annotate the value or use ::[types]");
            types.push_back(bindings.at(name));
        }
        Type layout = enumType(definition, types, loc, 0);
        const auto &instance = enumInstances.at(layout.name);
        const auto &fieldsLayout = program.nominalLayouts.at(layout.name);
        std::vector<int> fields{integer(static_cast<int64_t>(target.second), loc)};
        const size_t offset = instance.offsets.at(target.second);
        for (size_t i = 1; i < fieldsLayout.args.size(); ++i) {
            if (i >= offset && i < offset + values.size()) {
                require(type(values[i - offset]), fieldsLayout.args[i], loc, "constructor payload");
                fields.push_back(values[i - offset]);
            } else fields.push_back(emit(Op::Zero, fieldsLayout.args[i], {}, loc));
        }
        return emit(Op::MakeTuple, layout, fields, loc);
    }
    int expr(const ExprPtr &expression, const Type *expected = nullptr) {
        if (!expression)
            return -1;
        const auto &e = *expression;
        auto loc = e.location;
        switch (e.kind) {
        case Expr::Kind::Integer:
            return integer(e.integer, loc);
        case Expr::Kind::Float: {
            int r = emit(Op::ConstF64, F64, {}, loc);
            fn().code.back().real = e.real;
            return r;
        }
        case Expr::Kind::Boolean:
            return boolean(e.integer != 0 || e.text == "true", loc);
        case Expr::Kind::String:
            return string(e.text, loc);
        case Expr::Kind::Name: {
            if (auto *local = binding(e.text)) {
                Binding v = *local;
                return emit(Op::LoadCell, v.type, {v.cell}, loc);
            }
            if (auto *target = declaration(e.text, loc)) {
                if (!target->ast->typeParameters.empty()) fail(loc, "generic function values need explicit ::[types]");
                int r = emit(Op::MakeClosure, target->type, {}, loc);
                fn().code.back().text = target->irName;
                return r;
            }
            fail(loc, "unknown name '" + e.text + "'");
        }
        case Expr::Kind::Unary: {
            int value = expr(e.items.at(0));
            if (e.text == "not" || e.text == "!") {
                require(type(value), Bool, loc, "not");
                return emit(Op::Not, Bool, {value}, loc);
            }
            if (!type(value).numeric())
                fail(loc, "unary arithmetic requires numeric value");
            return e.text == "+" ? value : emit(Op::Neg, type(value), {value}, loc);
        }
        case Expr::Kind::Binary: {
            int left = expr(e.items.at(0));
            if (e.text == "|>") {
                auto right = e.items.at(1);
                std::vector<int> arguments{left};
                ExprPtr callee = right;
                if (right->kind == Expr::Kind::Call)
                    callee = right->items.at(0);
                int callable = -1;
                if (callee->kind != Expr::Kind::Name)
                    callable = expr(callee);
                else if (auto *local = binding(callee->text)) {
                    Binding b = *local;
                    callable = emit(Op::LoadCell, b.type, {b.cell}, loc);
                }
                if (right->kind == Expr::Kind::Call)
                    for (size_t i = 1; i < right->items.size(); ++i)
                        arguments.push_back(expr(right->items[i]));
                if (callable >= 0)
                    return indirect(callable, arguments, loc);
                return invoke(callee->text, arguments, loc, expected);
            }
            if (e.text == "and" || e.text == "or" || e.text == "&&" || e.text == "||") {
                require(type(left), Bool, loc, "boolean operation");
                auto carry = freshStack(context->stack, loc);
                context->stack = carry;
                int result = emit(Op::Move, Bool, {left}, loc);
                bool conjunction = e.text == "and" || e.text == "&&";
                int condition = conjunction ? left : emit(Op::Not, Bool, {left}, loc);
                size_t skip = jump(Op::JumpFalse, condition, loc);
                int right = expr(e.items[1]);
                require(type(right), Bool, loc, "boolean operation");
                copyStack(context->stack, carry, loc);
                context->stack = carry;
                Instruction move{Op::Move};
                move.dst = result;
                move.args = {right};
                move.location = loc;
                fn().code.push_back(move);
                patch(skip, position());
                return result;
            }
            return binary(e.text, left, expr(e.items.at(1)), loc);
        }
        case Expr::Kind::Call: {
            auto callee = e.items.at(0);
            std::vector<int> arguments;
            auto constructorName = callee;
            std::vector<Type> constructorTypes;
            if (callee->kind == Expr::Kind::TypeApply) { constructorTypes = callee->typeArguments; constructorName = callee->items.at(0); }
            if (constructorName->kind == Expr::Kind::Name && !binding(constructorName->text)) {
                auto target = constructor(constructorName->text, loc);
                if (target.first) return construct(target, constructorTypes, {e.items.begin() + 1, e.items.end()}, expected, loc);
            }
            if (callee->kind == Expr::Kind::Name && callee->text == "c_callback") {
                if (!context->unsafeMode) fail(loc, "c_callback requires an explicit unsafe: block");
                if (e.items.size() != 2) fail(loc, "c_callback expects one named source function");
                auto source = e.items[1];
                std::vector<Type> typeArguments;
                if (source->kind == Expr::Kind::TypeApply) { typeArguments = source->typeArguments; source = source->items.at(0); }
                if (source->kind != Expr::Kind::Name || binding(source->text)) fail(loc, "c_callback requires a noncapturing module-level function");
                auto *declared = declaration(source->text, loc);
                if (!declared || declared->ast->kind != Stmt::Kind::Function) fail(loc, "c_callback requires a named structured source function");
                Declaration target = *declared;
                if (!target.ast->typeParameters.empty()) {
                    if (typeArguments.empty()) fail(loc, "generic callbacks require explicit ::[types]");
                    target = specialize(target, typeArguments, nullptr, nullptr, loc);
                } else if (!typeArguments.empty()) fail(loc, "callback type arguments require a generic function");
                auto signature = target.type.args;
                Type result = signature.back(); signature.pop_back();
                if (result != Unit && !cScalar(result)) fail(loc, "C callbacks require scalar/pointer or unit results");
                for (const auto &parameter : signature) if (!cScalar(parameter)) fail(loc, "C callbacks require scalar/pointer parameters");
                int callback = emit(Op::CCallback, Type{TypeKind::Pointer, {Unit}}, {}, loc);
                fn().code.back().text = target.irName;
                return callback;
            }
            if (callee->kind == Expr::Kind::Name && !binding(callee->text)) {
                if (auto *target = declaration(callee->text, loc)) {
                    if (!target->ast->typeParameters.empty()) {
                        std::map<std::string, Type> bindings;
                        const auto variables = std::set<std::string>(target->ast->typeParameters.begin(), target->ast->typeParameters.end());
                        if (expected) infer(target->ast->returnType, *expected, variables, bindings, target->ast->location);
                        for (size_t i = 1; i < e.items.size(); ++i) {
                            Type hint;
                            const Type *expectedArgument = nullptr;
                            if (i <= target->ast->parameters.size()) {
                                const auto &parameter = target->ast->parameters[i - 1];
                                hint = partialType(parameter.type, bindings, parameter.location);
                                if (hint != Unit && !unresolved(hint)) expectedArgument = &hint;
                            }
                            arguments.push_back(expr(e.items[i], expectedArgument));
                            if (i <= target->ast->parameters.size()) infer(target->ast->parameters[i - 1].type, type(arguments.back()), variables, bindings, target->ast->parameters[i - 1].location);
                        }
                        return invoke(callee->text, arguments, loc, expected);
                    }
                }
            }
            if (callee->kind == Expr::Kind::Name) {
                auto parameters = parameterTypes(callee->text, loc, expected);
                if (auto *local = binding(callee->text)) {
                    Binding b = *local;
                    int callable = emit(Op::LoadCell, b.type, {b.cell}, loc);
                    for (size_t i = 1; i < e.items.size(); ++i)
                        arguments.push_back(expr(
                            e.items[i], i - 1 < parameters.size() ? &parameters[i - 1] : nullptr));
                    return indirect(callable, arguments, loc);
                }
                for (size_t i = 1; i < e.items.size(); ++i)
                    arguments.push_back(
                        expr(e.items[i], i - 1 < parameters.size() ? &parameters[i - 1] : nullptr));
                return invoke(callee->text, arguments, loc, expected);
            }
            int callable = expr(callee);
            auto parameters = type(callable).args;
            if (type(callable).kind == TypeKind::Function && !parameters.empty())
                parameters.pop_back();
            else
                parameters.clear();
            for (size_t i = 1; i < e.items.size(); ++i)
                arguments.push_back(
                    expr(e.items[i], i - 1 < parameters.size() ? &parameters[i - 1] : nullptr));
            return indirect(callable, arguments, loc);
        }
        case Expr::Kind::TypeApply: {
            auto name = e.items.at(0);
            if (name->kind != Expr::Kind::Name || binding(name->text)) fail(loc, "type application requires a module-level generic function");
            auto *target = declaration(name->text, loc);
            if (!target || target->ast->typeParameters.empty()) fail(loc, "type application requires a generic function");
            auto concrete = specialize(*target, e.typeArguments, nullptr, nullptr, loc);
            int result = emit(Op::MakeClosure, concrete.type, {}, loc);
            fn().code.back().text = concrete.irName;
            return result;
        }
        case Expr::Kind::Propagate: {
            int value = expr(e.items.at(0));
            Type source = type(value), destination = fn().result;
            if (source.kind != TypeKind::Result || destination.kind != TypeKind::Result)
                fail(loc, "Result propagation requires a Result operand and Result-returning function");
            if (!context->stack.empty()) fail(loc, "Result propagation requires an empty shared operand stack");
            require(source.args[1], destination.args[1], loc, "propagated error");
            int good = emit(Op::Index, Bool, {value, integer(0, loc)}, loc);
            size_t success = jump(Op::JumpFalse, emit(Op::Not, Bool, {good}, loc), loc);
            int error = emit(Op::ResultError, source.args[1], {value}, loc);
            int returned = invoke("err", {error}, loc, &destination);
            emit(Op::Return, Unit, {returned}, loc);
            patch(success, position());
            return emit(Op::Unwrap, source.args[0], {value}, loc);
        }
        case Expr::Kind::Array: {
            std::vector<int> values;
            Type element;
            if (expected && expected->kind == TypeKind::Array)
                element = expected->args.at(0);
            for (auto &item : e.items) {
                int value = expr(item, element == Unit ? nullptr : &element);
                if (value < 0)
                    fail(item->location, "array cannot contain unit");
                if (values.empty() && element == Unit)
                    element = type(value);
                require(type(value), element, item->location, "array element");
                values.push_back(value);
            }
            if (element == Unit)
                fail(loc, "empty array requires an explicit element type");
            return emit(Op::MakeArray, Type::array(element), values, loc);
        }
        case Expr::Kind::Tuple: {
            std::vector<int> values;
            std::vector<Type> types;
            for (size_t index = 0; index < e.items.size(); ++index) {
                auto &item = e.items[index];
                const Type *field =
                    expected && expected->kind == TypeKind::Tuple && index < expected->args.size()
                        ? &expected->args[index]
                        : nullptr;
                int value = expr(item, field);
                if (value < 0)
                    fail(loc, "tuple cannot contain unit");
                values.push_back(value);
                types.push_back(type(value));
            }
            return emit(Op::MakeTuple, Type{TypeKind::Tuple, types}, values, loc);
        }
        case Expr::Kind::Index: {
            int base = expr(e.items[0]);
            int index = expr(e.items[1]);
            require(type(index), I64, loc, "index");
            Type t = type(base), result;
            if (t.kind == TypeKind::Array)
                result = t.args.at(0);
            else if (t == String)
                result = I64;
            else if (t.kind == TypeKind::Tuple && t.name.empty()) {
                if (e.items[1]->kind != Expr::Kind::Integer)
                    fail(loc, "tuple index must be an integer literal");
                int64_t i = e.items[1]->integer;
                if (i < 0 || static_cast<size_t>(i) >= t.args.size())
                    fail(loc, "tuple index out of bounds");
                result = t.args[i];
            } else
                fail(loc, "indexing requires array, tuple, or string");
            return emit(Op::Index, result, {base, index}, loc);
        }
        case Expr::Kind::Lambda:
            return closure(e);
        case Expr::Kind::Stack: {
            validateType(e.returnType, loc, true);
            for (auto &item : e.items) {
                int value = expr(item);
                if (value < 0)
                    fail(loc, "cannot push unit onto stack");
                context->stack.push_back(value);
            }
            bool previous = context->stackMode;
            context->stackMode = true;
            block(e.body, true);
            context->stackMode = previous;
            return takeOutputs(e.returnType, loc);
        }
        }
        fail(loc, "unsupported expression");
    }
    void stackOperation(const std::string &name, Location loc) {
        if (name == "dup") {
            int a = pop(loc);
            context->stack.push_back(a);
            context->stack.push_back(a);
        } else if (name == "swap") {
            int b = pop(loc), a = pop(loc);
            context->stack.push_back(b);
            context->stack.push_back(a);
        } else if (name == "drop") {
            pop(loc);
        } else if (name == "over") {
            int b = pop(loc), a = pop(loc);
            context->stack.push_back(a);
            context->stack.push_back(b);
            context->stack.push_back(a);
        } else if (name == "rot") {
            int c = pop(loc), b = pop(loc), a = pop(loc);
            context->stack.push_back(b);
            context->stack.push_back(c);
            context->stack.push_back(a);
        } else if (name == "print" || name == "dump")
            emit(Op::Print, Unit, {pop(loc)}, loc);
        else if (name == "gc")
            emit(Op::Collect, Unit, {}, loc);
        else if (name == "and" || name == "or") {
            int b = pop(loc), a = pop(loc);
            require(type(a), Bool, loc, name);
            require(type(b), Bool, loc, name);
            int result = emit(Op::Move, Bool, {a}, loc);
            int condition = name == "and" ? a : emit(Op::Not, Bool, {a}, loc);
            size_t skip = jump(Op::JumpFalse, condition, loc);
            Instruction move{Op::Move};
            move.dst = result;
            move.args = {b};
            move.location = loc;
            fn().code.push_back(move);
            patch(skip, position());
            context->stack.push_back(result);
        } else if (name == "len") {
            int value = pop(loc);
            context->stack.push_back(invoke("len", {value}, loc));
        } else if (name == "neg" || name == "not") {
            int a = pop(loc);
            if (name == "not")
                require(type(a), Bool, loc, "not");
            else if (!type(a).numeric())
                fail(loc, "neg requires numeric value");
            context->stack.push_back(emit(name == "not" ? Op::Not : Op::Neg, type(a), {a}, loc));
        } else {
            auto *target = declaration(name, loc);
            if (target && target->ast->kind == Stmt::Kind::Word) {
                size_t count = target->ast->parameters.size();
                if (context->stack.size() < count)
                    fail(loc, "stack underflow calling word '" + name + "'");
                std::vector<int> args(context->stack.end() - count, context->stack.end());
                context->stack.resize(context->stack.size() - count);
                int value = invoke(name, args, loc);
                Type result = type(value);
                if (result.kind == TypeKind::Tuple && result.name.empty())
                    for (size_t i = 0; i < result.args.size(); ++i)
                        context->stack.push_back(
                            emit(Op::Index, result.args[i],
                                 {value, integer(static_cast<int64_t>(i), loc)}, loc));
                else if (value >= 0)
                    context->stack.push_back(value);
            } else {
                int b = pop(loc), a = pop(loc);
                context->stack.push_back(binary(name, a, b, loc));
            }
        }
    }
    void conditional(const Stmt &s) {
        int condition = s.expression ? expr(s.expression) : pop(s.location);
        require(type(condition), Bool, s.location, "if condition");
        auto initial = context->stack;
        size_t otherwise = jump(Op::JumpFalse, condition, s.location);
        block(s.body, true);
        auto left = context->stack;
        bool leftReturns = returns(s.body, true);
        std::vector<int> joined;
        if (!leftReturns)
            joined = freshStack(left, s.location);
        size_t finish = leftReturns ? static_cast<size_t>(-1) : jump(Op::Jump, -1, s.location);
        patch(otherwise, position());
        context->stack = initial;
        block(s.elseBody, true);
        bool rightReturns = returns(s.elseBody, true);
        if (!leftReturns && !rightReturns)
            copyStack(context->stack, joined, s.location);
        else if (leftReturns && !rightReturns)
            joined = context->stack;
        if (finish != static_cast<size_t>(-1))
            patch(finish, position());
        context->stack = joined;
    }
    void whileStatement(const Stmt &s) {
        if (!s.elseBody.empty())
            fail(s.location, "while/else is not part of the implemented language");
        auto carry = freshStack(context->stack, s.location);
        context->stack = carry;
        int head = position();
        int condition = s.expression ? expr(s.expression) : pop(s.location);
        require(type(condition), Bool, s.location, "while condition");
        auto exitStack = carry;
        if (!s.expression)
            exitStack.pop_back();
        sameStack(context->stack, exitStack, s.location);
        size_t finish = jump(Op::JumpFalse, condition, s.location);
        context->loops.push_back({head, carry, {}, exitStack});
        block(s.body, true);
        if (!returns(s.body, true)) {
            copyStack(context->stack, carry, s.location);
            size_t back = jump(Op::Jump, -1, s.location);
            patch(back, head);
        }
        auto loop = context->loops.back();
        context->loops.pop_back();
        int end = position();
        patch(finish, end);
        for (auto index : loop.breaks)
            patch(index, end);
        context->stack = exitStack;
    }
    void forStatement(const Stmt &s) {
        if (!s.elseBody.empty())
            fail(s.location, "for/else is not part of the implemented language");
        bool range = s.expression && s.expression->kind == Expr::Kind::Call &&
                     !s.expression->items.empty() &&
                     s.expression->items[0]->kind == Expr::Kind::Name &&
                     s.expression->items[0]->text == "range";
        int start = integer(0, s.location), end = -1, step = integer(1, s.location), sequence = -1;
        Type element = I64;
        if (range) {
            auto &args = s.expression->items;
            if (args.size() < 2 || args.size() > 4)
                fail(s.location, "range expects end, start/end, or start/end/step");
            std::vector<int> values;
            for (size_t i = 1; i < args.size(); ++i) {
                int value = expr(args[i]);
                require(type(value), I64, args[i]->location, "range argument");
                values.push_back(value);
            }
            if (values.size() == 1)
                end = values[0];
            else {
                start = values[0];
                end = values[1];
            }
            if (values.size() == 3)
                step = values[2];
            int nonzero = binary("!=", step, integer(0, s.location), s.location);
            size_t bad = jump(Op::JumpFalse, nonzero, s.location);
            size_t good = jump(Op::Jump, -1, s.location);
            patch(bad, position());
            emit(Op::Panic, Unit, {string("range step cannot be zero", s.location)}, s.location);
            patch(good, position());
        } else {
            sequence = expr(s.expression);
            Type t = type(sequence);
            if (t.kind == TypeKind::Array)
                element = t.args[0];
            else if (t == String)
                element = I64;
            else
                fail(s.location, "for requires an array, string, or range(...)");
            end = emit(Op::Length, I64, {sequence}, s.location);
        }
        int cursor = emit(Op::Move, I64, {start}, s.location);
        auto carry = freshStack(context->stack, s.location);
        context->stack = carry;
        int head = position();
        int condition = -1;
        if (range) {
            int positive = binary(">", step, integer(0, s.location), s.location);
            size_t negative = jump(Op::JumpFalse, positive, s.location);
            condition = emit(Op::Move, Bool, {binary("<", cursor, end, s.location)}, s.location);
            size_t join = jump(Op::Jump, -1, s.location);
            patch(negative, position());
            int other = binary(">", cursor, end, s.location);
            Instruction move{Op::Move};
            move.dst = condition;
            move.args = {other};
            move.location = s.location;
            fn().code.push_back(move);
            patch(join, position());
        } else
            condition = binary("<", cursor, end, s.location);
        size_t finish = jump(Op::JumpFalse, condition, s.location);
        context->scopes.emplace_back();
        int value = range ? cursor : emit(Op::Index, element, {sequence, cursor}, s.location);
        define(s.name, value, false, s.location);
        // Continue targets the increment, patched after the body is compiled.
        context->loops.push_back({-1, carry, {}, carry});
        block(s.body, false);
        auto loop = context->loops.back();
        context->loops.pop_back();
        context->scopes.pop_back();
        if (!returns(s.body, true))
            copyStack(context->stack, carry, s.location);
        int increment = position();
        for (size_t i = head; i < fn().code.size(); ++i)
            if (fn().code[i].op == Op::Jump && fn().code[i].target == -2)
                patch(i, increment);
        int next = binary("+", cursor, step, s.location);
        Instruction move{Op::Move};
        move.dst = cursor;
        move.args = {next};
        move.location = s.location;
        fn().code.push_back(move);
        size_t back = jump(Op::Jump, -1, s.location);
        patch(back, head);
        int done = position();
        patch(finish, done);
        for (auto index : loop.breaks)
            patch(index, done);
        context->stack = carry;
    }
    bool pattern(const ExprPtr &value, int subject, std::vector<size_t> &failures) {
        if (!value)
            fail(fn().location, "missing match pattern");
        const auto &p = *value;
        Type subjectType = type(subject);
        auto guard = [&](int condition) {
            failures.push_back(jump(Op::JumpFalse, condition, p.location));
        };
        if (p.kind == Expr::Kind::Name) {
            if (p.text != "_")
                define(p.text, subject, false, p.location);
            return true;
        }
        if (p.kind == Expr::Kind::Tuple || p.kind == Expr::Kind::Array) {
            bool tuple = p.kind == Expr::Kind::Tuple;
            if ((tuple && (subjectType.kind != TypeKind::Tuple || !subjectType.name.empty())) ||
                (!tuple && subjectType.kind != TypeKind::Array))
                fail(p.location, "pattern kind does not match subject type");
            if (tuple && p.items.size() != subjectType.args.size())
                fail(p.location, "tuple pattern has the wrong number of fields");
            if (!tuple)
                guard(binary("==", emit(Op::Length, I64, {subject}, p.location),
                             integer(static_cast<int64_t>(p.items.size()), p.location),
                             p.location));
            bool irrefutable = tuple;
            for (size_t i = 0; i < p.items.size(); ++i) {
                Type element = tuple ? subjectType.args[i] : subjectType.args[0];
                int part =
                    emit(Op::Index, element,
                         {subject, integer(static_cast<int64_t>(i), p.location)}, p.location);
                irrefutable = pattern(p.items[i], part, failures) && irrefutable;
            }
            return irrefutable;
        }
        if (p.kind == Expr::Kind::Call && !p.items.empty() &&
            p.items[0]->kind == Expr::Kind::Name) {
            auto name = p.items[0]->text;
            if (!subjectType.name.empty() && enumInstances.count(subjectType.name)) {
                const auto &instance = enumInstances.at(subjectType.name);
                auto variant = constructor(name, p.location);
                if (!variant.first || variant.first != instance.declaration) fail(p.location, "constructor does not belong to the matched enum");
                const auto &fields = variant.first->body.at(variant.second)->parameters;
                if (p.items.size() != fields.size() + 1) fail(p.location, "enum pattern has the wrong number of payloads");
                guard(binary("==", emit(Op::Index, I64, {subject, integer(0, p.location)}, p.location), integer(static_cast<int64_t>(variant.second), p.location), p.location));
                for (size_t i = 0; i < fields.size(); ++i) {
                    size_t offset = instance.offsets.at(variant.second) + i;
                    int field = emit(Op::Index, program.nominalLayouts.at(subjectType.name).args.at(offset), {subject, integer(static_cast<int64_t>(offset), p.location)}, p.location);
                    pattern(p.items[i + 1], field, failures);
                }
                return false;
            }
            if (name == "ok" || name == "err") {
                if (subjectType.kind != TypeKind::Result || p.items.size() != 2)
                    fail(p.location, "Result pattern needs one payload and a Result subject");
                int flag = emit(Op::Index, Bool, {subject, integer(0, p.location)}, p.location);
                if (name == "err")
                    flag = emit(Op::Not, Bool, {flag}, p.location);
                guard(flag);
                size_t field = name == "ok" ? 0 : 1;
                int part = emit(Op::Index, subjectType.args[field],
                                {subject, integer(static_cast<int64_t>(field + 1), p.location)},
                                p.location);
                pattern(p.items[1], part, failures);
                return false;
            }
            if (name == "range") {
                if (subjectType != I64 || p.items.size() != 3)
                    fail(p.location, "range pattern requires an i64 subject and two bounds");
                for (size_t i = 1; i < 3; ++i)
                    if (p.items[i]->kind != Expr::Kind::Integer)
                        fail(p.items[i]->location, "range pattern bounds must be integer literals");
                if (p.items[1]->integer >= p.items[2]->integer)
                    fail(p.location, "range pattern must have increasing bounds");
                guard(binary(">=", subject, integer(p.items[1]->integer, p.location), p.location));
                guard(binary("<", subject, integer(p.items[2]->integer, p.location), p.location));
                return false;
            }
            fail(p.location, "unsupported constructor in match pattern");
        }
        if (p.kind != Expr::Kind::Integer && p.kind != Expr::Kind::Float &&
            p.kind != Expr::Kind::Boolean && p.kind != Expr::Kind::String)
            fail(p.location, "pattern must be a literal, binding, tuple, array, Result, or range");
        guard(binary("==", subject, expr(value), p.location));
        return false;
    }
    void statement(const StmtPtr &statementValue) {
        const auto &s = *statementValue;
        switch (s.kind) {
        case Stmt::Kind::Let:
        case Stmt::Kind::Var: {
            if (s.annotated)
                validateType(s.type, s.location);
            int value = expr(s.expression, s.annotated ? &s.type : nullptr);
            if (s.annotated) {
                if (value < 0)
                    fail(s.location, "cannot bind unit");
                require(type(value), s.type, s.location, "initializer");
            }
            if (s.target && s.target->kind == Expr::Kind::Tuple) {
                if (type(value).kind != TypeKind::Tuple || !type(value).name.empty() ||
                    type(value).args.size() != s.target->items.size())
                    fail(s.location, "tuple destructuring shape mismatch");
                for (size_t i = 0; i < s.target->items.size(); ++i) {
                    auto &item = s.target->items[i];
                    if (item->kind != Expr::Kind::Name)
                        fail(item->location, "destructuring requires names");
                    int part =
                        emit(Op::Index, type(value).args[i],
                             {value, integer(static_cast<int64_t>(i), s.location)}, s.location);
                    define(item->text, part, s.kind == Stmt::Kind::Var, item->location);
                }
            } else
                define(s.name, value, s.kind == Stmt::Kind::Var, s.location);
            break;
        }
        case Stmt::Kind::Assign: {
            auto target = s.target;
            if (!target && !s.name.empty()) {
                target = std::make_shared<Expr>(Expr::Kind::Name, s.location);
                target->text = s.name;
            }
            if (!target)
                fail(s.location, "missing assignment target");
            if (target->kind == Expr::Kind::Name) {
                auto *found = binding(target->text);
                if (!found)
                    fail(s.location, "unknown assignment target '" + target->text + "'");
                Binding b = *found;
                if (!b.mutableValue)
                    fail(s.location,
                         "cannot assign immutable binding '" + target->text + "'; use var");
                int previous = s.operation.empty() ? -1 : emit(Op::LoadCell, b.type, {b.cell}, target->location);
                int value = expr(s.expression, &b.type);
                if (!s.operation.empty()) value = binary(s.operation, previous, value, s.location);
                require(type(value), b.type, s.location, "assignment");
                emit(Op::StoreCell, Unit, {b.cell, value}, s.location);
            } else if (target->kind == Expr::Kind::Index) {
                int base = expr(target->items[0]), index = expr(target->items[1]);
                require(type(index), I64, s.location, "index");
                if (type(base).kind != TypeKind::Array)
                    fail(s.location, "indexed assignment requires an array");
                Type element = type(base).args[0];
                int previous = s.operation.empty() ? -1 : emit(Op::Index, element, {base, index}, target->location);
                int value = expr(s.expression, &element);
                if (!s.operation.empty()) value = binary(s.operation, previous, value, s.location);
                require(type(value), element, s.location, "array assignment");
                emit(Op::SetIndex, Unit, {base, index, value}, s.location);
            } else
                fail(s.location, "invalid assignment target");
            break;
        }
        case Stmt::Kind::Expression: {
            if (context->stackMode && s.expression && s.expression->kind == Expr::Kind::Name) {
                auto *word = declaration(s.expression->text, s.location);
                if (word && word->ast->kind == Stmt::Kind::Word) {
                    stackOperation(s.expression->text, s.location);
                    break;
                }
            }
            int value = expr(s.expression);
            if (context->stackMode && value >= 0)
                context->stack.push_back(value);
            break;
        }
        case Stmt::Kind::StackOp:
            stackOperation(s.name, s.location);
            break;
        case Stmt::Kind::Return: {
            Type result = fn().result;
            int value = expr(s.expression, &result);
            if (value < 0)
                require(Unit, result, s.location, "return");
            else
                require(type(value), result, s.location, "return");
            if (!context->stack.empty())
                fail(s.location,
                     "structured function return leaves values on the shared operand stack");
            emit(Op::Return, Unit, value < 0 ? std::vector<int>{} : std::vector<int>{value},
                 s.location);
            break;
        }
        case Stmt::Kind::If:
            conditional(s);
            break;
        case Stmt::Kind::While:
            whileStatement(s);
            break;
        case Stmt::Kind::For:
            forStatement(s);
            break;
        case Stmt::Kind::Break:
        case Stmt::Kind::Continue: {
            if (context->loops.empty())
                fail(s.location, "loop control outside a loop");
            auto &loop = context->loops.back();
            copyStack(context->stack, s.kind == Stmt::Kind::Break ? loop.exit : loop.carry,
                      s.location);
            size_t branch = jump(Op::Jump, -1, s.location);
            if (s.kind == Stmt::Kind::Break)
                loop.breaks.push_back(branch);
            else
                patch(branch, loop.head < 0 ? -2 : loop.head);
            break;
        }
        case Stmt::Kind::Function: {
            if (!s.typeParameters.empty()) fail(s.location, "generic functions must be declared at module scope");
            if (reserved(s.name))
                fail(s.location, "'" + s.name + "' is reserved for the language or runtime");
            for (const auto &parameter : s.typeParameters)
                if (reserved(parameter)) fail(s.location, "reserved generic type parameter '" + parameter + "'");
            if (context->scopes.back().count(s.name))
                fail(s.location, "duplicate local name '" + s.name + "'");
            Expr lambda(Expr::Kind::Lambda, s.location);
            lambda.parameters = s.parameters;
            lambda.returnType = s.returnType;
            lambda.body = s.body;
            std::vector<Type> parameters;
            for (const auto &parameter : s.parameters)
                parameters.push_back(parameter.type);
            Type callable = Type::function(parameters, s.returnType);
            int cell = emit(Op::MakeCell, Type::cell(callable), {}, s.location);
            fn().localNames[cell] = s.name;
            context->scopes.back()[s.name] = {cell, callable, false};
            int value = closure(lambda);
            emit(Op::StoreCell, Unit, {cell, value}, s.location);
            break;
        }
        case Stmt::Kind::Word:
            fail(s.location, "word declarations must be at module scope");
        case Stmt::Kind::Unsafe: {
            bool previous = context->unsafeMode;
            context->unsafeMode = true;
            block(s.body, true);
            context->unsafeMode = previous;
            break;
        }
        case Stmt::Kind::Extern:
        case Stmt::Kind::Import:
        case Stmt::Kind::Enum:
            fail(s.location, "imports and extern declarations must be at module scope");
        case Stmt::Kind::Match: {
            int subject = expr(s.expression);
            const EnumInstance *matchedEnum = nullptr;
            if (enumInstances.count(type(subject).name)) matchedEnum = &enumInstances.at(type(subject).name);
            std::set<size_t> coveredVariants;
            auto initial = context->stack;
            std::vector<size_t> exits;
            std::vector<int> joined;
            bool haveJoin = false, wildcard = false;
            for (auto &arm : s.body) {
                context->stack = initial;
                std::vector<size_t> next;
                context->scopes.emplace_back();
                if (wildcard)
                    fail(arm->location, "unreachable case after wildcard");
                if (arm->name == "_")
                    wildcard = true;
                else
                    wildcard = pattern(arm->expression, subject, next);
                if (matchedEnum && !arm->other && arm->expression && arm->expression->kind == Expr::Kind::Call && !arm->expression->items.empty()) {
                    auto callee = arm->expression->items[0];
                    if (callee->kind == Expr::Kind::Name) {
                        auto variant = constructor(callee->text, callee->location);
                        bool complete = variant.first == matchedEnum->declaration;
                        if (complete && coveredVariants.count(variant.second)) fail(arm->location, "unreachable enum variant case");
                        for (size_t i = 1; i < arm->expression->items.size(); ++i) complete = complete && irrefutable(arm->expression->items[i]);
                        if (complete) coveredVariants.insert(variant.second);
                    }
                }
                if (arm->other) {
                    auto beforeGuard = context->stack;
                    int condition = expr(arm->other);
                    require(type(condition), Bool, arm->other->location, "match guard");
                    if (context->stack != beforeGuard) fail(arm->other->location, "match guard must preserve the operand stack");
                    next.push_back(jump(Op::JumpFalse, condition, arm->other->location));
                    wildcard = false;
                }
                block(arm->body, false);
                if (!returns(arm->body, true)) {
                    if (!haveJoin) {
                        joined = freshStack(context->stack, arm->location);
                        haveJoin = true;
                    } else
                        copyStack(context->stack, joined, arm->location);
                    exits.push_back(jump(Op::Jump, -1, arm->location));
                }
                context->scopes.pop_back();
                for (size_t branch : next)
                    patch(branch, position());
            }
            std::vector<std::vector<ExprPtr>> patternRows;
            for (const auto &arm : s.body) if (!arm->other) patternRows.push_back({arm->expression});
            unsigned analysisBudget = 8192;
            statementValue->exhaustive = exhaustiveMatrix(patternRows, {type(subject)}, s.location, analysisBudget);
            if (matchedEnum) {
                if (!statementValue->exhaustive) fail(s.location, "non-exhaustive enum match; cover every variant or add an unguarded wildcard");
            }
            if (!wildcard && (statementValue->exhaustive || returns(std::vector<StmtPtr>{statementValue}, true)))
                emit(Op::Panic, Unit, {string("exhaustive match invariant failed", s.location)},
                     s.location);
            else if (!wildcard) {
                if (haveJoin)
                    copyStack(initial, joined, s.location);
                else
                    joined = initial;
            }
            int done = position();
            for (auto exit : exits)
                patch(exit, done);
            context->stack = joined;
            break;
        }
        }
    }
    void block(const std::vector<StmtPtr> &body, bool scope) {
        if (scope)
            context->scopes.emplace_back();
        for (auto &item : body) {
            statement(item);
            if (returns(std::vector<StmtPtr>{item}, true))
                break;
        }
        if (scope)
            context->scopes.pop_back();
    }
    void compileFunction(const std::string &name, const std::vector<Parameter> &parameters,
                         const Type &result, const std::vector<StmtPtr> &body,
                         const std::vector<std::pair<std::string, Binding>> &captured, bool word,
                         Location loc) {
        validateType(result, loc, true);
        for (const auto &parameter : parameters)
            validateType(parameter.type, parameter.location);
        auto previous = std::move(context);
        size_t index = program.functions.size();
        Function function;
        function.name = name;
        function.result = result;
        function.location = loc;
        for (auto &parameter : parameters) {
            function.parameters.push_back(parameter.type);
            function.parameterNames.push_back(parameter.name);
            function.registers.push_back(parameter.type);
        }
        program.functions.push_back(std::move(function));
        context = std::make_unique<Context>();
        context->function = index;
        context->scopes.emplace_back();
        context->stackMode = word;
        for (size_t i = 0; i < captured.size(); ++i) {
            auto &item = captured[i];
            int value = emit(Op::Capture, Type::cell(item.second.type), {}, loc);
            fn().code.back().integer = static_cast<int64_t>(i);
            fn().localNames[value] = item.first;
            context->scopes.back()[item.first] = {value, item.second.type,
                                                  item.second.mutableValue};
        }
        context->scopes.emplace_back();
        for (size_t i = 0; i < parameters.size(); ++i)
            define(parameters[i].name, static_cast<int>(i), false, parameters[i].location);
        if (word)
            for (size_t i = 0; i < parameters.size(); ++i)
                context->stack.push_back(static_cast<int>(i));
        block(body, false);
        if (word && !returns(body)) {
            int value = takeOutputs(result, loc);
            if (!context->stack.empty())
                fail(loc, "word leaves undeclared stack outputs");
            emit(Op::Return, Unit, value < 0 ? std::vector<int>{} : std::vector<int>{value}, loc);
        } else if (!word && !returns(body)) {
            if (result != Unit)
                fail(loc, "function '" + name + "' does not return a value on every path");
            if (!context->stack.empty())
                fail(loc, "function leaves values on shared operand stack");
            emit(Op::Return, Unit, {}, loc);
        }
        context = std::move(previous);
    }

  public:
    explicit Compiler(bool symbols = false) : indexSymbols(symbols) {}
    Program run(const SyntaxTree &tree) {
        program.sourceFile = tree.filename;
        for (const auto &statement : tree.statements)
            if (statement->kind == Stmt::Kind::Import) {
                if (statement->other) {
                    const auto &alias = statement->other->text;
                    if (reserved(alias) || moduleAliases[statement->location.file].count(alias)) fail(statement->location, "duplicate or reserved module alias '" + alias + "'");
                    moduleAliases[statement->location.file][alias] = statement->name;
                } else imports[statement->location.file].insert(statement->name);
            }
        for (const auto &statement : tree.statements) {
            if (statement->kind != Stmt::Kind::Enum) continue;
            const auto &s = *statement;
            const std::string key = s.location.file + "::" + s.name;
            if (reserved(s.name) || enumDeclarations.count(key) || moduleAliases[s.location.file].count(s.name)) fail(s.location, "duplicate or reserved enum name '" + s.name + "'");
            for (const auto &parameter : s.typeParameters) if (reserved(parameter)) fail(s.location, "reserved enum type parameter '" + parameter + "'");
            enumDeclarations[key] = statement;
            if (s.exported) exportedEnums[s.name].push_back(key);
            for (size_t i = 0; i < s.body.size(); ++i) {
                const auto &variant = s.body[i];
                std::string variantKey = s.location.file + "::" + variant->name;
                if (reserved(variant->name) || constructors.count(variantKey) || moduleAliases[s.location.file].count(variant->name)) fail(variant->location, "duplicate or reserved constructor name '" + variant->name + "'");
                constructors[variantKey] = {key, i};
                if (s.exported) exportedConstructors[variant->name].push_back(variantKey);
            }
        }
        for (const auto &item : enumDeclarations)
            if (item.second->typeParameters.empty()) enumType(item.second, {}, item.second->location, 0);
        for (auto &statement : tree.statements) {
            auto resolvedStatement = statement;
            std::vector<CABIType> parameterABI;
            CABIType resultABI;
            if (statement->kind == Stmt::Kind::Extern) {
                resolvedStatement = std::make_shared<Stmt>(*statement);
                auto normalizedResult = externType(statement->returnType, statement->location);
                resolvedStatement->returnType = normalizedResult.first; resultABI = normalizedResult.second;
                for (auto &parameter : resolvedStatement->parameters) {
                    auto normalized = externType(parameter.type, parameter.location);
                    parameter.type = normalized.first; parameterABI.push_back(normalized.second);
                }
            }
            auto &s = *resolvedStatement;
            if (s.kind == Stmt::Kind::Enum) continue;
            if (s.kind == Stmt::Kind::Import) {
                continue;
            }
            if (s.kind == Stmt::Kind::Extern) {
                validateType(s.returnType, s.location, true);
                for (const auto &parameter : s.parameters)
                    validateType(parameter.type, parameter.location);
                if (reserved(s.name) || moduleAliases[s.location.file].count(s.name))
                    fail(s.location, "extern name '" + s.name +
                                         "' is reserved; declare a distinct local name with = "
                                         "\"native_symbol\"");
                if (s.returnType != Unit && !cType(s.returnType))
                    fail(s.location, "extern return requires a scalar, pointer, or flat "
                                     "C-compatible tuple record");
                External ext;
                ext.name = "extern_" + std::to_string(program.externals.size()) + "_" + s.name;
                ext.symbol = s.expression ? s.expression->text : s.name;
                ext.result = s.returnType;
                ext.variadic = s.variadic;
                ext.abiParameters = parameterABI;
                ext.abiResult = resultABI;
                for (auto &p : s.parameters) {
                    if (!cType(p.type))
                        fail(p.location, "extern argument requires a scalar, pointer, or flat "
                                         "C-compatible tuple record");
                    ext.parameters.push_back(p.type);
                }
                std::string externalKey = s.location.file + "::" + s.name;
                if (externals.count(externalKey) || byModule.count(externalKey) || constructors.count(externalKey))
                    fail(s.location, "duplicate extern declaration");
                for (const auto &previous : program.externals)
                    if (previous.symbol == ext.symbol && !sameExternalABI(previous, ext))
                        fail(s.location,
                             "conflicting declarations for native symbol '" + ext.symbol + "'");
                externals[externalKey] = ext;
                if (s.exported)
                    exportedExternals[s.name].push_back({s.location.file, externalKey});
                if (s.exported) exportedExternalKeys.insert(externalKey);
                program.externals.push_back(ext);
                continue;
            }
            if (s.kind != Stmt::Kind::Function && s.kind != Stmt::Kind::Word)
                fail(s.location,
                     "module scope accepts functions, words, imports and extern declarations");
            for (const auto &parameter : s.typeParameters)
                if (reserved(parameter)) fail(s.location, "reserved generic type parameter '" + parameter + "'");
            if (reserved(s.name) || moduleAliases[s.location.file].count(s.name))
                fail(s.location, "'" + s.name + "' is reserved for the language or runtime");
            std::string key = s.location.file + "::" + s.name;
            if (byModule.count(key) || externals.count(key) || constructors.count(key))
                fail(s.location, "duplicate function or word '" + s.name + "'");
            auto definition = statement;
            if (s.typeParameters.empty()) {
                definition = std::make_shared<Stmt>(*statement);
                definition->returnType = substitute(s.returnType, {}, s.location);
                for (auto &parameter : definition->parameters) parameter.type = substitute(parameter.type, {}, parameter.location);
            }
            std::vector<Type> params;
            for (auto &parameter : definition->parameters)
                params.push_back(parameter.type);
            std::string irName = "m" + std::to_string(declarations.size()) + "_" + s.name;
            if (s.name == "main" && s.location.file == tree.filename) {
                irName = "main";
                if (!params.empty() || (s.returnType != Unit && s.returnType != I64))
                    fail(s.location, "main must take no parameters and return unit or i64");
            }
            size_t index = declarations.size();
            byModule[key] = index;
            if (s.exported)
                exported[s.name].push_back(index);
            declarations.push_back(
                {definition, irName, s.location.file, Type::function(params, definition->returnType)});
        }
        if (!byModule.count(tree.filename + "::main"))
            fail({tree.filename, 1, 1}, "program requires fun main() -> unit or i64");
        std::vector<std::pair<Location, std::string>> errors;
        auto keys = [](const auto &map) {
            std::set<std::string> result;
            for (const auto &entry : map) result.insert(entry.first);
            return result;
        };
        auto restoreKeys = [](auto &map, const auto &before) {
            for (auto entry = map.begin(); entry != map.end(); )
                if (!before.count(entry->first)) entry = map.erase(entry);
                else ++entry;
        };
        for (auto &d : declarations) {
            if (!d.ast->typeParameters.empty()) continue;
            const size_t functionCount = program.functions.size();
            const auto specializedBefore = keys(specializations);
            const auto enumBefore = keys(enumInstances);
            const auto layoutsBefore = keys(program.nominalLayouts);
            const auto serialBefore = serial;
            const auto depthBefore = instantiationDepth;
            const auto budgetBefore = typeExpansionBudget;
            try {
                auto definition = cloneStmt(d.ast, {});
                compileFunction(d.irName, definition->parameters, definition->returnType, definition->body, {},
                                definition->kind == Stmt::Kind::Word, definition->location);
            } catch (const Error &error) {
                errors.emplace_back(error.location, error.what());
                program.functions.resize(functionCount);
                restoreKeys(specializations, specializedBefore);
                restoreKeys(enumInstances, enumBefore);
                restoreKeys(program.nominalLayouts, layoutsBefore);
                context.reset(); serial = serialBefore; instantiationDepth = depthBefore; typeExpansionBudget = budgetBefore;
                if (errors.size() == 20) break;
            }
        }
        if (!errors.empty()) {
            std::string message = errors.front().second;
            for (size_t i = 1; i < errors.size(); ++i)
                message += "\n" + errors[i].first.file + ":" + std::to_string(errors[i].first.line) + ":" +
                           std::to_string(errors[i].first.column) + ": error: " + errors[i].second;
            if (errors.size() == 20) message += "\nstopped after 20 semantic function errors";
            throw Error(errors.front().first, message);
        }
        verify(program);
        return std::move(program);
    }
};
} // namespace
Program compile(const SyntaxTree &tree, bool indexSymbols) {
    return Compiler(indexSymbols).run(tree);
}
} // namespace pika
