cmake_minimum_required(VERSION 3.25)

project(
  Orchard
  VERSION 0.1.0
  DESCRIPTION "Open-source APFS support for Windows"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ORCHARD_BUILD_TESTS "Build Orchard test targets." ON)
set(ORCHARD_ENABLE_WINFSP "AUTO" CACHE STRING "Build the WinFsp adapter (AUTO, ON, OFF).")
set_property(CACHE ORCHARD_ENABLE_WINFSP PROPERTY STRINGS AUTO ON OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(CTest)
include(OrchardProject)
include(OrchardTest)

add_subdirectory(src)
add_subdirectory(tools)

if(BUILD_TESTING AND ORCHARD_BUILD_TESTS)
  add_subdirectory(tests)
endif()

orchard_add_maintenance_targets()
