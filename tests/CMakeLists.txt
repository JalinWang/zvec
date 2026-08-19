include(${PROJECT_ROOT_DIR}/cmake/bazel.cmake)
include(${PROJECT_ROOT_DIR}/cmake/option.cmake)

add_test(
        NAME apply_patch_once_test
        COMMAND ${CMAKE_COMMAND}
        -DUTILS_FILE=${PROJECT_ROOT_DIR}/cmake/utils.cmake
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply_patch_once_test.cmake)

# TODO(windows): this is for test_util.h, should be removed
include_directories(${PROJECT_ROOT_DIR})

cc_directories(ailego)
cc_directories(db)
cc_directories(core)
cc_directories(turbo)
if(BUILD_ZVEC_SHARED AND NOT IOS AND NOT ANDROID)
    cc_directories(shared)
endif()
if(BUILD_C_BINDINGS)
    cc_directories(c)
endif()
