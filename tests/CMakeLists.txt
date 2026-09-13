cmake_minimum_required(VERSION 3.14)
project(PackageManagerModuleTests LANGUAGES CXX)

include(LogosTest)

# Unit tests (mocked PackageManagerLib)
logos_test(
    NAME package_manager_module_tests
    MODULE_SOURCES
        ../src/package_manager_impl.cpp
    TEST_SOURCES
        main.cpp
        test_package_manager.cpp
        # Per-variant availability: two lists in, one verdict out -- no disk,
        # no package load, so it belongs in the mocked suite.
        test_availability.cpp
        # The signer-trust prompt's mapping from (policy, signature state) to a
        # verdict. Real signatures are in test_signature.cpp's integration half.
        test_signer_trust.cpp
        package_manager_events_test.cpp
    MOCK_C_SOURCES
        mocks/mock_package_manager_lib.cpp
        mocks/mock_lgx.cpp
    EXTRA_INCLUDES
        stubs
)

# Integration tests (real PackageManagerLib + lgx)
find_library(LIBPM_PATH
    NAMES libpackage_manager_lib.so libpackage_manager_lib.dylib
          package_manager_lib
    PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../lib
    NO_DEFAULT_PATH)

find_library(LIBLGX_PATH
    NAMES liblgx.so liblgx.dylib lgx
    PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../lib
    NO_DEFAULT_PATH)

if(LIBPM_PATH)
    set(_pm_integration_libs "${LIBPM_PATH}")
    if(LIBLGX_PATH)
        list(APPEND _pm_integration_libs "${LIBLGX_PATH}")
    endif()

    message(STATUS "[PackageManagerTests] package manager lib found: ${LIBPM_PATH} — building integration tests")
    if(LIBLGX_PATH)
        message(STATUS "[PackageManagerTests] lgx found: ${LIBLGX_PATH}")
    else()
        message(STATUS "[PackageManagerTests] lgx not found in ../lib — linking package_manager_lib only")
    endif()

    logos_test(
        NAME package_manager_module_integration_tests
        MODULE_SOURCES
            ../src/package_manager_impl.cpp
        TEST_SOURCES
            main.cpp
            test_package_manager_integration.cpp
            test_directory_config.cpp
            test_scanning.cpp
            test_signature.cpp
            test_keyring.cpp
            package_manager_events_test.cpp
        EXTRA_INCLUDES
            ../lib
        EXTRA_LINK_LIBS
            ${_pm_integration_libs}
    )

    set(_rpath_dirs "")
    get_filename_component(_pm_dir "${LIBPM_PATH}" DIRECTORY)
    list(APPEND _rpath_dirs "${_pm_dir}")
    if(LIBLGX_PATH)
        get_filename_component(_lgx_dir "${LIBLGX_PATH}" DIRECTORY)
        list(APPEND _rpath_dirs "${_lgx_dir}")
    endif()
    list(REMOVE_DUPLICATES _rpath_dirs)
    string(REPLACE ";" ":" _rpath_joined "${_rpath_dirs}")
    set_target_properties(package_manager_module_integration_tests PROPERTIES
        BUILD_RPATH "${_rpath_joined}")
else()
    message(STATUS "[PackageManagerTests] package_manager_lib not found in ../lib — skipping integration tests")
endif()
