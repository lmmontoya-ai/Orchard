#include "orchard/apfs/file_read.h"

#include <algorithm>
#include <limits>

namespace orchard::apfs {
namespace {

constexpr std::uint64_t kSparseFlag = 0x00000200ULL;

} // namespace

blockio::Result<FileMetadata> GetFileMetadata(const VolumeContext& volume,
                                              const std::uint64_t inode_id) {
  auto inode_result = volume.GetInode(inode_id);
  if (!inode_result.ok()) {
    return inode_result.error();
  }

  FileMetadata metadata;
  metadata.object_id = inode_id;
  metadata.logical_size = inode_result.value().logical_size;
  metadata.kind = inode_result.value().kind;
  metadata.sparse = (inode_result.value().internal_flags & kSparseFlag) != 0U;

  auto xattr_result = volume.FindXattr(inode_id, kCompressionXattrName);
  if (!xattr_result.ok()) {
    return xattr_result.error();
  }
  if (xattr_result.value().has_value()) {
    auto compression_result = ParseCompressionInfo(std::span<const std::uint8_t>(
        xattr_result.value()->data.data(), xattr_result.value()->data.size()));
    if (!compression_result.ok()) {
      return compression_result.error();
    }
    metadata.compression = compression_result.value();
    if (metadata.compression.uncompressed_size != 0U) {
      metadata.logical_size = metadata.compression.uncompressed_size;
    }
  }

  return metadata;
}

blockio::Result<std::vector<std::uint8_t>> ReadFileRange(const VolumeContext& volume,
                                                         const std::uint64_t inode_id,
                                                         const std::uint64_t offset,
                                                         const std::size_t size) {
  auto metadata_result = GetFileMetadata(volume, inode_id);
  if (!metadata_result.ok()) {
    return metadata_result.error();
  }

  if (!IsRegularFile(metadata_result.value().kind) &&
      metadata_result.value().kind != InodeKind::kSymlink) {
    return MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                         "Requested inode is not readable as a file.");
  }
  if (size == 0U || offset >= metadata_result.value().logical_size) {
    return std::vector<std::uint8_t>{};
  }

  const auto remaining = metadata_result.value().logical_size - offset;
  const auto bounded_size = static_cast<std::size_t>(
      std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(size)));

  auto compression_xattr_result = volume.FindXattr(inode_id, kCompressionXattrName);
  if (!compression_xattr_result.ok()) {
    return compression_xattr_result.error();
  }
  if (compression_xattr_result.value().has_value()) {
    auto decoded_result = DecodeCompressionPayload(
        std::span<const std::uint8_t>(compression_xattr_result.value()->data.data(),
                                      compression_xattr_result.value()->data.size()));
    if (!decoded_result.ok()) {
      return decoded_result.error();
    }

    return std::vector<std::uint8_t>(
        decoded_result.value().begin() + static_cast<std::ptrdiff_t>(offset),
        decoded_result.value().begin() + static_cast<std::ptrdiff_t>(offset + bounded_size));
  }

  auto extents_result = volume.ListFileExtents(inode_id);
  if (!extents_result.ok()) {
    return extents_result.error();
  }

  std::vector<std::uint8_t> bytes(bounded_size, 0U);
  const auto range_start = offset;
  const auto range_end = offset + bounded_size;

  for (const auto& extent : extents_result.value()) {
    const auto extent_start = extent.key.logical_address;
    const auto extent_end = extent_start + extent.length;
    if (extent_end <= range_start || extent_start >= range_end) {
      continue;
    }

    const auto copy_start = std::max<std::uint64_t>(range_start, extent_start);
    const auto copy_end = std::min<std::uint64_t>(range_end, extent_end);
    const auto extent_relative_offset = copy_start - extent_start;
    const auto destination_offset = copy_start - range_start;
    const auto read_size = static_cast<std::size_t>(copy_end - copy_start);

    const auto physical_byte_offset = extent_relative_offset;
    const auto physical_block =
        extent.physical_block + (physical_byte_offset / volume.block_size());
    const auto block_offset = physical_byte_offset % volume.block_size();
    auto data_result = volume.ReadPhysicalBytes(physical_block, block_offset, read_size);
    if (!data_result.ok()) {
      return data_result.error();
    }

    std::copy(data_result.value().begin(), data_result.value().end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(destination_offset));
  }

  return bytes;
}

blockio::Result<std::vector<std::uint8_t>> ReadWholeFile(const VolumeContext& volume,
                                                         const std::uint64_t inode_id) {
  auto metadata_result = GetFileMetadata(volume, inode_id);
  if (!metadata_result.ok()) {
    return metadata_result.error();
  }
  if (metadata_result.value().logical_size >
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return MakeApfsError(blockio::ErrorCode::kOutOfRange,
                         "Requested file exceeds the supported in-memory read size.");
  }

  return ReadFileRange(volume, inode_id, 0U,
                       static_cast<std::size_t>(metadata_result.value().logical_size));
}

} // namespace orchard::apfs
