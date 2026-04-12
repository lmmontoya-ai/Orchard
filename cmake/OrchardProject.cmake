function(orchard_configure_target target_name)
  target_compile_features("${target_name}" PUBLIC cxx_std_20)

  if(MSVC)
    target_compile_options(
      "${target_name}"
      PRIVATE
        /W4
        /permissive-
        /EHsc
        /Zc:__cplusplus
    )
  else()
    target_compile_options(
      "${target_name}"
      PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
  endif()
endfunction()

function(orchard_add_maintenance_targets)
  find_program(ORCHARD_POWERSHELL NAMES pwsh powershell REQUIRED)

  add_custom_target(
    orchard_format
    COMMAND
      "${ORCHARD_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
      -File "${PROJECT_SOURCE_DIR}/tools/dev/orchard-format.ps1"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Formatting Orchard C++ sources with clang-format"
  )

  add_custom_target(
    orchard_format_check
    COMMAND
      "${ORCHARD_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
      -File "${PROJECT_SOURCE_DIR}/tools/dev/orchard-format.ps1"
      -Check
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Checking Orchard C++ formatting with clang-format"
  )

  add_custom_target(
    orchard_lint
    COMMAND
      "${ORCHARD_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
      -File "${PROJECT_SOURCE_DIR}/tools/dev/orchard-lint.ps1"
      -BuildDir "${PROJECT_BINARY_DIR}"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Running clang-tidy over Orchard translation units"
  )
endfunction()
