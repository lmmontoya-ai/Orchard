#include "orchard/apfs/link_read.h"

#include <algorithm>

namespace orchard::apfs {

blockio::Result<std::string> ReadSymlinkTarget(const VolumeContext& volume,
                                               const std::uint64_t inode_id) {
  auto metadata_result = GetFileMetadata(volume, inode_id);
  if (!metadata_result.ok()) {
    return metadata_result.error();
  }
  if (metadata_result.value().kind != InodeKind::kSymlink) {
    return MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                         "Requested inode is not an APFS symlink.");
  }

  auto bytes_result = ReadWholeFile(volume, inode_id);
  if (!bytes_result.ok()) {
    return bytes_result.error();
  }
  if (std::find(bytes_result.value().begin(), bytes_result.value().end(), 0U) !=
      bytes_result.value().end()) {
    return MakeApfsError(blockio::ErrorCode::kUnsupportedTarget,
                         "Symlink targets containing NUL bytes are not supported.");
  }

  return std::string(bytes_result.value().begin(), bytes_result.value().end());
}

blockio::Result<LinkInfo> GetLinkInfo(const VolumeContext& volume, const std::uint64_t inode_id) {
  auto metadata_result = GetFileMetadata(volume, inode_id);
  if (!metadata_result.ok()) {
    return metadata_result.error();
  }

  LinkInfo info;
  info.inode_id = inode_id;
  info.kind = metadata_result.value().kind;
  info.link_count = metadata_result.value().link_count;
  if (info.kind == InodeKind::kSymlink) {
    auto target_result = ReadSymlinkTarget(volume, inode_id);
    if (!target_result.ok()) {
      return target_result.error();
    }
    info.symlink_target = std::move(target_result.value());
  }

  return info;
}

} // namespace orchard::apfs
