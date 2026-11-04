#pragma once
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pika {
struct Location {
    std::string file;
    unsigned line = 1, column = 1;
};
class Error : public std::runtime_error {
public:
    Location location;
    Error(Location where, const std::string& message)
        : std::runtime_error(message), location(std::move(where)) {}
};
enum class TypeKind : uint32_t {
    Unit = 0, I64 = 1, F64 = 2, Bool = 3, String = 4,
    Array = 5, Tuple = 6, Function = 7, Cell = 8, Pointer = 9, Result = 10
};
struct Type {
    TypeKind kind = TypeKind::Unit;
    std::vector<Type> args;
    Type() = default;
    Type(TypeKind k) : kind(k) {}
    Type(TypeKind k, std::vector<Type> a) : kind(k), args(std::move(a)) {}
    bool operator==(const Type& other) const { return kind == other.kind && args == other.args; }
    bool operator!=(const Type& other) const { return !(*this == other); }
    bool managed() const { return (kind >= TypeKind::String && kind <= TypeKind::Cell) || kind == TypeKind::Result; }
    bool numeric() const { return kind == TypeKind::I64 || kind == TypeKind::F64; }
    std::string str() const;
    static Type array(Type element) { return {TypeKind::Array, {std::move(element)}}; }
    static Type cell(Type element) { return {TypeKind::Cell, {std::move(element)}}; }
    static Type function(std::vector<Type> parameters, Type result) {
        parameters.push_back(std::move(result));
        return {TypeKind::Function, std::move(parameters)};
    }
};
inline const Type Unit{TypeKind::Unit}, I64{TypeKind::I64}, F64{TypeKind::F64},
    Bool{TypeKind::Bool}, String{TypeKind::String};
}
