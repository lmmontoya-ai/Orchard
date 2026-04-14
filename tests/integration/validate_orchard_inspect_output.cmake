if(NOT DEFINED EXECUTABLE_PATH)
  message(FATAL_ERROR "EXECUTABLE_PATH is required.")
endif()

if(NOT DEFINED TARGET_PATH)
  message(FATAL_ERROR "TARGET_PATH is required.")
endif()

if(NOT DEFINED EXPECTED_LAYOUT)
  message(FATAL_ERROR "EXPECTED_LAYOUT is required.")
endif()

if(NOT DEFINED EXPECTED_VOLUME_NAME)
  message(FATAL_ERROR "EXPECTED_VOLUME_NAME is required.")
endif()

if(NOT DEFINED EXPECTED_PROBE_PATH)
  set(EXPECTED_PROBE_PATH "/alpha.txt")
endif()

if(NOT DEFINED EXPECTED_COMPRESSION)
  set(EXPECTED_COMPRESSION "decmpfs_uncompressed_attribute")
endif()

execute_process(
  COMMAND "${EXECUTABLE_PATH}" --target "${TARGET_PATH}"
  RESULT_VARIABLE inspect_result
  OUTPUT_VARIABLE inspect_output
  ERROR_VARIABLE inspect_error
)

if(NOT inspect_result EQUAL 0)
  message(FATAL_ERROR "orchard-inspect failed: ${inspect_error}")
endif()

set(required_fragments
  "\"tool\": \"orchard-inspect\""
  "\"inspection_status\": \"success\""
  "\"layout\": \"${EXPECTED_LAYOUT}\""
  "\"block_size\": 4096"
  "\"selected_xid\": 42"
  "\"checkpoint_descriptor_area\""
  "\"volumes_resolved_via_omap\": true"
  "\"name\": \"${EXPECTED_VOLUME_NAME}\""
  "\"action\": \"MountReadWrite\""
  "\"name\": \"alpha.txt\""
  "\"path\": \"${EXPECTED_PROBE_PATH}\""
  "\"compression\": \"${EXPECTED_COMPRESSION}\""
)

if(DEFINED EXPECTED_PARTITION_NAME)
  list(APPEND required_fragments "\"name\": \"${EXPECTED_PARTITION_NAME}\"")
endif()

if(DEFINED EXPECTED_SYMLINK_TARGET)
  list(APPEND required_fragments "\"symlink_target\": \"${EXPECTED_SYMLINK_TARGET}\"")
endif()

if(DEFINED EXPECTED_ALIAS)
  list(APPEND required_fragments "\"${EXPECTED_ALIAS}\"")
endif()

if(DEFINED EXPECTED_LINK_COUNT)
  list(APPEND required_fragments "\"link_count\": ${EXPECTED_LINK_COUNT}")
endif()

foreach(required_fragment IN LISTS required_fragments)
  string(FIND "${inspect_output}" "${required_fragment}" match_position)
  if(match_position EQUAL -1)
    message(FATAL_ERROR "Missing expected fragment '${required_fragment}' in output:\n${inspect_output}")
  endif()
endforeach()

message(STATUS "orchard-inspect output validation passed for ${TARGET_PATH}.")
