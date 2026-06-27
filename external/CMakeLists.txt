# external/CMakeLists.txt -- Build distributable deterministic draft spec SDK
#
# Produces shared artifacts for external consumers (plugin authors, testers):
#   include/deterministic_draft_plugin.h       -- C API contract for plugin authors
#   external/include/llama_deterministic_draft.h -- Consumer header for libdeterministic_draft_spec.so
#   external/lib/libdeterministic_draft_spec.so   -- spec loader shared library
#
# Plugin authors use deterministic_draft_plugin.h to build their plugin .so.
# Consumers use llama_deterministic_draft.h to link against the .so and load plugins.

cmake_minimum_required(VERSION 3.14)

# Build the spec loader shared library (no llama dependency)
# Uses the same source as libllama (src/llama-deterministic-draft.cpp)
# so that the API is always available.
add_library(deterministic_draft_spec SHARED
    "${CMAKE_SOURCE_DIR}/src/llama-deterministic-draft.cpp"
)

set_target_properties(deterministic_draft_spec PROPERTIES EXPORT_NAME deterministic_draft_spec)

# When built as a standalone library, export the symbols via SPEC_API.
# When built inside libllama, LLAMA_API on the declarations handles export
# and SPEC_API is left empty.
if (WIN32)
    target_compile_definitions(deterministic_draft_spec PRIVATE SPEC_API=__declspec\(dllexport\))
else()
    target_compile_definitions(deterministic_draft_spec PRIVATE SPEC_API=__attribute__\(\(visibility\(\"default\"\)\)\))
endif()

# Expose both header locations via include paths:
#   - ${CMAKE_SOURCE_DIR}/include  for deterministic_draft_plugin.h (plugin contract)
#   - ${CMAKE_CURRENT_SOURCE_DIR}/include  for llama_deterministic_draft.h (consumer header)
target_include_directories(deterministic_draft_spec PUBLIC
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(deterministic_draft_spec PRIVATE cxx_std_17)

set_target_properties(deterministic_draft_spec PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/lib"
)

if (NOT WIN32)
    target_link_libraries(deterministic_draft_spec PRIVATE ${CMAKE_DL_LIBS})
endif()

# Copy headers to external/include/ for SDK distribution (with proper dependencies)
add_custom_target(deterministic_draft_spec_headers ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/include/deterministic_draft_plugin.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/deterministic_draft_plugin.h"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/include/deterministic_draft_capabilities.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/deterministic_draft_capabilities.h"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/include/deterministic_draft_plugin.h"
        "${CMAKE_SOURCE_DIR}/include/deterministic_draft_capabilities.h"
    COMMENT "Copying deterministic draft SDK headers to external/include/"
)
add_dependencies(deterministic_draft_spec deterministic_draft_spec_headers)

message(STATUS "Deterministic draft spec SDK:")
message(STATUS "  Headers: ${CMAKE_CURRENT_SOURCE_DIR}/include/")
message(STATUS "    deterministic_draft_plugin.h (for plugin authors)")
message(STATUS "    llama_deterministic_draft.h (for consumers)")
message(STATUS "  Library: ${CMAKE_CURRENT_SOURCE_DIR}/lib/libdeterministic_draft_spec.so")
