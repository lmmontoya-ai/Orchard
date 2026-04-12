set(ORCHARD_SAMPLE_MANIFEST "${PROJECT_SOURCE_DIR}/tests/corpus/manifests/sample-fixtures.json")
set(ORCHARD_SAMPLE_IMAGE "${PROJECT_SOURCE_DIR}/tests/corpus/samples/plain-user-data.img")

add_test(
  NAME orchard_validate_fixture_manifest
  COMMAND
    "${CMAKE_COMMAND}"
      -DMANIFEST_PATH=${ORCHARD_SAMPLE_MANIFEST}
      -P "${CMAKE_CURRENT_SOURCE_DIR}/validate_fixture_manifest.cmake"
)

add_test(
  NAME orchard_inspect_stub_output
  COMMAND
    "${CMAKE_COMMAND}"
      -DEXECUTABLE_PATH=$<TARGET_FILE:orchard_inspect>
      -DTARGET_PATH=${ORCHARD_SAMPLE_IMAGE}
      -P "${CMAKE_CURRENT_SOURCE_DIR}/validate_orchard_inspect_output.cmake"
)
