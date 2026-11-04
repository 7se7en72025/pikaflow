#include "pikaflow/frontend.hpp"
#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 16 * 1024) return 0;
    std::string source;
    if (size) source.assign(reinterpret_cast<const char*>(data), size);
    try {
        const auto tokens = pika::lex(source, "<fuzz>");
        (void)pika::parse(tokens, "<fuzz>");
    } catch (const pika::Error&) {
        // Malformed source is expected. Other exceptions and sanitizer errors fail.
    }
    return 0;
}
