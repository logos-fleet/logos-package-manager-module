cmake_minimum_required(VERSION 3.14)
project(LogosPackageManagerModulePlugin LANGUAGES CXX)

# Include the Logos Module CMake helper
if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

# WHAT AN ARCHIVE CANNOT CARRY.
#
# On a desktop `package_manager_lib` and `lgx` arrive as shared libraries and
# the dynamic loader resolves what they need. On iOS they arrive as archives
# (logos-package-manager's nix/mobile-ios.nix), and an archive resolves nothing:
# lgx's Ed25519 travels inside liblgx.a, but zlib and the CoreFoundation calls
# its Unicode backend makes are the PLATFORM's and have to be named here. Both
# ship in the iOS SDK; there is nothing to bundle, only to name.
set(PACKAGE_MANAGER_PLATFORM_LIBS "")
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(APPEND PACKAGE_MANAGER_PLATFORM_LIBS z "-framework CoreFoundation")
endif()

# Universal module — generated_code/ is picked up automatically by LogosModule.cmake
logos_module(
    NAME package_manager
    SOURCES
        src/package_manager_impl.h
        src/package_manager_impl.cpp
    EXTERNAL_LIBS
        package_manager_lib
        lgx
    LINK_LIBRARIES
        ${PACKAGE_MANAGER_PLATFORM_LIBS}
)
