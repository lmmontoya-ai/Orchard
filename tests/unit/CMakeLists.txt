orchard_add_cpp_test(
  orchard_unit_blockio
  SOURCES
    inspection_target_tests.cpp
  LIBRARIES
    orchard_blockio
)

orchard_add_cpp_test(
  orchard_unit_apfs
  SOURCES
    probe_tests.cpp
  LIBRARIES
    orchard_apfs_core
)

