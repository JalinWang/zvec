# Snowball is consumed through pre-generated UTF-8 C sources under
# ${CMAKE_CURRENT_SOURCE_DIR}/generated. This avoids requiring GNU Make,
# nmake, Perl, sed, or the Snowball compiler during normal CMake builds.
#
# To regenerate the vendored sources after upgrading Snowball or changing
# libstemmer/modules.txt, run the upstream Snowball build once on a POSIX
# environment:
#   make -C snowball-3.1.1 libstemmer/libstemmer_utf8.c \
#       libstemmer/modules_utf8.h src_c/stem_UTF_8_*.c
# then copy the generated files into generated/.

set(SNOWBALL_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated")

file(GLOB _snowball_gen_srcs CONFIGURE_DEPENDS
  "${SNOWBALL_GENERATED_DIR}/src_c/stem_UTF_8_*.c"
)

set(_snowball_target_srcs
  "${SNOWBALL_GENERATED_DIR}/runtime/api.c"
  "${SNOWBALL_GENERATED_DIR}/runtime/utilities.c"
  "${SNOWBALL_GENERATED_DIR}/libstemmer/libstemmer_utf8.c"
  ${_snowball_gen_srcs}
)

if(NOT TARGET snowball)
  add_library(snowball STATIC ${_snowball_target_srcs})
  target_include_directories(snowball SYSTEM PUBLIC
    "${SNOWBALL_GENERATED_DIR}/include"
  )
  target_include_directories(snowball PRIVATE
    "${SNOWBALL_GENERATED_DIR}/runtime"
    "${SNOWBALL_GENERATED_DIR}/libstemmer"
    "${SNOWBALL_GENERATED_DIR}/src_c"
  )
  set_target_properties(snowball PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 99
  )
endif()

set(snowball_FOUND TRUE PARENT_SCOPE)
