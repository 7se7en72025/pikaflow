#include "pikaflow/frontend.hpp"
#include <cctype>
#include <iomanip>
#include <sstream>

namespace pika {
namespace {
bool nameStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool namePart(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
std::string quoted(const std::string& text) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : text) {
        switch (c) {
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        case '\0': out << "\\0"; break;
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        default:
            if (c < 32) out << "\\x" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c) << std::dec;
            else out << char(c);
        }
    }
    return out.str() + '"';
}
}

std::vector<Token> lex(const std::string& source, const std::string& filename) {
    std::vector<Token> result;
    std::vector<unsigned> indents{0};
    std::vector<std::pair<char, Location>> brackets;
    size_t offset = source.size() >= 3 && source.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    unsigned line = 1, column = 1;
    bool lineStart = true, logicalLine = false;
    auto location = [&] { return Location{filename, line, column}; };
    auto peek = [&](size_t ahead = 0) { return offset + ahead < source.size() ? source[offset + ahead] : '\0'; };
    auto take = [&]() {
        char c = source[offset++];
        if (c == '\n') { ++line; column = 1; }
        else ++column;
        return c;
    };
    auto emit = [&](TokenKind kind, std::string text, Location at) {
        result.push_back({kind, std::move(text), std::move(at)});
    };
    while (offset < source.size()) {
        if (lineStart) {
            unsigned spaces = 0;
            while (peek() == ' ') { take(); ++spaces; }
            if (peek() == '\t') throw Error(location(), "tabs are not allowed for indentation; use spaces");
            const bool blank = peek() == '\n' || peek() == '\r' || peek() == '\0'
                || peek() == '#' || (peek() == '/' && peek(1) == '/');
            if (brackets.empty() && !blank) {
                if (spaces > indents.back()) {
                    indents.push_back(spaces);
                    emit(TokenKind::Indent, "", {filename, line, 1});
                } else {
                    while (spaces < indents.back()) {
                        indents.pop_back();
                        emit(TokenKind::Dedent, "", {filename, line, 1});
                    }
                    if (spaces != indents.back()) throw Error(location(), "inconsistent indentation: dedent does not match an enclosing block");
                }
            }
            lineStart = false;
        }
        if (offset == source.size()) break;
        const char c = peek();
        if (c == ' ' || c == '\t') { take(); continue; }
        if (c == '\r' || c == '\n') {
            Location at = location();
            if (c == '\r') { take(); if (peek() != '\n') { ++line; column = 1; } }
            if (peek() == '\n') take();
            if (brackets.empty() && logicalLine) {
                emit(TokenKind::Newline, "", at);
                logicalLine = false;
            }
            lineStart = true;
            continue;
        }
        if (c == '#' || (c == '/' && peek(1) == '/')) {
            while (offset < source.size() && peek() != '\r' && peek() != '\n') take();
            continue;
        }
        Location at = location();
        logicalLine = true;
        if (nameStart(c)) {
            std::string text;
            while (namePart(peek())) text += take();
            emit(TokenKind::Name, std::move(text), at);
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::string text;
            bool floating = false;
            while (std::isdigit(static_cast<unsigned char>(peek()))) text += take();
            if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
                floating = true;
                text += take();
                while (std::isdigit(static_cast<unsigned char>(peek()))) text += take();
            }
            if (peek() == 'e' || peek() == 'E') {
                floating = true;
                text += take();
                if (peek() == '+' || peek() == '-') text += take();
                if (!std::isdigit(static_cast<unsigned char>(peek()))) throw Error(location(), "expected digits after floating-point exponent");
                while (std::isdigit(static_cast<unsigned char>(peek()))) text += take();
            }
            if (nameStart(peek())) throw Error(location(), "numeric literal must be separated from an identifier");
            emit(floating ? TokenKind::Float : TokenKind::Integer, std::move(text), at);
            continue;
        }
        if (c == '"' || c == '\'') {
            const char delimiter = take();
            std::string text;
            while (offset < source.size() && peek() != delimiter) {
                if (peek() == '\n' || peek() == '\r') throw Error(at, "unterminated string literal");
                char value = take();
                if (value == '\\') {
                    if (offset == source.size()) throw Error(at, "unterminated string escape");
                    Location escaped = location();
                    switch (take()) {
                    case 'n': value = '\n'; break;
                    case 'r': value = '\r'; break;
                    case 't': value = '\t'; break;
                    case '0': value = '\0'; break;
                    case '\\': value = '\\'; break;
                    case '"': value = '"'; break;
                    case '\'': value = '\''; break;
                    default: throw Error(escaped, "unknown string escape");
                    }
                }
                text += value;
            }
            if (offset == source.size()) throw Error(at, "unterminated string literal");
            take();
            emit(TokenKind::String, std::move(text), at);
            continue;
        }
        if (c == '(' || c == '[' || c == '{') brackets.push_back({c, at});
        if (c == ')' || c == ']' || c == '}') {
            const char expected = c == ')' ? '(' : c == ']' ? '[' : '{';
            if (brackets.empty() || brackets.back().first != expected) throw Error(at, "unmatched closing delimiter");
            brackets.pop_back();
        }
        std::string symbol;
        if (c == '.' && peek(1) == '.' && peek(2) == '.') symbol = "...";
        else {
            const std::string pair = std::string(1, c) + peek(1);
            if (pair == "->" || pair == "|>" || pair == "==" || pair == "!="
                || pair == "<=" || pair == ">=" || pair == "&&" || pair == "||"
                || pair == "+=" || pair == "-=" || pair == "*=" || pair == "/=" || pair == "%=") symbol = pair;
            else if (std::string("()[]{}:;,+-*/%=<>!.").find(c) != std::string::npos) symbol.assign(1, c);
            else if (c == '\0') throw Error(at, "NUL byte is not allowed in source code");
            else throw Error(at, std::string("unexpected character '") + c + "'");
        }
        for (size_t i = 0; i < symbol.size(); ++i) take();
        emit(TokenKind::Symbol, std::move(symbol), at);
    }
    if (!brackets.empty()) throw Error(brackets.back().second, "unclosed delimiter");
    if (logicalLine) emit(TokenKind::Newline, "", location());
    while (indents.size() > 1) { indents.pop_back(); emit(TokenKind::Dedent, "", location()); }
    emit(TokenKind::End, "", location());
    return result;
}

std::string dumpTokens(const std::vector<Token>& tokens) {
    static const char* names[] = {"NAME", "INTEGER", "FLOAT", "STRING", "SYMBOL", "NEWLINE", "INDENT", "DEDENT", "END"};
    std::ostringstream out;
    for (const auto& token : tokens) {
        out << token.location.line << ':' << token.location.column << ' '
            << names[static_cast<unsigned>(token.kind)];
        if (!token.text.empty()) out << ' ' << quoted(token.text);
        out << '\n';
    }
    return out.str();
}
}
