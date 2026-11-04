#include "pikaflow/frontend.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
pika::SyntaxTree tree(const std::string& source) { return pika::parse(pika::lex(source, "test.pf"), "test.pf"); }
void reject(const std::string& source, const std::string& fragment) {
    try { tree(source); }
    catch (const pika::Error& error) {
        require(error.location.file == "test.pf", "diagnostic lost filename");
        require(error.location.line > 0 && error.location.column > 0, "diagnostic lost position");
        require(std::string(error.what()).find(fragment) != std::string::npos, "unexpected diagnostic");
        return;
    }
    throw std::runtime_error("invalid program was accepted: " + source);
}
}

int main() {
    try {
        using namespace pika;
        const auto tokens = lex("# header\r\nfun main() -> unit:\r\n    # blank\r\n    print(\"hello\\nworld\")\r\n    ", "test.pf");
        require(std::count_if(tokens.begin(), tokens.end(), [](const Token& t) { return t.kind == TokenKind::Indent; }) == 1, "wrong indentation tokens");
        require(std::count_if(tokens.begin(), tokens.end(), [](const Token& t) { return t.kind == TokenKind::Dedent; }) == 1, "wrong dedentation tokens");
        auto stringToken = std::find_if(tokens.begin(), tokens.end(), [](const Token& t) { return t.kind == TokenKind::String; });
        require(stringToken != tokens.end() && stringToken->text == "hello\nworld" && stringToken->location.line == 4, "string decoding/location failed");
        require(lex("    ", "test.pf").size() == 1, "trailing whitespace should produce only END");
        require(tree("").statements.empty(), "empty module should parse");

        const auto basic = tree(
            "fun add(a: i64, b: i64) -> i64:\n"
            "    return a + b * 2\n"
            "fun main() -> unit:\n"
            "    let answer = 20 |> add(22)\n"
            "    answer |> print\n");
        require(basic.statements.size() == 2, "function declarations lost");
        const auto sum = basic.statements[0]->body[0]->expression;
        require(sum->kind == Expr::Kind::Binary && sum->text == "+" && sum->items[1]->text == "*", "operator precedence failed");
        const auto pipe = basic.statements[1]->body[0]->expression;
        require(pipe->text == "|>" && pipe->items[1]->kind == Expr::Kind::Call, "pipeline AST failed");

        const auto hybrid = tree(
            "word exchange(i64, i64) -> (i64, i64):\n"
            "    swap\n"
            "fun main() -> unit:\n"
            "    let (a, b) = stack(20, 22) -> (i64, i64):\n"
            "        exchange\n"
            "    let answer = stack(a, b) -> i64:\n"
            "        + dup *\n"
            "    stack:\n"
            "        -12 3 + drop\n");
        require(hybrid.statements[0]->kind == Stmt::Kind::Word, "word declaration failed");
        require(hybrid.statements[0]->parameters.size() == 2 && hybrid.statements[0]->parameters[0].name == "arg0", "word input types failed");
        const auto destructure = hybrid.statements[1]->body[0];
        require(destructure->target && destructure->target->items.size() == 2, "tuple destructuring failed");
        require(destructure->expression->kind == Expr::Kind::Stack && destructure->expression->body[0]->expression->text == "exchange", "stack word expression failed");
        require(hybrid.statements[1]->body[1]->expression->body.size() == 3, "postfix operations lost");
        require(hybrid.statements[1]->body[2]->expression->body[0]->expression->integer == -12, "negative stack literal failed");

        const auto closures = tree(
            "fun maker(offset: i64) -> fn(i64) -> i64:\n"
            "    let f = fun(x: i64) -> i64:\n"
            "        return x + offset\n"
            "    return f\n"
            "fun main() -> unit:\n"
            "    let xs: [i64] = [1,\n"
            "        2, 3]\n"
            "    xs[0] = maker(2)(3)\n");
        require(closures.statements[0]->returnType.kind == TypeKind::Function, "function type failed");
        require(closures.statements[0]->body.size() == 2 && closures.statements[0]->body[0]->expression->kind == Expr::Kind::Lambda, "multiline lambda swallowed following statement");
        require(closures.statements[1]->body[1]->target->kind == Expr::Kind::Index, "index assignment failed");

        const auto flow = tree(
            "fun main() -> unit:\n"
            "    var i = 0\n"
            "    while i < 3:\n"
            "        if i == 0:\n"
            "            i += 1\n"
            "            continue\n"
            "        elif i == 1:\n"
            "            break\n"
            "        else:\n"
            "            print(i)\n"
            "    for x in [1, 2]:\n"
            "        match x:\n"
            "            case 1:\n"
            "                print(true)\n"
            "            case _:\n"
            "                print(false)\n");
        auto conditional = flow.statements[0]->body[1]->body[0];
        require(conditional->elseBody[0]->kind == Stmt::Kind::If, "elif lowering failed");
        auto match = flow.statements[0]->body[2]->body[0];
        require(match->kind == Stmt::Kind::Match && match->body.size() == 2 && match->body[1]->name == "_", "match AST failed");

        const auto imports = tree(
            "import \"math.pf\" as math\n"
            "extern fun printf(format: ptr[i64], ...) -> i64\n"
            "export fun parse(x: str) -> Result[i64, str]:\n"
            "    return ok(1)\n");
        require(imports.statements[0]->other->text == "math", "import alias failed");
        require(imports.statements[1]->variadic, "extern varargs failed");
        require(imports.statements[2]->exported && imports.statements[2]->returnType.kind == TypeKind::Result, "Result/export types failed");
        require(tree("let n = -9223372036854775808\n").statements[0]->expression->integer == std::numeric_limits<int64_t>::min(), "minimum i64 literal failed");

        reject("fun main():\n\tprint(1)\n", "tabs");
        reject("fun main():\n    print(1)\n  print(2)\n", "inconsistent indentation");
        reject("fun main()\n    return\n", "expected ':'");
        reject("fun main():\nprint(1)\n", "indented block");
        reject("let x = \"unterminated\n", "unterminated string");
        reject("let x = \"\\q\"\n", "unknown string escape");
        reject("let x = (1 + 2]\n", "unmatched closing");
        reject("let x = (1 + 2\n", "unclosed delimiter");
        reject("let x = 2e+\n", "expected digits");
        reject("let x = 12abc\n", "numeric literal");
        reject("let x = 9223372036854775808\n", "i64 range");
        reject("let x = -9223372036854775809\n", "i64 range");
        reject("fun f(a: i64, a: bool):\n    return\n", "duplicate parameter");
        reject("let x: imaginary = 1\n", "unknown type");
        reject("fun main():\n    match 1:\n        case _:\n            return\n        case 2:\n            return\n", "wildcard case");

        // Deterministic malformed-input smoke test: every input must terminate or diagnose.
        std::mt19937 random(20261104);
        const std::string alphabet = "abc012 +-*/()[]:\n\"#\t";
        for (unsigned trial = 0; trial < 1000; ++trial) {
            std::string input;
            for (unsigned i = random() % 80; i > 0; --i) input += alphabet[random() % alphabet.size()];
            try { tree(input); } catch (const Error&) {}
        }
        require(dumpTokens(tokens).find("STRING \"hello\\nworld\"") != std::string::npos, "token dump lost string escaping");
        require(dumpAST(hybrid).find("Stack -> (i64, i64)") != std::string::npos, "AST dump failed");
        std::cout << "frontend tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "frontend test failure: " << error.what() << '\n';
        return 1;
    }
}
