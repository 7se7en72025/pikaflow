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
    std::vector<unsigned> indentation;
    unsigned nesting = 0;
    struct NestingGuard {
        unsigned& depth;
        NestingGuard(unsigned& value, const Location& location) : depth(value) {
            if (++depth > 128) throw Error(location, "maximum parser nesting depth exceeded");
        }
        ~NestingGuard() { --depth; }
    };
    const Token& peek(size_t ahead = 0) const {
        return tokens[std::min(position + ahead, tokens.size() - 1)];
    }
    bool is(TokenKind kind) const { return peek().kind == kind; }
    bool is(const std::string& text) const {
        return (peek().kind == TokenKind::Name || peek().kind == TokenKind::Symbol) && peek().text == text;
    }
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
        NestingGuard guard(nesting, peek().location);
        if (accept("[")) { Type element = type(); expect("]"); return Type::array(std::move(element)); }
        if (accept("(")) {
            std::vector<Type> elements;
            if (!is(")")) { do { elements.push_back(type()); } while (accept(",") && !is(")")); }
            expect(")");
            return elements.empty() ? Unit : Type{TypeKind::Tuple, std::move(elements)};
        }
        Token token = expect(TokenKind::Name, "a type");
        const Location firstName = token.location;
        Location lastName = token.location;
        while (accept(".")) {
            auto member = expect(TokenKind::Name, "a qualified type name");
            token.text += "." + member.text; lastName = member.location;
        }
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
        if (token.text == "task") {
            expect("["); Type result = type(); expect("]");
            return {TypeKind::Task, {std::move(result)}};
        }
        Type named{TypeKind::Named, {}, token.text};
        named.sourceLocation = lastName;
        if (firstName.column != lastName.column || firstName.line != lastName.line) named.qualifierLocation = firstName;
        if (accept("[")) {
            do { named.args.push_back(type()); } while (accept(","));
            expect("]");
        }
        return named;
    }
    std::vector<std::string> typeParameters(std::vector<Location> *locations = nullptr) {
        std::vector<std::string> result;
        if (!accept("[")) return result;
        do {
            auto parameter = expect(TokenKind::Name, "a type parameter");
            if (std::find(result.begin(), result.end(), parameter.text) != result.end())
                throw Error(parameter.location, "duplicate type parameter '" + parameter.text + "'");
            result.push_back(parameter.text);
            if (locations) locations->push_back(parameter.location);
        } while (accept(","));
        expect("]");
        return result;
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
                bool synthetic = false;
                if (word && peek(1).text != ":") {
                    name = "arg" + std::to_string(result.size());
                    synthetic = true;
                    parameterType = type();
                } else {
                    name = expect(TokenKind::Name, "a parameter name").text;
                    expect(":");
                    parameterType = type();
                }
                for (const auto& existing : result) if (existing.name == name) throw Error(at, "duplicate parameter '" + name + "'");
                result.push_back({std::move(name), std::move(parameterType), at, synthetic});
                if (!accept(",")) break;
                if (is(")")) break;
            }
        }
        expect(")");
        return result;
    }
    std::vector<StmtPtr> suite(bool stack = false) {
        NestingGuard guard(nesting, peek().location);
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
    ExprPtr checkedExpression(ExprPtr value) {
        value->syntaxDepth = 1;
        for (const auto& item : value->items)
            value->syntaxDepth = std::max(value->syntaxDepth, item->syntaxDepth + 1);
        if (value->syntaxDepth > 128)
            throw Error(value->location, "maximum expression syntax depth (128) exceeded");
        return value;
    }
    ExprPtr expression(int minimum = 0) {
        NestingGuard guard(nesting, peek().location);
        ExprPtr left = unary();
        for (;;) {
            const int binding = peek().kind == TokenKind::Name || peek().kind == TokenKind::Symbol ? precedence(peek().text) : 0;
            if (binding <= minimum) break;
            Token op = take();
            auto result = std::make_shared<Expr>(Expr::Kind::Binary, op.location);
            result->text = op.text;
            result->items = {left, expression(binding)};
            left = checkedExpression(std::move(result));
        }
        return checkedExpression(std::move(left));
    }
    ExprPtr unary() {
        NestingGuard guard(nesting, peek().location);
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
            return checkedExpression(std::move(result));
        }
        return postfix(primary());
    }
    ExprPtr postfix(ExprPtr left) {
        left = checkedExpression(std::move(left));
        for (;;) {
            if (accept(".")) {
                if (left->kind != Expr::Kind::Name) throw Error(left->location, "qualified access requires a module alias");
                auto member = expect(TokenKind::Name, "a module member name");
                left->text += "." + member.text; left->nameLocation = member.location;
            } else if (is("(")) {
                Token open = take();
                auto call = std::make_shared<Expr>(Expr::Kind::Call, open.location);
                call->items.push_back(std::move(left));
                if (!is(")")) { do { call->items.push_back(expression()); } while (accept(",") && !is(")")); }
                expect(")"); left = std::move(call);
            } else if (accept("::")) {
                auto application = std::make_shared<Expr>(Expr::Kind::TypeApply, left->location);
                application->items.push_back(std::move(left));
                expect("[");
                do { application->typeArguments.push_back(type()); } while (accept(","));
                expect("]"); left = std::move(application);
            } else if (accept("?")) {
                auto propagation = std::make_shared<Expr>(Expr::Kind::Propagate, left->location);
                propagation->items.push_back(std::move(left)); left = std::move(propagation);
            } else if (is("[")) {
                Token open = take();
                auto index = std::make_shared<Expr>(Expr::Kind::Index, open.location);
                index->items = {left, expression()};
                expect("]"); left = std::move(index);
            } else break;
            left = checkedExpression(std::move(left));
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
            auto result = std::make_shared<Expr>(Expr::Kind::Name, token.location); result->text = token.text; result->nameLocation = token.location; return result;
        }
        throw Error(token.location, "expected an expression, found " + description(token));
    }
    ExprPtr returnValues() {
        ExprPtr first = expression();
        if (!accept(",")) return first;
        auto result = std::make_shared<Expr>(Expr::Kind::Tuple, first->location);
        result->items.push_back(std::move(first));
        do { result->items.push_back(expression()); } while (accept(","));
        return checkedExpression(std::move(result));
    }
    StmtPtr conditional(Token start, bool stack) {
        NestingGuard guard(nesting, start.location);
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
            if (is("_") && peek(1).text != "." && peek(1).text != "(") { take(); arm->name = "_"; }
            else arm->expression = expression();
            if (accept("if")) arm->other = expression();
            if (arm->name == "_" && !arm->other) wildcardSeen = true;
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
            if (is("export")) throw Error(peek().location, "duplicate export modifier");
            auto result = statement(stack);
            if (result->kind != Stmt::Kind::Function && result->kind != Stmt::Kind::Word
                && result->kind != Stmt::Kind::Extern && result->kind != Stmt::Kind::Enum
                && result->kind != Stmt::Kind::Let && result->kind != Stmt::Kind::Var)
                throw Error(start.location, "only functions, words, extern declarations and variables may be exported");
            result->exported = true;
            return result;
        }
        if (accept("enum")) {
            auto result = std::make_shared<Stmt>(Stmt::Kind::Enum, start.location);
            auto name = expect(TokenKind::Name, "an enum name");
            result->name = name.text; result->nameLocation = name.location;
            result->typeParameters = typeParameters(&result->typeParameterLocations);
            expect(":"); expect(TokenKind::Newline, "a newline after ':'");
            expect(TokenKind::Indent, "enum variants");
            separators();
            while (!is(TokenKind::Dedent) && !is(TokenKind::End)) {
                auto variant = std::make_shared<Stmt>(Stmt::Kind::Function, peek().location);
                auto variantName = expect(TokenKind::Name, "a variant name");
                variant->name = variantName.text; variant->nameLocation = variantName.location;
                if (is("(")) variant->parameters = parameters(false, false, variant->variadic);
                finishSimple(); result->body.push_back(std::move(variant)); separators();
            }
            expect(TokenKind::Dedent, "end of enum variants");
            if (result->body.empty()) throw Error(start.location, "enum requires at least one variant");
            return result;
        }
        if (is("fun") || is("fn") || is("word") || is("extern")) {
            bool external = accept("extern");
            bool word = false;
            if (external) {
                if (!accept("fun") && !accept("fn")) throw Error(peek().location, "expected 'fun' after 'extern'");
            } else word = take().text == "word";
            auto result = std::make_shared<Stmt>(external ? Stmt::Kind::Extern : word ? Stmt::Kind::Word : Stmt::Kind::Function, start.location);
            auto name = expect(TokenKind::Name, "a function name");
            result->name = name.text; result->nameLocation = name.location;
            result->typeParameters = typeParameters(&result->typeParameterLocations);
            if (external && !result->typeParameters.empty()) throw Error(start.location, "extern functions cannot be generic");
            result->parameters = parameters(word, external, result->variadic);
            if (accept("->")) result->returnType = type();
            if (external) {
                if (accept("=")) {
                    Token symbol = expect(TokenKind::String, "a foreign symbol string");
                    if (symbol.text.empty() || symbol.text.find('\0') != std::string::npos)
                        throw Error(symbol.location, "foreign symbol must be nonempty and contain no NUL bytes");
                    result->expression = std::make_shared<Expr>(Expr::Kind::String, symbol.location);
                    result->expression->text = std::move(symbol.text);
                }
                finishSimple();
            }
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
            } else {
                auto name = expect(TokenKind::Name, "a variable name");
                result->name = name.text; result->nameLocation = name.location;
            }
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
            auto name = expect(TokenKind::Name, "a loop variable");
            result->name = name.text; result->nameLocation = name.location;
            expect("in"); result->expression = expression(); result->body = suite(stack);
            if (accept("else")) result->elseBody = suite(stack);
            return result;
        }
        if (accept("match")) return matchStatement(start, stack);
        if (accept("unsafe")) {
            auto result = std::make_shared<Stmt>(Stmt::Kind::Unsafe, start.location);
            result->body = suite(stack);
            return result;
        }
        if (accept("break") || accept("continue")) {
            auto result = std::make_shared<Stmt>(start.text == "break" ? Stmt::Kind::Break : Stmt::Kind::Continue, start.location);
            finishSimple(); return result;
        }
        if (accept("import")) {
            auto result = std::make_shared<Stmt>(Stmt::Kind::Import, start.location);
            result->name = expect(TokenKind::String, "an import path string").text;
            if (accept("as")) {
                auto alias = expect(TokenKind::Name, "an import alias");
                result->other = nameExpression(alias); result->nameLocation = alias.location;
            }
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
                result->operation = assignment.text.substr(0, 1);
            }
            finishSimple(); return result;
        }
        auto result = std::make_shared<Stmt>(Stmt::Kind::Expression, start.location);
        result->expression = std::move(left); finishSimple(); return result;
    }
    static ExprPtr nameExpression(const Token& token) {
        auto result = std::make_shared<Expr>(Expr::Kind::Name, token.location); result->text = token.text; result->nameLocation = token.location; return result;
    }
    void recover(size_t before) {
        if (position <= before && position + 1 < tokens.size()) ++position;
        while (position + 1 < tokens.size()) {
            const auto& token = peek();
            if (indentation[position] == 0 && token.location.column == 1 && token.kind == TokenKind::Name
                && (token.text == "fun" || token.text == "fn" || token.text == "word"
                    || token.text == "extern" || token.text == "import" || token.text == "export"
                    || token.text == "let" || token.text == "var")) return;
            ++position;
        }
    }
    void stackStatement(std::vector<StmtPtr>& body) {
        if (is("if") || is("while") || is("for") || is("match") || is("let") || is("var")
            || is("return") || is("break") || is("continue") || is("fun") || is("word") || is("unsafe")) {
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
    Parser(const std::vector<Token>& input, std::string file) : tokens(input), filename(std::move(file)) {
        unsigned level = 0;
        for (const auto& token : input) {
            indentation.push_back(level);
            if (token.kind == TokenKind::Indent) ++level;
            else if (token.kind == TokenKind::Dedent && level) --level;
        }
    }
    SyntaxTree run() {
        if (tokens.empty()) throw Error({filename, 1, 1}, "empty token stream");
        SyntaxTree result; result.filename = filename;
        std::vector<std::pair<Location, std::string>> errors;
        separators();
        while (!is(TokenKind::End)) {
            const size_t before = position;
            try {
                if (is(TokenKind::Indent)) throw Error(peek().location, "unexpected indentation at top level");
                if (is(TokenKind::Dedent)) throw Error(peek().location, "unexpected end of block");
                result.statements.push_back(statement());
            } catch (const Error& error) {
                errors.emplace_back(error.location, error.what());
                nesting = 0;
                if (errors.size() == 20) break;
                recover(before);
            }
            separators();
        }
        if (!errors.empty()) {
            std::string message = errors.front().second;
            for (size_t index = 1; index < errors.size(); ++index) {
                const auto& entry = errors[index];
                message += "\n" + entry.first.file + ":" + std::to_string(entry.first.line) + ":"
                    + std::to_string(entry.first.column) + ": error: " + entry.second;
            }
            if (errors.size() == 20) message += "\nstopped after 20 parser errors";
            throw Error(errors.front().first, message);
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
    case Expr::Kind::TypeApply: return "TypeApply";
    case Expr::Kind::Propagate: return "Propagate";
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
    case Stmt::Kind::Unsafe: return "Unsafe";
    case Stmt::Kind::Enum: return "Enum";
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
    for (const auto& argument : expr->typeArguments) out << " [" << argument.str() << "]";
    out << " @" << expr->location.line << ':' << expr->location.column << '\n';
    for (const auto& item : expr->items) dumpExpression(out, item, depth + 1);
    for (const auto& parameter : expr->parameters) out << std::string((depth + 1) * 2, ' ') << "Parameter " << parameter.name << ": " << parameter.type.str() << '\n';
    dumpStatements(out, expr->body, depth + 1);
}
void dumpStatements(std::ostream& out, const std::vector<StmtPtr>& stmts, unsigned depth) {
    for (const auto& stmt : stmts) {
        out << std::string(depth * 2, ' ') << statementName(stmt->kind);
        if (!stmt->name.empty()) out << ' ' << stmt->name;
        if (!stmt->operation.empty()) out << " " << stmt->operation << "=";
        for (const auto& parameter : stmt->typeParameters) out << " [" << parameter << "]";
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
