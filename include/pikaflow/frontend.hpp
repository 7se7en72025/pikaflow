#pragma once
#include "pikaflow/common.hpp"

namespace pika {
enum class TokenKind { Name, Integer, Float, String, Symbol, Newline, Indent, Dedent, End };
struct Token { TokenKind kind; std::string text; Location location; };
std::vector<Token> lex(const std::string& source, const std::string& filename);
struct Expr;
struct Stmt;
using ExprPtr = std::shared_ptr<Expr>;
using StmtPtr = std::shared_ptr<Stmt>;
struct Parameter { std::string name; Type type; Location location; bool synthetic = false; };
struct Expr {
    enum class Kind { Integer, Float, Boolean, String, Name, Unary, Binary, Call,
        Index, Array, Tuple, Lambda, Stack, TypeApply, Propagate };
    Kind kind;
    Location location;
    Location nameLocation;
    std::string text;
    int64_t integer = 0;
    double real = 0;
    std::vector<ExprPtr> items;
    std::vector<Parameter> parameters;
    std::vector<Type> typeArguments;
    Type returnType;
    std::vector<StmtPtr> body;
    explicit Expr(Kind k, Location l = {}) : kind(k), location(std::move(l)) {}
};
struct Stmt {
    enum class Kind { Function, Word, Let, Var, Assign, Expression, Return, If,
        While, For, Import, Extern, Break, Continue, StackOp, Match, Unsafe, Enum };
    Kind kind;
    Location location;
    Location nameLocation;
    std::string name;
    std::string operation;
    Type type;
    bool annotated = false, exported = false, variadic = false;
    bool exhaustive = false;
    std::vector<Parameter> parameters;
    std::vector<std::string> typeParameters;
    std::vector<Location> typeParameterLocations;
    Type returnType;
    ExprPtr expression, target, other;
    std::vector<StmtPtr> body, elseBody;
    explicit Stmt(Kind k, Location l = {}) : kind(k), location(std::move(l)) {}
};
struct SyntaxTree { std::vector<StmtPtr> statements; std::string filename; };
SyntaxTree parse(const std::vector<Token>& tokens, const std::string& filename);
std::string dumpTokens(const std::vector<Token>& tokens);
std::string dumpAST(const SyntaxTree& tree);
}
