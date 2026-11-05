#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>

// Built only for sanitizer validation; both valid modes deliberately fail.
int main(int argc, char** argv) {
    if(argc == 2 && std::strcmp(argv[1], "address") == 0) {
        auto* memory = new std::uint64_t[1];
        volatile std::uint64_t* observable = memory;
        observable[1] = 42;
        delete[] memory;
        return 0;
    }
    if(argc == 2 && std::strcmp(argv[1], "undefined") == 0) {
        volatile int maximum = std::numeric_limits<int>::max();
        volatile int increment = 1;
        volatile int result = maximum + increment;
        (void)result;
        return 0;
    }
    std::cerr << "usage: sanitizer_probe address|undefined\n";
    return 2;
}
