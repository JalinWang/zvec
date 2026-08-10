include(${PROJECT_ROOT_DIR}/cmake/bazel.cmake)
include(${PROJECT_ROOT_DIR}/cmake/option.cmake)

# These tests pin the ANSI code page of the test process through an embedded
# application manifest, which only exists on Windows and only MSVC's linker can
# embed one. Everywhere else the ACP is not a concept and the paths are UTF-8
# already, so there is nothing to cover.
if(NOT WIN32 OR NOT MSVC)
  return()
endif()

set(UTF8_ACP_LIBS
    zvec
    zvec_proto
    core_knn_flat
    core_knn_flat_sparse
    core_knn_hnsw
    core_knn_hnsw_rabitq
    core_knn_hnsw_sparse
    core_knn_ivf
    core_knn_diskann
    core_mix_reducer
    core_metric
    core_utility
    core_quantizer
    ${CMAKE_THREAD_LIBS_INIT}
    ${CMAKE_DL_LIBS}
)

# "<code page>:<locale name used to select it>"
# The four double-byte code pages are the ones where a UTF-8 path can fail to
# decode, which is what issue #626 was about. 1252 is a single-byte code page
# added purely for extra regression coverage: every byte value maps to some
# character in it, so it can never throw, only silently resolve a non-ASCII
# UTF-8 path to mojibake -- verifying the fix still holds against that milder
# hazard too. 65001 is the counter-example where narrow and wide conversions
# agree.
set(UTF8_ACP_VARIANTS
    "936:zh-CN"
    "932:ja-JP"
    "949:ko-KR"
    "950:zh-TW"
    "1252:en-US"
    "65001:UTF-8"
)

foreach(UTF8_ACP_VARIANT ${UTF8_ACP_VARIANTS})
  string(REPLACE ":" ";" UTF8_ACP_PARTS ${UTF8_ACP_VARIANT})
  list(GET UTF8_ACP_PARTS 0 ZVEC_ACP_CODE)
  list(GET UTF8_ACP_PARTS 1 ZVEC_ACP_LOCALE)

  set(ZVEC_ACP_TARGET utf8_acp${ZVEC_ACP_CODE}_test)
  set(ZVEC_ACP_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/${ZVEC_ACP_TARGET}.manifest")
  configure_file(acp.manifest.in "${ZVEC_ACP_MANIFEST}" @ONLY)

  cc_gmock(
    NAME ${ZVEC_ACP_TARGET} STRICT
    LIBS ${UTF8_ACP_LIBS}
    SRCS utf8_acp_test.cc ../index/utils/utils.cc
    INCS ${CMAKE_CURRENT_SOURCE_DIR}/..
    DEFS ZVEC_TEST_EXPECTED_ACP=${ZVEC_ACP_CODE}
  )

  # /INCREMENTAL (the MSVC debug default) makes CMake's Ninja generator run a
  # second mt.exe pass to merge an incremental-linking manifest fragment; with
  # our manifest already fully embedded by the linker itself there is nothing
  # left for that pass to merge and mt.exe errors out (c10100a7). Turn
  # incremental linking off for these tiny test binaries to skip that pass.
  target_link_options(${ZVEC_ACP_TARGET} PRIVATE
      "/MANIFEST:EMBED"
      "/MANIFESTINPUT:${ZVEC_ACP_MANIFEST}"
      "/INCREMENTAL:NO"
  )
  set_property(TARGET ${ZVEC_ACP_TARGET} APPEND
      PROPERTY LINK_DEPENDS "${ZVEC_ACP_MANIFEST}")

  cc_test_suite(zvec_utf8_acp ${ZVEC_ACP_TARGET})
endforeach()
