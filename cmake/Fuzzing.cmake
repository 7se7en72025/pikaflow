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

option(PIKAFLOW_BUILD_SEMANTIC_FUZZER "Build the bounded Clang semantic optimizer libFuzzer target" OFF)
if(PIKAFLOW_BUILD_SEMANTIC_FUZZER)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR MSVC)
        message(FATAL_ERROR "PIKAFLOW_BUILD_SEMANTIC_FUZZER requires Clang with libFuzzer, ASan and UBSan runtimes")
    endif()
    # Compile every tested implementation unit with instrumentation, independently of pikaflow_core.
    add_executable(pikaflow_semantic_fuzzer
        tests/fuzz_semantic.cpp src/common.cpp src/lexer.cpp src/parser.cpp
        src/compiler.cpp src/ir.cpp src/optimizer.cpp src/interpreter.cpp src/runtime.cpp)
    target_compile_features(pikaflow_semantic_fuzzer PRIVATE cxx_std_17)
    target_include_directories(pikaflow_semantic_fuzzer PRIVATE include)
    target_compile_options(pikaflow_semantic_fuzzer PRIVATE
        -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined
        -fno-sanitize-recover=all)
    target_link_options(pikaflow_semantic_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(pikaflow_semantic_fuzzer PRIVATE Threads::Threads)
    if(BUILD_TESTING)
        add_test(NAME semantic_fuzzer_seeds COMMAND pikaflow_semantic_fuzzer -runs=1
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz_semantic_corpus/arithmetic.bin"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz_semantic_corpus/cfg.bin"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz_semantic_corpus/arrays.bin"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz_semantic_corpus/records.bin"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz_semantic_corpus/newclosures.bin")
    endif()
endif()
