#include "orchard/fs_winfsp/directory_query.h"

#include <utility>

#include "orchard/apfs/file_read.h"
#include "orchard/fs_winfsp/file_info.h"
#include "orchard/fs_winfsp/path_bridge.h"

namespace orchard::fs_winfsp {
namespace {

std::string ComposeChildPath(const std::string_view parent, const std::string_view child) {
  if (parent == "/") {
    return std::string("/") + std::string(child);
  }

  std::string path(parent);
  if (path.empty() || path.back() != '/') {
    path.push_back('/');
  }
  path.append(child);
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
    node.normalized_path = ComposeChildPath(directory_node.normalized_path, entry.key.name);
    node.metadata = std::move(metadata_result.value());

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

} // namespace orchard::fs_winfsp
