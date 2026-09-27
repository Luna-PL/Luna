cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_LIST_DIR}/compiled_fragment_bundle.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/compiled_fragment_pinned_protocol.cmake")

# Explicit mode entry point: no automatic protocol detection, probe execution,
# archived-script execution, writes or performance/release approval.
function(luna_validate_compiled_pinned_bundle directory)
    luna_validate_compiled_bundle_impl("${directory}" pinned)
endfunction()

if(DEFINED LUNA_COMPILED_PINNED_BUNDLE_DIR)
    luna_validate_compiled_pinned_bundle("${LUNA_COMPILED_PINNED_BUNDLE_DIR}")
elseif(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    message(FATAL_ERROR "LUNA_COMPILED_PINNED_BUNDLE_DIR is required")
endif()
