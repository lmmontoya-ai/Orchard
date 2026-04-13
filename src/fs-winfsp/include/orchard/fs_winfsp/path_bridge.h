#pragma once

#include <string>
#include <string_view>

#include "orchard/blockio/result.h"

namespace orchard::fs_winfsp {

blockio::Result<std::string> NormalizeWindowsPath(std::wstring_view windows_path);
blockio::Result<std::wstring> OrchardPathToWindowsPath(std::string_view orchard_path);
blockio::Result<std::wstring> Utf8ToWide(std::string_view text);
blockio::Result<std::string> WideToUtf8(std::wstring_view text);
int CompareDirectoryNames(std::wstring_view left, std::wstring_view right,
                          bool case_insensitive) noexcept;

} // namespace orchard::fs_winfsp
