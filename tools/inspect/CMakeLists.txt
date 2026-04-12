add_executable(
  orchard_inspect
  src/main.cpp
)

target_link_libraries(
  orchard_inspect
  PRIVATE
    orchard_apfs_core
    orchard_blockio
)

target_compile_definitions(
  orchard_inspect
  PRIVATE
    ORCHARD_VERSION="${PROJECT_VERSION}"
)

orchard_configure_target(orchard_inspect)

set_target_properties(
  orchard_inspect
  PROPERTIES
    OUTPUT_NAME "orchard-inspect"
)

