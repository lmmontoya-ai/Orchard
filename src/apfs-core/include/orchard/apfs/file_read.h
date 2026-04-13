#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "orchard/apfs/path_lookup.h"

namespace orchard::apfs {

struct FileMetadata {
  std::uint64_t object_id = 0;
  std::uint64_t logical_size = 0;
  InodeKind kind = InodeKind::kUnknown;
  bool sparse = false;
  CompressionInfo compression;
};

blockio::Result<FileMetadata> GetFileMetadata(const VolumeContext& volume, std::uint64_t inode_id);

blockio::Result<std::vector<std::uint8_t>> ReadFileRange(const VolumeContext& volume,
                                                         std::uint64_t inode_id,
                                                         std::uint64_t offset, std::size_t size);

blockio::Result<std::vector<std::uint8_t>> ReadWholeFile(const VolumeContext& volume,
                                                         std::uint64_t inode_id);

} // namespace orchard::apfs
