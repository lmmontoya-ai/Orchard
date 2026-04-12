add_library(
  orchard_orchardctl
  STATIC
    src/orchardctl_placeholder.cpp
)

target_include_directories(
  orchard_orchardctl
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

orchard_configure_target(orchard_orchardctl)

