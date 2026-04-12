add_library(
  orchard_apfs_core
  STATIC
    src/inspection.cpp
    src/probe.cpp
)

target_include_directories(
  orchard_apfs_core
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(
  orchard_apfs_core
  PUBLIC
    orchard_blockio
)

orchard_configure_target(orchard_apfs_core)

