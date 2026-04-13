add_executable(
  orchard-mount-smoke
  src/main.cpp
)

target_link_libraries(
  orchard-mount-smoke
  PRIVATE
    orchard_fs_winfsp
)

orchard_configure_target(orchard-mount-smoke)

if(TARGET WinFsp::WinFsp)
  orchard_copy_runtime_dlls(orchard-mount-smoke)
endif()
