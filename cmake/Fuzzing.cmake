option(PIKAFLOW_BUILD_FUZZER "Build the Clang libFuzzer lexer/parser target" OFF)
if(PIKAFLOW_BUILD_FUZZER)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR MSVC)
        message(FATAL_ERROR "PIKAFLOW_BUILD_FUZZER requires Clang with libFuzzer, ASan and UBSan runtimes")
    endif()
    add_executable(pikaflow_parser_fuzzer
        tests/fuzz_parser.cpp src/common.cpp src/lexer.cpp src/parser.cpp)
    target_compile_features(pikaflow_parser_fuzzer PRIVATE cxx_std_17)
    target_include_directories(pikaflow_parser_fuzzer PRIVATE include)
    target_compile_options(pikaflow_parser_fuzzer PRIVATE
        -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined
        -fno-sanitize-recover=all)
    target_link_options(pikaflow_parser_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
