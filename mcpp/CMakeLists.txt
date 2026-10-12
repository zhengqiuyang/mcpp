cmake_minimum_required(VERSION 3.15)
project(mcpp VERSION 0.2.0 LANGUAGES CXX)

include(GNUInstallDirs)

add_library(mcpp INTERFACE)
add_library(mcpp::mcpp ALIAS mcpp)
target_include_directories(mcpp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
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
    foreach(example IN ITEMS echo_server filesystem_server sysinfo_server mini_client http_echo_server llama_agent)
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

    set(MCPP_TEST_TARGETS test_json test_protocol test_process test_http
                          test_features test_http_hardening test_http_threaded
                          test_v04 test_agent)
    foreach(t IN ITEMS ${MCPP_TEST_TARGETS})
        add_executable(${t} tests/${t}.cpp)
        target_link_libraries(${t} PRIVATE mcpp Threads::Threads)
        target_compile_options(${t} PRIVATE ${MCPP_WARN_OPTS})
        if(MCPP_STATIC_LINK)
            target_link_options(${t} PRIVATE ${MCPP_STATIC_LINK})
        endif()
        add_test(NAME ${t} COMMAND ${t})
    endforeach()
    # socket/thread tests: never let a platform quirk hang CI forever
    set_tests_properties(test_http test_features test_http_hardening
                         test_http_threaded test_agent PROPERTIES TIMEOUT 120)

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

    # randomized robustness (deterministic PRNGs, bounded iterations)
    foreach(f IN ITEMS fuzz_json fuzz_rpc)
        add_executable(${f} tests/${f}.cpp)
        target_link_libraries(${f} PRIVATE mcpp)
        target_compile_options(${f} PRIVATE ${MCPP_WARN_OPTS})
        if(MCPP_STATIC_LINK)
            target_link_options(${f} PRIVATE ${MCPP_STATIC_LINK})
        endif()
        add_test(NAME ${f} COMMAND ${f} 50000)
    endforeach()

    add_executable(bench tests/bench.cpp)
    target_link_libraries(bench PRIVATE mcpp)
    target_compile_options(bench PRIVATE ${MCPP_WARN_OPTS})
    if(MCPP_STATIC_LINK)
        target_link_options(bench PRIVATE ${MCPP_STATIC_LINK})
    endif()
endif()

# ---- install / find_package(mcpp) support ---------------------------------
include(CMakePackageConfigHelpers)

install(TARGETS mcpp EXPORT mcppTargets)
install(EXPORT mcppTargets
        FILE mcppTargets.cmake
        NAMESPACE mcpp::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcpp)
configure_package_config_file(${CMAKE_CURRENT_SOURCE_DIR}/mcppConfig.cmake.in
                              ${CMAKE_CURRENT_BINARY_DIR}/mcppConfig.cmake
                              INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcpp)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/mcppConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/mcppConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/mcppConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcpp)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
