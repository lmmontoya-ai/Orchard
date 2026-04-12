if(NOT DEFINED MANIFEST_PATH)
  message(FATAL_ERROR "MANIFEST_PATH is required.")
endif()

file(READ "${MANIFEST_PATH}" manifest_json)

string(JSON schema_version ERROR_VARIABLE schema_error GET "${manifest_json}" schema_version)
if(schema_error)
  message(FATAL_ERROR "Manifest schema_version is missing: ${schema_error}")
endif()

if(NOT schema_version STREQUAL "1")
  message(FATAL_ERROR "Expected schema_version 1, got ${schema_version}")
endif()

string(JSON fixture_count ERROR_VARIABLE fixture_error LENGTH "${manifest_json}" fixtures)
if(fixture_error)
  message(FATAL_ERROR "Manifest fixtures array is missing: ${fixture_error}")
endif()

if(fixture_count LESS 1)
  message(FATAL_ERROR "Manifest must define at least one fixture.")
endif()

math(EXPR last_index "${fixture_count} - 1")
foreach(index RANGE 0 ${last_index})
  foreach(required_key IN ITEMS id label source_type relative_path volume_role case_mode encryption snapshots expected_policy)
    string(JSON value ERROR_VARIABLE value_error GET "${manifest_json}" fixtures ${index} ${required_key})
    if(value_error)
      message(FATAL_ERROR "Fixture ${index} is missing key '${required_key}': ${value_error}")
    endif()
  endforeach()

  string(JSON compression_length ERROR_VARIABLE compression_error LENGTH "${manifest_json}" fixtures ${index} compression_algorithms)
  if(compression_error)
    message(FATAL_ERROR "Fixture ${index} is missing compression_algorithms: ${compression_error}")
  endif()

  string(JSON feature_length ERROR_VARIABLE feature_error LENGTH "${manifest_json}" fixtures ${index} feature_flags)
  if(feature_error)
    message(FATAL_ERROR "Fixture ${index} is missing feature_flags: ${feature_error}")
  endif()
endforeach()

message(STATUS "Fixture manifest validation passed for ${MANIFEST_PATH}")

