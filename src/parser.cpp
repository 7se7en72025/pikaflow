#include "pikaflow/frontend.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace pika {
namespace {
class Parser {
    const std::vector<Token>& tokens;
    std::string filename;
    size_t position = 0;
    const Token& peek(size_t ahead = 0) const {
        return tokens[std::min(position + ahead, tokens.size() - 1)];
    }
    bool is(TokenKind kind) const { return peek().kind == kind; }
    bool is(const std::string& text) const { return peek().text == text; }
    Token take() {
        Token token = peek();
        if (position + 1 < tokens.size()) ++position;
        return token;
    }
    bool accept(const std::string& text) { if (!is(text)) return false; take(); return true; }
    Token expect(const std::string& text) {
        if (!is(text)) throw Error(peek().location, "expected '" + text + "', found " + description(peek()));
        return take();
    }
    Token expect(TokenKind kind, const std::string& what) {
        if (!is(kind)) throw Error(peek().location, "expected " + what + ", found " + description(peek()));
        return take();
    }
    static std::string description(const Token& token) {
        switch (token.kind) {
        case TokenKind::Newline: return "end of line";
        case TokenKind::Indent: return "an indented block";
        case TokenKind::Dedent: return "end of block";
        case TokenKind::End: return "end of file";
        default: return "'" + token.text + "'";
        }
    }
    void separators() { while (is(TokenKind::Newline) || is(";")) take(); }
    bool lineEnd() const { return is(TokenKind::Newline) || is(TokenKind::Dedent) || is(TokenKind::End) || is(";"); }
    void finishSimple() {
        if (accept(";")) return;
        if (is(TokenKind::Newline)) { take(); return; }
        if (is(TokenKind::Dedent) || is(TokenKind::End)) return;
        // A multiline stack or lambda expression already consumed its final DEDENT.
        if (position && tokens[position - 1].kind == TokenKind::Dedent) return;
        throw Error(peek().location, "expected end of statement, found " + description(peek()));
    }
    Type type() {
        if (accept("[")) { Type element = type(); expect("]"); return Type::array(std::move(element)); }
        if (accept("(")) {
            std::vector<Type> elements;
            if (!is(")")) { do { elements.push_back(type()); } while (accept(",") && !is(")")); }
            expect(")");
            return elements.empty() ? Unit : Type{TypeKind::Tuple, std::move(elements)};
        }
        Token token = expect(TokenKind::Name, "a type");
        if (token.text == "i64" || token.text == "int") return I64;
        if (token.text == "f64" || token.text == "float") return F64;
        if (token.text == "bool") return Bool;
        if (token.text == "unit" || token.text == "void") return Unit;
        if (token.text == "str" || token.text == "string") return String;
        if (token.text == "array") { expect("["); Type element = type(); expect("]"); return Type::array(std::move(element)); }
        if (token.text == "Result") {
            expect("["); Type success = type(); expect(","); Type failure = type(); expect("]");
            return {TypeKind::Result, {std::move(success), std::move(failure)}};
        }
        if (token.text == "ptr") {
            if (!accept("[")) return {TypeKind::Pointer};
            Type element = type(); expect("]"); return {TypeKind::Pointer, {std::move(element)}};
        }
        if (token.text == "fn" || token.text == "fun") {
            expect("(");
            std::vector<Type> parameters;
            if (!is(")")) { do { parameters.push_back(type()); } while (accept(",") && !is(")")); }
            expect(")"); expect("->");
            return Type::function(std::move(parameters), type());
        }
        throw Error(token.location, "unknown type '" + token.text + "'");
    }
    std::vector<Parameter> parameters(bool word, bool allowVariadic, bool& variadic) {
        expect("(");
        std::vector<Parameter> result;
        if (!is(")")) {
            for (;;) {
                if (accept("...")) {
                    if (!allowVariadic) throw Error(peek().location, "variadic parameters are only allowed on extern functions");
                    variadic = true;
                    break;
                }
                Location at = peek().location;
                std::string name;
                Type parameterType;
                if (word && peek(1).text != ":") {
                    name = "arg" + std::to_string(result.size());
                    parameterType = type();
                } else {
                    name = expect(TokenKind::Name, "a parameter name").text;
                    expect(":");
                    parameterType = type();
                }
                for (const auto& existing : result) if (existing.name == name) throw Error(at, "duplicate parameter '" + name + "'");
                result.push_back({std::move(name), std::move(parameterType), at});
                if (!accept(",")) break;
                if (is(")")) break;
            }
        }
        expect(")");
        return result;
    }
    std::vector<StmtPtr> suite(bool stack = false) {
        expect(":");
        expect(TokenKind::Newline, "a newline after ':'");
        while (is(TokenKind::Newline)) take();
        expect(TokenKind::Indent, "an indented block");
        std::vector<StmtPtr> body;
        separators();
        while (!is(TokenKind::Dedent) && !is(TokenKind::End)) {
            const size_t before = position;
            if (accept("pass")) finishSimple();
            else if (stack) stackStatement(body);
            else body.push_back(statement());
            if (position == before) throw Error(peek().location, "parser made no progress");
            separators();
        }
        expect(TokenKind::Dedent, "end of indented block");
        return body;
    }
    int precedence(const std::string& op) const {
        if (op == "|>") return 1;
        if (op == "or" || op == "||") return 2;
        if (op == "and" || op == "&&") return 3;
        if (op == "==" || op == "!=") return 4;
        if (op == "<" || op == "<=" || op == ">" || op == ">=") return 5;
        if (op == "+" || op == "-") return 6;
        if (op == "*" || op == "/" || op == "%") return 7;
        return 0;
    }
    ExprPtr expression(int minimum = 0) {
        ExprPtr left = unary();
        for (;;) {
            const int binding = precedence(peek().text);
            if (binding <= minimum) break;
            Token op = take();
            auto result = std::make_shared<Expr>(Expr::Kind::Binary, op.location);
            result->text = op.text;
            result->items = {left, expression(binding)};
            left = std::move(result);
        }
        return left;
    }
    ExprPtr unary() {
        if (is("-") || is("+") || is("not") || is("!")) {
            Token op = take();
            if (op.text == "-" && is(TokenKind::Integer)) {
                Token number = take();
                try {
                    const auto magnitude = std::stoull(number.text);
                    if (magnitude > uint64_t(std::numeric_limits<int64_t>::max()) + 1) throw std::out_of_range("i64");
                    auto result = std::make_shared<Expr>(Expr::Kind::Integer, op.location);
                    result->integer = magnitude == uint64_t(std::numeric_limits<int64_t>::max()) + 1
                        ? std::numeric_limits<int64_t>::min() : -static_cast<int64_t>(magnitude);
                    return postfix(std::move(result));
                } catch (const std::exception&) { throw Error(number.location, "integer literal is outside the i64 range"); }
            }
            auto result = std::make_shared<Expr>(Expr::Kind::Unary, op.location);
            result->text = op.text;
            result->items.push_back(unary());
            return result;
        }
        return postfix(primary());
    }
    ExprPtr postfix(ExprPtr left) {
        for (;;) {
            if (is("(")) {
                Token open = take();
                auto call = std::make_shared<Expr>(Expr::Kind::Call, open.location);
                call->items.push_back(std::move(left));
                if (!is(")")) { do { call->items.push_back(expression()); } while (accept(",") && !is(")")); }
                expect(")"); left = std::move(call);
            } else if (is("[")) {
                Token open = take();
                auto index = std::make_shared<Expr>(Expr::Kind::Index, open.location);
                index->items = {left, expression()};
                expect("]"); left = std::move(index);
            } else break;
        }
        return left;
    }
    ExprPtr primary() {
        Token token = take();
        if (token.kind == TokenKind::Integer) {
            auto result = std::make_shared<Expr>(Expr::Kind::Integer, token.location);
            try { result->integer = std::stoll(token.text); }
            catch (const std::exception&) { throw Error(token.location, "integer literal is outside the i64 range"); }
            return result;
        }
        if (token.kind == TokenKind::Float) {
            auto result = std::make_shared<Expr>(Expr::Kind::Float, token.location);
            try { result->real = std::stod(token.text); }
            catch (const std::exception&) { throw Error(token.location, "floating-point literal is outside the f64 range"); }
            if (!std::isfinite(result->real)) throw Error(token.location, "floating-point literal is outside the finite f64 range");
            return result;
        }
        if (token.kind == TokenKind::String) {
            auto result = std::make_shared<Expr>(Expr::Kind::String, token.location); result->text = token.text; return result;
        }
        if (token.text == "true" || token.text == "false") {
            auto result = std::make_shared<Expr>(Expr::Kind::Boolean, token.location); result->integer = token.text == "true"; return result;
        }
        if (token.text == "fun" || token.text == "fn") {
            auto result = std::make_shared<Expr>(Expr::Kind::Lambda, token.location);
            bool variadic = false;
            result->parameters = parameters(false, false, variadic);
            if (accept("->")) result->returnType = type();
            result->body = suite();
            return result;
        }
        if (token.text == "stack") {
            auto result = std::make_shared<Expr>(Expr::Kind::Stack, token.location);
            if (accept("(")) {
                if (!is(")")) { do { result->items.push_back(expression()); } while (accept(",") && !is(")")); }
                expect(")");
            }
            if (accept("->")) result->returnType = type();
            result->body = suite(true);
            return result;
        }
        if (token.text == "(") {
            if (accept(")")) return std::make_shared<Expr>(Expr::Kind::Tuple, token.location);
            ExprPtr first = expression();
            if (!accept(",")) { expect(")"); return first; }
            auto result = std::make_shared<Expr>(Expr::Kind::Tuple, token.location);
            result->items.push_back(std::move(first));
            if (!is(")")) { do { result->items.push_back(expression()); } while (accept(",") && !is(")")); }
            expect(")"); return result;
        }
        if (token.text == "[") {
            auto result = std::make_shared<Expr>(Expr::Kind::Array, token.location);
            if (!is("]")) { do { result->items.push_back(expression()); } while (accept(",") && !is("]")); }
            expect("]"); return result;
        }
        if (token.kind == TokenKind::Name) {
            auto result = std::make_shared<Expr>(Expr::Kind::Name, token.location); result->text = token.text; return result;
        }
        throw Error(token.location, "expected an expression, found " + description(token));
    }
    ExprPtr returnValues() {
        ExprPtr first = expression();
        if (!accept(",")) return first;
        auto result = std::make_shared<Expr>(Expr::Kind::Tuple, first->location);
        result->items.push_back(std::move(first));
        do { result->items.push_back(expression()); } while (accept(","));
        return result;
    }
    StmtPtr conditional(Token start, bool stack) {
        auto result = std::make_shared<Stmt>(Stmt::Kind::If, start.location);
        if (!is(":")) result->expression = expression();
        result->body = suite(stack);
        if (is("elif")) result->elseBody.push_back(conditional(take(), stack));
        else if (accept("else")) result->elseBody = suite(stack);
        return result;
    }
    StmtPtr matchStatement(Token start, bool stack) {
        auto result = std::make_shared<Stmt>(Stmt::Kind::Match, start.location);
        result->expression = expression();
        expect(":"); expect(TokenKind::Newline, "a newline after ':'");
        while (is(TokenKind::Newline)) take();
        expect(TokenKind::Indent, "indented match cases");
        separators();
        bool wildcardSeen = false;
        while (!is(TokenKind::Dedent) && !is(TokenKind::End)) {
            Token caseToken = expect("case");
            if (wildcardSeen) throw Error(caseToken.location, "wildcard case must be the final match case");
            auto arm = std::make_shared<Stmt>(Stmt::Kind::If, caseToken.location);
            if (accept("_")) { arm->name = "_"; wildcardSeen = true; }
            else arm->expression = expression();
            arm->body = suite(stack);
            result->body.push_back(std::move(arm));
            separators();
        }
        expect(TokenKind::Dedent, "end of match cases");
        if (result->body.empty()) throw Error(start.location, "match requires at least one case");
        return result;
    }
    StmtPtr statement(bool stack = false) {
        Token start = peek();
        if (accept("export")) {
            auto result = statement(stack);
            if (result->kind != Stmt::Kind::Function && result->kind != Stmt::Kind::Word
                && result->kind != Stmt::Kind::Let && result->kind != Stmt::Kind::Var)
                throw Error(start.location, "only functions, words and variables may be exported");
            result->exported = true;
            return result;
        }
        if (is("fun") || is("fn") || is("word") || is("extern")) {
            bool external = accept("extern");
            bool word = false;
            if (external) {
                if (!accept("fun") && !accept("fn")) throw Error(peek().location, "expected 'fun' after 'extern'");
            } else word = take().text == "word";
            auto result = std::make_shared<Stmt>(external ? Stmt::Kind::Extern : word ? Stmt::Kind::Word : Stmt::Kind::Function, start.location);
            result->name = expect(TokenKind::Name, "a function name").text;
            result->parameters = parameters(word, external, result->variadic);
            if (accept("->")) result->returnType = type();
            if (external) finishSimple();
            else result->body = suite(word);
            return result;
        }
        if (is("let") || is("var")) {
            bool mutableVariable = take().text == "var";
            auto result = std::make_shared<Stmt>(mutableVariable ? Stmt::Kind::Var : Stmt::Kind::Let, start.location);
            if (accept("(")) {
                auto target = std::make_shared<Expr>(Expr::Kind::Tuple, start.location);
                do {
                    Token name = expect(TokenKind::Name, "a destructured variable name");
                    auto value = std::make_shared<Expr>(Expr::Kind::Name, name.location); value->text = name.text;
                    target->items.push_back(std::move(value));
                } while (accept(",") && !is(")"));
                expect(")"); result->target = std::move(target);
            } else result->name = expect(TokenKind::Name, "a variable name").text;
            if (accept(":")) { result->type = type(); result->annotated = true; }
            expect("="); result->expression = expression(); finishSimple(); return result;
        }
        if (accept("return")) {
            auto result = std::make_shared<Stmt>(Stmt::Kind::Return, start.location);
            if (!lineEnd()) result->expression = returnValues();
            finishSimple(); return result;
        }
        if (accept("if")) return conditional(start, stack);
        if (accept("while")) {
            auto result = std::make_shared<Stmt>(Stmt::Kind::While, start.location);
            if (!is(":")) result->expression = expression();
            result->body = suite(stack);
            if (accept("else")) result->elseBody = suite(stack);
            return result;
        }
        if (accept("for")) {
            auto result = std::make_shared<Stmt>(Stmt::Kind::For, start.location);
            result->name = expect(TokenKind::Name, "a loop variable").text;
            expect("in"); result->expression = expression(); result->body = suite(stack);
            if (accept("else")) result->elseBody = suite(stack);
            return result;
        }
        if (accept("match")) return matchStatement(start, stack);
        if (accept("break") || accept("continue")) {
            auto result = std::make_shared<Stmt>(start.text == "break" ? Stmt::Kind::Break : Stmt::Kind::Continue, start.location);
            finishSimple(); return result;
        }
        if (accept("import")) {
            auto result = std::make_shared<Stmt>(Stmt::Kind::Import, start.location);
            result->name = expect(TokenKind::String, "an import path string").text;
            if (accept("as")) result->other = nameExpression(expect(TokenKind::Name, "an import alias"));
            finishSimple(); return result;
        }
        ExprPtr left = expression();
        if (is("=") || is("+=") || is("-=") || is("*=") || is("/=") || is("%=")) {
            Token assignment = take();
            if (left->kind != Expr::Kind::Name && left->kind != Expr::Kind::Index)
                throw Error(left->location, "assignment target must be a variable or array element");
            auto result = std::make_shared<Stmt>(Stmt::Kind::Assign, start.location);
            result->target = left;
            if (left->kind == Expr::Kind::Name) result->name = left->text;
            result->expression = expression();
            if (assignment.text != "=") {
                if (left->kind == Expr::Kind::Index) throw Error(assignment.location, "compound indexed assignment is not supported; use an explicit assignment");
                auto binary = std::make_shared<Expr>(Expr::Kind::Binary, assignment.location);
                binary->text = assignment.text.substr(0, 1); binary->items = {left, result->expression};
                result->expression = std::move(binary);
            }
            finishSimple(); return result;
        }
        auto result = std::make_shared<Stmt>(Stmt::Kind::Expression, start.location);
        result->expression = std::move(left); finishSimple(); return result;
    }
    static ExprPtr nameExpression(const Token& token) {
        auto result = std::make_shared<Expr>(Expr::Kind::Name, token.location); result->text = token.text; return result;
    }
    void stackStatement(std::vector<StmtPtr>& body) {
        if (is("if") || is("while") || is("for") || is("match") || is("let") || is("var")
            || is("return") || is("break") || is("continue") || is("fun") || is("word")) {
            body.push_back(statement(true)); return;
        }
        const Token token = peek();
        if (token.text == ";") { take(); return; }
        const bool negativeLiteral = token.text == "-" && (peek(1).kind == TokenKind::Integer || peek(1).kind == TokenKind::Float)
            && token.location.line == peek(1).location.line
            && token.location.column + 1 == peek(1).location.column;
        if (negativeLiteral) {
            auto stmt = std::make_shared<Stmt>(Stmt::Kind::Expression, token.location);
            stmt->expression = unary(); body.push_back(std::move(stmt)); return;
        }
        const std::string builtins = " dup swap drop over rot print dump gc neg not len and or ";
        const bool namedOperation = token.kind == TokenKind::Name && builtins.find(" " + token.text + " ") != std::string::npos
            && peek(1).text != "(" && peek(1).text != "[";
        const bool operatorOperation = token.kind == TokenKind::Symbol && token.text != "(" && token.text != "[";
        if (namedOperation || operatorOperation) {
            take();
            auto stmt = std::make_shared<Stmt>(Stmt::Kind::StackOp, token.location);
            stmt->name = token.text; body.push_back(std::move(stmt)); return;
        }
        auto stmt = std::make_shared<Stmt>(Stmt::Kind::Expression, token.location);
        stmt->expression = postfix(primary()); body.push_back(std::move(stmt));
    }
public:
    Parser(const std::vector<Token>& input, std::string file) : tokens(input), filename(std::move(file)) {}
    SyntaxTree run() {
        if (tokens.empty()) throw Error({filename, 1, 1}, "empty token stream");
        SyntaxTree result; result.filename = filename;
        separators();
        while (!is(TokenKind::End)) {
            if (is(TokenKind::Indent)) throw Error(peek().location, "unexpected indentation at top level");
            if (is(TokenKind::Dedent)) throw Error(peek().location, "unexpected end of block");
            result.statements.push_back(statement());
            separators();
        }
        return result;
    }
};

const char* expressionName(Expr::Kind kind) {
    switch (kind) {
    case Expr::Kind::Integer: return "Integer";
    case Expr::Kind::Float: return "Float";
    case Expr::Kind::Boolean: return "Boolean";
    case Expr::Kind::String: return "String";
    case Expr::Kind::Name: return "Name";
    case Expr::Kind::Unary: return "Unary";
    case Expr::Kind::Binary: return "Binary";
    case Expr::Kind::Call: return "Call";
    case Expr::Kind::Index: return "Index";
    case Expr::Kind::Array: return "Array";
    case Expr::Kind::Tuple: return "Tuple";
    case Expr::Kind::Lambda: return "Lambda";
    case Expr::Kind::Stack: return "Stack";
    }
    return "Unknown";
}
const char* statementName(Stmt::Kind kind) {
    switch (kind) {
    case Stmt::Kind::Function: return "Function";
    case Stmt::Kind::Word: return "Word";
    case Stmt::Kind::Let: return "Let";
    case Stmt::Kind::Var: return "Var";
    case Stmt::Kind::Assign: return "Assign";
    case Stmt::Kind::Expression: return "Expression";
    case Stmt::Kind::Return: return "Return";
    case Stmt::Kind::If: return "If";
    case Stmt::Kind::While: return "While";
    case Stmt::Kind::For: return "For";
    case Stmt::Kind::Import: return "Import";
    case Stmt::Kind::Extern: return "Extern";
    case Stmt::Kind::Break: return "Break";
    case Stmt::Kind::Continue: return "Continue";
    case Stmt::Kind::StackOp: return "StackOp";
    case Stmt::Kind::Match: return "Match";
    }
    return "Unknown";
}
void dumpStatements(std::ostream&, const std::vector<StmtPtr>&, unsigned);
void dumpExpression(std::ostream& out, const ExprPtr& expr, unsigned depth) {
    if (!expr) return;
    out << std::string(depth * 2, ' ') << expressionName(expr->kind);
    if (expr->kind == Expr::Kind::Integer) out << ' ' << expr->integer;
    else if (expr->kind == Expr::Kind::Float) out << ' ' << std::setprecision(17) << expr->real;
    else if (expr->kind == Expr::Kind::Boolean) out << ' ' << (expr->integer ? "true" : "false");
    else if (!expr->text.empty()) out << ' ' << std::quoted(expr->text);
    if (expr->kind == Expr::Kind::Stack || expr->kind == Expr::Kind::Lambda) out << " -> " << expr->returnType.str();
    out << " @" << expr->location.line << ':' << expr->location.column << '\n';
    for (const auto& item : expr->items) dumpExpression(out, item, depth + 1);
    for (const auto& parameter : expr->parameters) out << std::string((depth + 1) * 2, ' ') << "Parameter " << parameter.name << ": " << parameter.type.str() << '\n';
    dumpStatements(out, expr->body, depth + 1);
}
void dumpStatements(std::ostream& out, const std::vector<StmtPtr>& stmts, unsigned depth) {
    for (const auto& stmt : stmts) {
        out << std::string(depth * 2, ' ') << statementName(stmt->kind);
        if (!stmt->name.empty()) out << ' ' << stmt->name;
        if (stmt->exported) out << " [export]";
        if (stmt->annotated) out << ": " << stmt->type.str();
        if (stmt->kind == Stmt::Kind::Function || stmt->kind == Stmt::Kind::Word || stmt->kind == Stmt::Kind::Extern) out << " -> " << stmt->returnType.str();
        out << " @" << stmt->location.line << ':' << stmt->location.column << '\n';
        for (const auto& parameter : stmt->parameters) out << std::string((depth + 1) * 2, ' ') << "Parameter " << parameter.name << ": " << parameter.type.str() << '\n';
        if (stmt->target) { out << std::string((depth + 1) * 2, ' ') << "Target\n"; dumpExpression(out, stmt->target, depth + 2); }
        dumpExpression(out, stmt->expression, depth + 1);
        dumpExpression(out, stmt->other, depth + 1);
        dumpStatements(out, stmt->body, depth + 1);
        if (!stmt->elseBody.empty()) { out << std::string((depth + 1) * 2, ' ') << "Else\n"; dumpStatements(out, stmt->elseBody, depth + 2); }
    }
}
}

SyntaxTree parse(const std::vector<Token>& tokens, const std::string& filename) { return Parser(tokens, filename).run(); }
std::string dumpAST(const SyntaxTree& tree) {
    std::ostringstream out; out << "Module " << tree.filename << '\n';
    dumpStatements(out, tree.statements, 1); return out.str();
}
}
