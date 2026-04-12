add_library(
  orchard_mount_service
  STATIC
    src/mount_service_placeholder.cpp
)

target_include_directories(
  orchard_mount_service
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

orchard_configure_target(orchard_mount_service)

