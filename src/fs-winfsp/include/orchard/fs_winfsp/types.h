#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "orchard/apfs/file_read.h"
#include "orchard/apfs/path_lookup.h"

namespace orchard::fs_winfsp {

struct VolumeSelector {
  std::optional<std::uint64_t> object_id;
  std::optional<std::string> name;
};

struct MountConfig {
  std::filesystem::path target_path;
  std::wstring mount_point;
  VolumeSelector selector;
  bool require_read_only_mount = true;
  bool allow_downgrade_from_readwrite = true;
};

struct FileNode {
  std::uint64_t inode_id = 0;
  std::uint64_t parent_inode_id = 0;
  std::string normalized_path;
  orchard::apfs::FileMetadata metadata;
  std::optional<std::string> symlink_target;
  bool symlink_reparse_eligible = false;
  bool metadata_complete = false;
};

struct BasicFileInfo {
  std::uint32_t file_attributes = 0;
  std::uint32_t reparse_tag = 0;
  std::uint64_t allocation_size = 0;
  std::uint64_t file_size = 0;
  std::uint64_t creation_time = 0;
  std::uint64_t last_access_time = 0;
  std::uint64_t last_write_time = 0;
  std::uint64_t change_time = 0;
  std::uint64_t index_number = 0;
  std::uint32_t hard_links = 0;
  std::uint32_t ea_size = 0;
};

struct DirectoryMarker {
  std::optional<std::wstring> value;
};

} // namespace orchard::fs_winfsp
