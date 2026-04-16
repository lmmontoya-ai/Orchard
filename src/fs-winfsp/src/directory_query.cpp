#include "orchard/fs_winfsp/directory_query.h"

#include <utility>

#include "orchard/apfs/file_read.h"
#include "orchard/fs_winfsp/file_info.h"
#include "orchard/fs_winfsp/mount.h"
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

orchard::apfs::FileMetadata
MakeFallbackMetadata(const orchard::apfs::DirectoryEntryRecord& entry) noexcept {
  orchard::apfs::FileMetadata metadata;
  metadata.object_id = entry.file_id;
  metadata.kind = entry.kind;
  return metadata;
}

orchard::apfs::FileMetadata
MakeMetadataFromInode(const orchard::apfs::InodeRecord& inode) noexcept {
  orchard::apfs::FileMetadata metadata;
  metadata.object_id = inode.key.header.object_id;
  metadata.logical_size = inode.logical_size;
  metadata.allocated_size = inode.allocated_size;
  metadata.internal_flags = inode.internal_flags;
  metadata.creation_time_unix_nanos = inode.creation_time_unix_nanos;
  metadata.last_access_time_unix_nanos = inode.last_access_time_unix_nanos;
  metadata.last_write_time_unix_nanos = inode.last_write_time_unix_nanos;
  metadata.change_time_unix_nanos = inode.change_time_unix_nanos;
  metadata.kind = inode.kind;
  metadata.child_count = inode.child_count;
  metadata.link_count = inode.link_count;
  metadata.mode = inode.mode;
  return metadata;
}

} // namespace

blockio::Result<DirectoryQueryEntry>
BuildDirectoryQueryEntry(const MountedVolume& mounted_volume, const FileNode& directory_node,
                         const orchard::apfs::DirectoryEntryRecord& entry) {
  const auto& volume = mounted_volume.volume_context();
  if (directory_node.metadata.kind != orchard::apfs::InodeKind::kDirectory) {
    return orchard::apfs::MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                                        "Directory query requested for a non-directory node.");
  }

  const auto normalized_child_path = ComposeChildPath(ChildPathRequest{
      .parent = directory_node.normalized_path,
      .child = entry.key.name,
  });

  FileNode node;
  node.inode_id = entry.file_id;
  node.parent_inode_id = directory_node.inode_id;
  node.normalized_path = normalized_child_path;
  if (entry.inode.has_value()) {
    node.metadata = MakeMetadataFromInode(*entry.inode);
  } else {
    auto metadata_result = orchard::apfs::GetFileMetadata(volume, entry.file_id);
    node.metadata = metadata_result.ok() ? metadata_result.value() : MakeFallbackMetadata(entry);
  }
  if (node.metadata.kind == orchard::apfs::InodeKind::kSymlink) {
    auto projected_node_result = mounted_volume.ResolveFileNode(normalized_child_path);
    if (projected_node_result.ok()) {
      node = std::move(projected_node_result.value());
    }
  }

  auto wide_name_result = Utf8ToWide(entry.key.name);
  if (!wide_name_result.ok()) {
    return wide_name_result.error();
  }

  return DirectoryQueryEntry{
      .file_name = std::move(wide_name_result.value()),
      .file_info = BuildBasicFileInfo(node, volume.block_size()),
  };
}

blockio::Result<std::vector<DirectoryQueryEntry>>
BuildDirectoryQueryEntries(const MountedVolume& mounted_volume, const FileNode& directory_node,
                           const std::span<const orchard::apfs::DirectoryEntryRecord> entries) {
  if (directory_node.metadata.kind != orchard::apfs::InodeKind::kDirectory) {
    return orchard::apfs::MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                                        "Directory query requested for a non-directory node.");
  }

  std::vector<DirectoryQueryEntry> query_entries;
  query_entries.reserve(entries.size());

  for (const auto& entry : entries) {
    auto query_entry_result = BuildDirectoryQueryEntry(mounted_volume, directory_node, entry);
    if (!query_entry_result.ok()) {
      return query_entry_result.error();
    }

    query_entries.push_back(std::move(query_entry_result.value()));
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

DirectoryQueryPage
PaginateFilteredDirectoryQueryEntries(const std::span<const DirectoryQueryEntry> entries,
                                      const DirectoryQueryRequest& request,
                                      const DirectoryQueryPaginationConfig& config) {
  DirectoryQueryPage page;
  if (entries.empty()) {
    return page;
  }

  auto current = entries.begin();
  if (request.marker.has_value()) {
    current = std::upper_bound(
        entries.begin(), entries.end(), *request.marker,
        [&request](const std::wstring& marker_name, const DirectoryQueryEntry& entry) {
          return CompareDirectoryNames(marker_name, entry.file_name, request.case_insensitive) < 0;
        });
  }

  std::size_t used_bytes = 0U;
  for (; current != entries.end(); ++current) {
    if (!MatchesDirectoryPattern(current->file_name, request.pattern, request.case_insensitive)) {
      continue;
    }

    const auto entry_bytes = EstimateDirectoryQueryEntryBytes(*current, config.base_entry_size);
    if (entry_bytes > config.max_bytes) {
      page.truncated = true;
      return page;
    }
    if (!page.entries.empty() && used_bytes + entry_bytes > config.max_bytes) {
      page.truncated = true;
      return page;
    }

    used_bytes += entry_bytes;
    page.entries.push_back(*current);
    page.last_emitted_name = page.entries.back().file_name;
  }

  return page;
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
