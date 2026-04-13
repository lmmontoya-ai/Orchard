orchard_add_cpp_test(
  orchard_unit_blockio
  SOURCES
    inspection_target_tests.cpp
  LIBRARIES
    orchard_blockio
)

orchard_add_cpp_test(
  orchard_unit_blockio_reader
  SOURCES
    reader_tests.cpp
  LIBRARIES
    orchard_blockio
)

orchard_add_cpp_test(
  orchard_unit_apfs
  SOURCES
    apfs_tests.cpp
  LIBRARIES
    orchard_apfs_core
    orchard_fs_winfsp
)

if(TARGET WinFsp::WinFsp)
  orchard_copy_runtime_dlls(orchard_unit_apfs)
endif()
