#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "orchard/apfs/file_read.h"

namespace orchard::apfs {

struct LinkInfo {
  std::uint64_t inode_id = 0;
  InodeKind kind = InodeKind::kUnknown;
  std::uint32_t link_count = 0;
  std::optional<std::string> symlink_target;
};

blockio::Result<std::string> ReadSymlinkTarget(const VolumeContext& volume, std::uint64_t inode_id);
blockio::Result<LinkInfo> GetLinkInfo(const VolumeContext& volume, std::uint64_t inode_id);

} // namespace orchard::apfs
