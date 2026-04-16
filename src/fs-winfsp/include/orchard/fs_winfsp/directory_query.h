#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "orchard/blockio/result.h"
#include "orchard/fs_winfsp/types.h"

namespace orchard::fs_winfsp {

class MountedVolume;

struct DirectoryQueryEntry {
  std::wstring file_name;
  BasicFileInfo file_info;
};

struct DirectoryQueryRequest {
  std::optional<std::wstring> marker;
  std::wstring pattern;
  bool case_insensitive = false;
};

struct DirectoryQueryPage {
  std::vector<DirectoryQueryEntry> entries;
  std::optional<std::wstring> last_emitted_name;
  bool truncated = false;
};

struct DirectoryQueryPaginationConfig {
  std::size_t max_bytes = 0;
  std::size_t base_entry_size = 0;
};

blockio::Result<DirectoryQueryEntry>
BuildDirectoryQueryEntry(const MountedVolume& mounted_volume, const FileNode& directory_node,
                         const orchard::apfs::DirectoryEntryRecord& entry);

blockio::Result<std::vector<DirectoryQueryEntry>>
BuildDirectoryQueryEntries(const MountedVolume& mounted_volume, const FileNode& directory_node,
                           std::span<const orchard::apfs::DirectoryEntryRecord> entries);

std::vector<DirectoryQueryEntry>
FilterDirectoryQueryEntries(std::span<const DirectoryQueryEntry> entries,
                            const DirectoryQueryRequest& request);
std::size_t EstimateDirectoryQueryEntryBytes(const DirectoryQueryEntry& entry,
                                             std::size_t base_entry_size) noexcept;
DirectoryQueryPage
PaginateFilteredDirectoryQueryEntries(std::span<const DirectoryQueryEntry> entries,
                                      const DirectoryQueryRequest& request,
                                      const DirectoryQueryPaginationConfig& config);
DirectoryQueryPage PaginateDirectoryQueryEntries(std::span<const DirectoryQueryEntry> entries,
                                                 const DirectoryQueryPaginationConfig& config);

} // namespace orchard::fs_winfsp
