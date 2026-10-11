cmake_minimum_required(VERSION 3.15)
project(code-intel-mcp VERSION 0.1.0 LANGUAGES CXX)

# mcpp is vendored from the sibling directory (add_subdirectory keeps the
# monorepo self-contained; swap for find_package(mcpp) once published).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../mcpp ${CMAKE_CURRENT_BINARY_DIR}/mcpp)

add_executable(code-intel-mcp src/main.cpp)
target_link_libraries(code-intel-mcp PRIVATE mcpp::mcpp)
target_compile_features(code-intel-mcp PRIVATE cxx_std_17)
if(WIN32 AND NOT MSVC)
    target_link_options(code-intel-mcp PRIVATE -static -static-libgcc -static-libstdc++)
endif()
if(MSVC)
    target_compile_options(code-intel-mcp PRIVATE /W4)
else()
    target_compile_options(code-intel-mcp PRIVATE -Wall -Wextra -Wpedantic)
endif()
