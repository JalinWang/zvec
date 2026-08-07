set(SNOWBALL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/snowball-3.1.1")
set(SNOWBALL_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/snowball-codegen")

include("${CMAKE_CURRENT_SOURCE_DIR}/snowball_codegen.cmake")
snowball_generate_utf8(
  SOURCE_DIR "${SNOWBALL_SOURCE_DIR}"
  BUILD_DIR "${SNOWBALL_BUILD_DIR}"
)

set_source_files_properties(${SNOWBALL_GENERATED_SOURCES}
  PROPERTIES GENERATED TRUE)

if(NOT TARGET snowball)
  add_library(snowball STATIC ${SNOWBALL_GENERATED_SOURCES})
  add_dependencies(snowball ${SNOWBALL_CODEGEN_TARGET})
  # Public include points to the SOURCE directory — libstemmer.h exists at
  # configure time and does not depend on the codegen step.
  target_include_directories(snowball SYSTEM PUBLIC
    "${SNOWBALL_SOURCE_DIR}/include"
  )
  # Private includes for generated headers (modules_utf8.h, stem_*.h).
  target_include_directories(snowball PRIVATE
    "${SNOWBALL_BUILD_DIR}"
    "${SNOWBALL_BUILD_DIR}/libstemmer"
    "${SNOWBALL_BUILD_DIR}/src_c"
  )
  set_target_properties(snowball PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 99
  )
endif()

set(snowball_FOUND TRUE PARENT_SCOPE)
