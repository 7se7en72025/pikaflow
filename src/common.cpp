#include "pikaflow/common.hpp"
#include <sstream>

namespace pika {
std::string Type::str() const {
    if (!name.empty()) {
        if (kind != TypeKind::Named || args.empty()) return name;
        std::string result = name + "[";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) result += ", ";
            result += args[i].str();
        }
        return result + "]";
    }
    switch (kind) {
    case TypeKind::Unit:
        return "unit";
    case TypeKind::I64:
        return "i64";
    case TypeKind::F64:
        return "f64";
    case TypeKind::Bool:
        return "bool";
    case TypeKind::String:
        return "str";
    default:
        break;
    }
    std::ostringstream out;
    if (kind == TypeKind::Array)
        out << "[";
    else if (kind == TypeKind::Tuple)
        out << "(";
    else if (kind == TypeKind::Cell)
        out << "cell[";
    else if (kind == TypeKind::Pointer)
        out << "ptr[";
    else if (kind == TypeKind::Result)
        out << "Result[";
    else if (kind == TypeKind::Task)
        out << "task[";
    else if (kind == TypeKind::Function) {
        out << "fn(";
        for (size_t i = 0; i + 1 < args.size(); ++i) {
            if (i)
                out << ", ";
            out << args[i].str();
        }
        return out.str() + ")->" + (args.empty() ? "unit" : args.back().str());
    }
    for (size_t i = 0; i < args.size(); ++i) {
        if (i)
            out << ", ";
        out << args[i].str();
    }
    out << (kind == TypeKind::Tuple ? ")" : "]");
    return out.str();
}
} // namespace pika
