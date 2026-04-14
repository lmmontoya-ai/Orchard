#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "orchard/blockio/result.h"

namespace orchard::fs_winfsp {

struct SymlinkReparseRequest {
  std::wstring mount_point;
  std::string target;
};

struct WindowsSymlinkTarget {
  std::wstring substitute_name;
  std::wstring print_name;
  bool relative = false;
};

blockio::Result<WindowsSymlinkTarget> TranslateSymlinkTarget(const SymlinkReparseRequest& request);
blockio::Result<std::vector<std::uint8_t>>
BuildSymlinkReparseData(const SymlinkReparseRequest& request);

} // namespace orchard::fs_winfsp
