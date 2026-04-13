#include "orchard/fs_winfsp/directory_query.h"

#include <utility>

#include "orchard/apfs/file_read.h"
#include "orchard/fs_winfsp/file_info.h"
#include "orchard/fs_winfsp/path_bridge.h"

namespace orchard::fs_winfsp {
namespace {

struct ChildPathRequest {
  std::string_view parent;
  std::string_view child;
};

std::string ComposeChildPath(const ChildPathRequest& request) {
  if (request.parent == "/") {
    return std::string("/") + std::string(request.child);
  }

  std::string path(request.parent);
  if (path.empty() || path.back() != '/') {
    path.push_back('/');
  }
  path.append(request.child);
  return path;
}

} // namespace

blockio::Result<std::vector<DirectoryQueryEntry>>
BuildDirectoryQueryEntries(const orchard::apfs::VolumeContext& volume,
                           const FileNode& directory_node,
                           const std::span<const orchard::apfs::DirectoryEntryRecord> entries) {
  if (directory_node.metadata.kind != orchard::apfs::InodeKind::kDirectory) {
    return orchard::apfs::MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                                        "Directory query requested for a non-directory node.");
  }

  std::vector<DirectoryQueryEntry> query_entries;
  query_entries.reserve(entries.size());

  for (const auto& entry : entries) {
    auto metadata_result = orchard::apfs::GetFileMetadata(volume, entry.file_id);
    if (!metadata_result.ok()) {
      return metadata_result.error();
    }

    FileNode node;
    node.inode_id = entry.file_id;
    node.parent_inode_id = directory_node.inode_id;
    node.normalized_path = ComposeChildPath(ChildPathRequest{
        .parent = directory_node.normalized_path,
        .child = entry.key.name,
    });
    node.metadata = metadata_result.value();

    auto wide_name_result = Utf8ToWide(entry.key.name);
    if (!wide_name_result.ok()) {
      return wide_name_result.error();
    }

    query_entries.push_back(DirectoryQueryEntry{
        .file_name = std::move(wide_name_result.value()),
        .file_info = BuildBasicFileInfo(node, volume.block_size()),
    });
  }

  return query_entries;
}

std::vector<DirectoryQueryEntry>
FilterDirectoryQueryEntries(const std::span<const DirectoryQueryEntry> entries,
                            const DirectoryQueryRequest& request) {
  std::vector<DirectoryQueryEntry> filtered;
  filtered.reserve(entries.size());

  for (const auto& entry : entries) {
    if (request.marker.has_value() &&
        CompareDirectoryNames(entry.file_name, *request.marker, request.case_insensitive) <= 0) {
      continue;
    }
    if (!MatchesDirectoryPattern(entry.file_name, request.pattern, request.case_insensitive)) {
      continue;
    }

    filtered.push_back(entry);
  }

  return filtered;
}

std::size_t EstimateDirectoryQueryEntryBytes(const DirectoryQueryEntry& entry,
                                             const std::size_t base_entry_size) noexcept {
  return base_entry_size + (entry.file_name.size() * sizeof(wchar_t));
}

DirectoryQueryPage PaginateDirectoryQueryEntries(const std::span<const DirectoryQueryEntry> entries,
                                                 const DirectoryQueryPaginationConfig& config) {
  DirectoryQueryPage page;
  if (entries.empty()) {
    return page;
  }

  std::size_t used_bytes = 0U;
  for (const auto& entry : entries) {
    const auto entry_bytes = EstimateDirectoryQueryEntryBytes(entry, config.base_entry_size);
    if (entry_bytes > config.max_bytes) {
      page.truncated = true;
      return page;
    }
    if (!page.entries.empty() && used_bytes + entry_bytes > config.max_bytes) {
      page.truncated = true;
      return page;
    }

    used_bytes += entry_bytes;
    page.entries.push_back(entry);
    page.last_emitted_name = page.entries.back().file_name;
  }

  return page;
}

} // namespace orchard::fs_winfsp
