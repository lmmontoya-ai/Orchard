set(_orchard_winfsp_sources
  src/file_info.cpp
  src/mount.cpp
  src/path_bridge.cpp
)

set(_orchard_winfsp_public_headers
  include/orchard/fs_winfsp/file_info.h
  include/orchard/fs_winfsp/filesystem.h
  include/orchard/fs_winfsp/mount.h
  include/orchard/fs_winfsp/path_bridge.h
  include/orchard/fs_winfsp/types.h
)

set(_orchard_winfsp_mode "${ORCHARD_ENABLE_WINFSP}")
string(TOUPPER "${_orchard_winfsp_mode}" _orchard_winfsp_mode)

set(_orchard_winfsp_have_sdk OFF)
if(NOT _orchard_winfsp_mode STREQUAL "OFF")
  find_package(WinFsp QUIET)
  if(WinFsp_FOUND)
    set(_orchard_winfsp_have_sdk ON)
  elseif(_orchard_winfsp_mode STREQUAL "ON")
    message(FATAL_ERROR
      "ORCHARD_ENABLE_WINFSP=ON was requested, but the WinFsp developer SDK was not found. "
      "Set WINFSP_ROOT_DIR to a root containing inc/winfsp/winfsp.h and lib/winfsp-*.lib.")
  else()
    message(STATUS "WinFsp developer SDK not found; building the Orchard WinFsp stub adapter.")
  endif()
endif()

set(ORCHARD_WINFSP_ENABLED ${_orchard_winfsp_have_sdk})

add_library(
  orchard_fs_winfsp
  STATIC
    ${_orchard_winfsp_public_headers}
    ${_orchard_winfsp_sources}
)

if(_orchard_winfsp_have_sdk)
  target_sources(
    orchard_fs_winfsp
    PRIVATE
      src/filesystem.cpp
  )
else()
  target_sources(
    orchard_fs_winfsp
    PRIVATE
      src/filesystem_stub.cpp
  )
endif()

target_include_directories(
  orchard_fs_winfsp
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(
  orchard_fs_winfsp
  PUBLIC
    orchard_apfs_core
    orchard_blockio
)

if(_orchard_winfsp_have_sdk)
  target_link_libraries(
    orchard_fs_winfsp
    PUBLIC
      WinFsp::WinFsp
    PRIVATE
      advapi32
  )
  target_compile_definitions(
    orchard_fs_winfsp
    PUBLIC
      ORCHARD_HAS_WINFSP=1
  )
else()
  target_compile_definitions(
    orchard_fs_winfsp
    PUBLIC
      ORCHARD_HAS_WINFSP=0
  )
endif()

orchard_configure_target(orchard_fs_winfsp)
