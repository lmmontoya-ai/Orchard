#include "orchard/fs_winfsp/file_info.h"

#include <Windows.h>

namespace orchard::fs_winfsp {
namespace {

constexpr std::uint64_t kWindowsUnixEpochDelta100Ns = 116444736000000000ULL;

std::uint64_t UnixNanosToWindowsFileTime(const std::uint64_t unix_nanos) noexcept {
  if (unix_nanos == 0U) {
    return 0U;
  }

  return kWindowsUnixEpochDelta100Ns + (unix_nanos / 100U);
}

} // namespace

BasicFileInfo BuildBasicFileInfo(const FileNode& node, const std::uint32_t block_size) noexcept {
  static_cast<void>(block_size);

  BasicFileInfo info;
  info.index_number = node.inode_id;
  info.hard_links = node.metadata.link_count;
  info.file_attributes = FILE_ATTRIBUTE_READONLY;
  info.creation_time = UnixNanosToWindowsFileTime(node.metadata.creation_time_unix_nanos);
  info.last_access_time = UnixNanosToWindowsFileTime(node.metadata.last_access_time_unix_nanos);
  info.last_write_time = UnixNanosToWindowsFileTime(node.metadata.last_write_time_unix_nanos);
  info.change_time = UnixNanosToWindowsFileTime(node.metadata.change_time_unix_nanos);

  switch (node.metadata.kind) {
  case orchard::apfs::InodeKind::kDirectory:
    info.file_attributes |= FILE_ATTRIBUTE_DIRECTORY;
    info.file_size = 0U;
    info.allocation_size = node.metadata.allocated_size;
    break;
  case orchard::apfs::InodeKind::kRegularFile:
    info.file_size = node.metadata.logical_size;
    info.allocation_size = node.metadata.allocated_size;
    break;
  case orchard::apfs::InodeKind::kSymlink:
    if (node.symlink_reparse_eligible) {
      info.file_attributes |= FILE_ATTRIBUTE_REPARSE_POINT;
      info.reparse_tag = IO_REPARSE_TAG_SYMLINK;
    }
    info.file_size = node.metadata.logical_size;
    info.allocation_size = node.metadata.allocated_size;
    break;
  case orchard::apfs::InodeKind::kUnknown:
    info.file_size = node.metadata.logical_size;
    info.allocation_size = node.metadata.allocated_size;
    break;
  }

  return info;
}

} // namespace orchard::fs_winfsp
