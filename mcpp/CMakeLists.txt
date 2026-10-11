cmake_minimum_required(VERSION 3.15)
project(mcpp VERSION 0.1.0 LANGUAGES CXX)

add_library(mcpp INTERFACE)
add_library(mcpp::mcpp ALIAS mcpp)
target_include_directories(mcpp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(mcpp INTERFACE cxx_std_17)

option(MCPP_BUILD_TESTS "Build mcpp tests" ON)
option(MCPP_BUILD_EXAMPLES "Build mcpp examples" ON)

if(MCPP_BUILD_TESTS)
    enable_testing()
    add_executable(test_json tests/test_json.cpp)
    target_link_libraries(test_json PRIVATE mcpp)
    add_test(NAME json COMMAND test_json)

    add_executable(test_protocol tests/test_protocol.cpp)
    target_link_libraries(test_protocol PRIVATE mcpp)
    add_test(NAME protocol COMMAND test_protocol)

    if(MSVC)
        target_compile_options(test_json test_protocol PRIVATE /W4)
    else()
        target_compile_options(test_json test_protocol PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(MCPP_BUILD_EXAMPLES)
    foreach(example IN ITEMS echo_server filesystem_server sysinfo_server mini_client)
        add_executable(${example} examples/${example}.cpp)
        target_link_libraries(${example} PRIVATE mcpp)
        if(WIN32 AND NOT MSVC)
            # standalone exes: no MinGW runtime DLL hunting on user machines
            target_link_options(${example} PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
    endforeach()
endif()

# Integration: the client talks to the bundled echo server over real pipes.
if(MCPP_BUILD_TESTS AND MCPP_BUILD_EXAMPLES)
    add_test(
        NAME mini_client_against_echo
        COMMAND mini_client $<TARGET_FILE:echo_server>
                --call echo --args "{\"message\":\"ping\",\"upper\":true}")
    set_tests_properties(mini_client_against_echo PROPERTIES PASS_REGULAR_EXPRESSION
        "PING")
endif()

install(DIRECTORY include/ DESTINATION include)
