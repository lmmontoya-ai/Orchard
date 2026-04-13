#include "orchard/fs_winfsp/file_info.h"

#include <Windows.h>

namespace orchard::fs_winfsp {
namespace {

std::uint64_t AlignUp(const std::uint64_t value, const std::uint32_t alignment) noexcept {
  if (alignment == 0U || value == 0U) {
    return value;
  }

  const auto remainder = value % alignment;
  if (remainder == 0U) {
    return value;
  }

  return value + (alignment - remainder);
}

} // namespace

BasicFileInfo BuildBasicFileInfo(const FileNode& node, const std::uint32_t block_size) noexcept {
  BasicFileInfo info;
  info.index_number = node.inode_id;
  info.file_attributes = FILE_ATTRIBUTE_READONLY;

  switch (node.metadata.kind) {
  case orchard::apfs::InodeKind::kDirectory:
    info.file_attributes |= FILE_ATTRIBUTE_DIRECTORY;
    info.file_size = 0U;
    info.allocation_size = 0U;
    break;
  case orchard::apfs::InodeKind::kRegularFile:
  case orchard::apfs::InodeKind::kSymlink:
  case orchard::apfs::InodeKind::kUnknown:
    info.file_size = node.metadata.logical_size;
    info.allocation_size = AlignUp(node.metadata.logical_size, block_size);
    break;
  }

  return info;
}

} // namespace orchard::fs_winfsp
