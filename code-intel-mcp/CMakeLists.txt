cmake_minimum_required(VERSION 3.15)
project(code-intel-mcp VERSION 0.2.0 LANGUAGES C CXX)

# tree-sitter runtime + generated parsers need C11 (MSVC defaults to C89
# otherwise) and glibc's endian functions (_DEFAULT_SOURCE for le16toh).
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# mcpp is vendored from the sibling directory (add_subdirectory keeps the
# monorepo self-contained; swap for find_package(mcpp) once published).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../mcpp ${CMAKE_CURRENT_BINARY_DIR}/mcpp)

option(CODE_INTEL_TREESITTER "Use vendored tree-sitter for precise symbols (C/C++/Python/TS/TSX)" ON)

set(CI_SOURCES src/main.cpp)
if(CODE_INTEL_TREESITTER)
    set(V ${CMAKE_CURRENT_SOURCE_DIR}/vendor)

    add_library(ts_runtime STATIC ${V}/tree-sitter/src/lib.c)
    target_include_directories(ts_runtime PUBLIC ${V}/tree-sitter/include ${V}/tree-sitter/src)
    target_compile_definitions(ts_runtime PUBLIC MCPP_CODE_INTEL_TREESITTER)
    target_compile_definitions(ts_runtime PRIVATE _DEFAULT_SOURCE)

    add_library(ts_c STATIC ${V}/tree-sitter-c/parser.c)
    target_include_directories(ts_c PRIVATE ${V}/tree-sitter-c)
    add_library(ts_cpp STATIC ${V}/tree-sitter-cpp/parser.c ${V}/tree-sitter-cpp/scanner.c)
    target_include_directories(ts_cpp PRIVATE ${V}/tree-sitter-cpp)
    add_library(ts_python STATIC ${V}/tree-sitter-python/parser.c ${V}/tree-sitter-python/scanner.c)
    target_include_directories(ts_python PRIVATE ${V}/tree-sitter-python)
    add_library(ts_typescript STATIC ${V}/tree-sitter-typescript/typescript/src/parser.c
                                     ${V}/tree-sitter-typescript/typescript/src/scanner.c)
    target_include_directories(ts_typescript PRIVATE ${V}/tree-sitter-typescript/typescript/src
                                                     ${V}/tree-sitter-typescript)
    add_library(ts_tsx STATIC ${V}/tree-sitter-typescript/tsx/src/parser.c
                              ${V}/tree-sitter-typescript/tsx/src/scanner.c)
    target_include_directories(ts_tsx PRIVATE ${V}/tree-sitter-typescript/tsx/src
                                              ${V}/tree-sitter-typescript)

    list(APPEND CI_SOURCES src/ts_symbols.cpp)
endif()

add_executable(code-intel-mcp ${CI_SOURCES})
target_include_directories(code-intel-mcp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(code-intel-mcp PRIVATE mcpp::mcpp)
target_compile_features(code-intel-mcp PRIVATE cxx_std_17)
if(CODE_INTEL_TREESITTER)
    target_link_libraries(code-intel-mcp PRIVATE ts_runtime ts_c ts_cpp ts_python ts_typescript ts_tsx)
endif()
if(MSVC)
    target_compile_options(code-intel-mcp PRIVATE /W4)
else()
    target_compile_options(code-intel-mcp PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(WIN32 AND NOT MSVC)
    target_link_options(code-intel-mcp PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# ---- tests -----------------------------------------------------------------
enable_testing()
find_package(Threads REQUIRED)
add_executable(test_index tests/test_index.cpp)
target_include_directories(test_index PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_index PRIVATE Threads::Threads)
if(CODE_INTEL_TREESITTER)
    target_sources(test_index PRIVATE src/ts_symbols.cpp)
    target_link_libraries(test_index PRIVATE ts_runtime ts_c ts_cpp ts_python ts_typescript ts_tsx)
    target_compile_definitions(test_index PRIVATE MCPP_CODE_INTEL_TREESITTER)
endif()
target_compile_features(test_index PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(test_index PRIVATE /W4)
else()
    target_compile_options(test_index PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(WIN32 AND NOT MSVC)
    target_link_options(test_index PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME index COMMAND test_index)
