#pragma once

#include <cstdint>

#include "orchard/fs_winfsp/types.h"

namespace orchard::fs_winfsp {

BasicFileInfo BuildBasicFileInfo(const FileNode& node, std::uint32_t block_size) noexcept;

} // namespace orchard::fs_winfsp
