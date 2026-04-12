add_library(
  orchard_fs_winfsp
  STATIC
    src/fs_winfsp_placeholder.cpp
)

target_include_directories(
  orchard_fs_winfsp
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

orchard_configure_target(orchard_fs_winfsp)

