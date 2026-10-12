# mcpp is header-only: configure the mcpp/ subdirectory, install headers +
# CMake config, and drop whatever landed in debug/.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO zhengqiuyang/mcpp
    REF v0.3.0
    SHA512 6d91933a74d13ca52a84604517a91bef4ce43e90fb6c22c7e1329d88e78ca8ffe528109fa57c88a5563bf19ea42214f11635421f19b6907af53d85b0c80cd44e
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/mcpp"
    OPTIONS
        -DMCPP_BUILD_TESTS=OFF
        -DMCPP_BUILD_EXAMPLES=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME mcpp CONFIG_PATH lib/cmake/mcpp)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug")
file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
