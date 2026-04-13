#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "orchard/apfs/volume.h"
#include "orchard/blockio/result.h"
#include "orchard/fs_winfsp/types.h"

namespace orchard::fs_winfsp {

struct DirectoryQueryEntry {
  std::wstring file_name;
  BasicFileInfo file_info;
};

struct DirectoryQueryRequest {
  std::optional<std::wstring> marker;
  std::wstring pattern;
  bool case_insensitive = false;
};

blockio::Result<std::vector<DirectoryQueryEntry>>
BuildDirectoryQueryEntries(const orchard::apfs::VolumeContext& volume,
                           const FileNode& directory_node,
                           std::span<const orchard::apfs::DirectoryEntryRecord> entries);

std::vector<DirectoryQueryEntry>
FilterDirectoryQueryEntries(std::span<const DirectoryQueryEntry> entries,
                            const DirectoryQueryRequest& request);

} // namespace orchard::fs_winfsp
