cmake_minimum_required(VERSION 3.15)
project(mcpp VERSION 0.1.0 LANGUAGES CXX)

add_library(mcpp INTERFACE)
add_library(mcpp::mcpp ALIAS mcpp)
target_include_directories(mcpp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(mcpp INTERFACE cxx_std_17)
if(WIN32)
    # http_transport.hpp needs WinSock2 (system library, always present)
    target_link_libraries(mcpp INTERFACE ws2_32)
endif()

option(MCPP_BUILD_TESTS "Build mcpp tests" ON)
option(MCPP_BUILD_EXAMPLES "Build mcpp examples" ON)

if(MSVC)
    set(MCPP_WARN_OPTS /W4)
else()
    set(MCPP_WARN_OPTS -Wall -Wextra -Wpedantic)
endif()
if(WIN32 AND NOT MSVC)
    # standalone exes: no MinGW runtime DLL hunting on user machines
    set(MCPP_STATIC_LINK -static -static-libgcc -static-libstdc++)
endif()

if(MCPP_BUILD_EXAMPLES)
    foreach(example IN ITEMS echo_server filesystem_server sysinfo_server mini_client http_echo_server)
        add_executable(${example} examples/${example}.cpp)
        target_link_libraries(${example} PRIVATE mcpp)
        target_compile_options(${example} PRIVATE ${MCPP_WARN_OPTS})
        if(MCPP_STATIC_LINK)
            target_link_options(${example} PRIVATE ${MCPP_STATIC_LINK})
        endif()
    endforeach()
endif()

if(MCPP_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)

    set(MCPP_TEST_TARGETS test_json test_protocol test_process test_http)
    foreach(t IN ITEMS ${MCPP_TEST_TARGETS})
        add_executable(${t} tests/${t}.cpp)
        target_link_libraries(${t} PRIVATE mcpp Threads::Threads)
        target_compile_options(${t} PRIVATE ${MCPP_WARN_OPTS})
        if(MCPP_STATIC_LINK)
            target_link_options(${t} PRIVATE ${MCPP_STATIC_LINK})
        endif()
        add_test(NAME ${t} COMMAND ${t})
    endforeach()

    # full child-process session against the bundled server
    if(TARGET echo_server)
        set_tests_properties(test_process PROPERTIES ENVIRONMENT
                             "MCPP_ECHO_SERVER=$<TARGET_FILE:echo_server>")
    endif()

    # integration: the client talks to the bundled echo server over real pipes
    if(TARGET mini_client AND TARGET echo_server)
        add_test(
            NAME mini_client_against_echo
            COMMAND mini_client $<TARGET_FILE:echo_server>
                    --call echo --args "{\"message\":\"ping\",\"upper\":true}")
        set_tests_properties(mini_client_against_echo PROPERTIES PASS_REGULAR_EXPRESSION
            "PING")
    endif()

    add_executable(bench tests/bench.cpp)
    target_link_libraries(bench PRIVATE mcpp)
    target_compile_options(bench PRIVATE ${MCPP_WARN_OPTS})
    if(MCPP_STATIC_LINK)
        target_link_options(bench PRIVATE ${MCPP_STATIC_LINK})
    endif()
endif()

install(DIRECTORY include/ DESTINATION include)
