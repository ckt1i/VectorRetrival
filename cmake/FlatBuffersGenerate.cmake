# ============================================================================
# FlatBuffers Schema Generation CMake Module
# ============================================================================
# Usage:
#   flatbuffers_generate_headers(
#     TARGET <target_name>
#     SCHEMAS <schema1.fbs> <schema2.fbs> ...
#     OUTPUT_DIR <output_directory>
#     [INCLUDE_PREFIX <prefix>]
#   )
# ============================================================================

function(flatbuffers_generate_headers)
  cmake_parse_arguments(
    FBS
    ""
    "TARGET;OUTPUT_DIR;INCLUDE_PREFIX"
    "SCHEMAS"
    ${ARGN}
  )

  if(NOT FBS_TARGET)
    message(FATAL_ERROR "flatbuffers_generate_headers: TARGET is required")
  endif()

  if(NOT FBS_SCHEMAS)
    message(FATAL_ERROR "flatbuffers_generate_headers: SCHEMAS is required")
  endif()

  if(NOT FBS_OUTPUT_DIR)
    set(FBS_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
  endif()

  # Create output directory
  file(MAKE_DIRECTORY ${FBS_OUTPUT_DIR})

  # Collect generated headers
  set(GENERATED_HEADERS)

  foreach(SCHEMA ${FBS_SCHEMAS})
    get_filename_component(SCHEMA_NAME ${SCHEMA} NAME_WE)
    get_filename_component(SCHEMA_ABS ${SCHEMA} ABSOLUTE)
    
    set(GENERATED_HEADER ${FBS_OUTPUT_DIR}/${SCHEMA_NAME}_generated.h)
    list(APPEND GENERATED_HEADERS ${GENERATED_HEADER})

    add_custom_command(
      OUTPUT ${GENERATED_HEADER}
      COMMAND flatc
        --cpp
        --cpp-std c++17
        --gen-mutable
        --gen-object-api
        --gen-compare
        --scoped-enums
        -o ${FBS_OUTPUT_DIR}
        ${SCHEMA_ABS}
      DEPENDS ${SCHEMA_ABS} flatc
      COMMENT "Generating FlatBuffers header for ${SCHEMA_NAME}.fbs"
      VERBATIM
    )
  endforeach()

  # Create a custom target that depends on all generated headers
  add_custom_target(${FBS_TARGET}_generate
    DEPENDS ${GENERATED_HEADERS}
  )

  # Create an interface library for the generated headers
  add_library(${FBS_TARGET} INTERFACE)
  add_dependencies(${FBS_TARGET} ${FBS_TARGET}_generate)
  target_include_directories(${FBS_TARGET} INTERFACE ${FBS_OUTPUT_DIR})
  target_link_libraries(${FBS_TARGET} INTERFACE flatbuffers)

  # Export generated headers list
  set(${FBS_TARGET}_HEADERS ${GENERATED_HEADERS} PARENT_SCOPE)
endfunction()

# ============================================================================
# Convenience macro for schema directory
# ============================================================================
macro(vdb_add_flatbuffers_schemas)
  set(SCHEMA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/schema)
  set(SCHEMA_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/schema/generated)

  # Find all .fbs files in schema directory
  file(GLOB FBS_SCHEMAS ${SCHEMA_DIR}/*.fbs)

  if(FBS_SCHEMAS)
    flatbuffers_generate_headers(
      TARGET vdb_schemas
      SCHEMAS ${FBS_SCHEMAS}
      OUTPUT_DIR ${SCHEMA_OUTPUT_DIR}
    )
    message(STATUS "FlatBuffers schemas: ${FBS_SCHEMAS}")
  else()
    # Create empty interface target if no schemas yet
    add_library(vdb_schemas INTERFACE)
    target_link_libraries(vdb_schemas INTERFACE flatbuffers)
    message(STATUS "No FlatBuffers schemas found in ${SCHEMA_DIR}")
  endif()
endmacro()

# Auto-invoke schema generation
vdb_add_flatbuffers_schemas()
