#include "orchard/apfs/path_lookup.h"

#include <sstream>

namespace orchard::apfs {
namespace {

std::string FoldName(const std::string_view text) {
  std::string folded(text);
  for (auto& character : folded) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return folded;
}

bool NamesEqual(const std::string_view left, const std::string_view right,
                const bool case_insensitive) {
  if (!case_insensitive) {
    return left == right;
  }
  return FoldName(left) == FoldName(right);
}

std::vector<std::string> SplitPath(const std::string_view path) {
  std::vector<std::string> components;
  std::size_t current = 0U;
  while (current < path.size()) {
    while (current < path.size() && path[current] == '/') {
      ++current;
    }
    if (current >= path.size()) {
      break;
    }

    auto next = path.find('/', current);
    if (next == std::string_view::npos) {
      next = path.size();
    }
    components.emplace_back(path.substr(current, next - current));
    current = next;
  }
  return components;
}

std::string JoinNormalizedPath(const std::vector<std::string>& components) {
  if (components.empty()) {
    return "/";
  }

  std::string path;
  for (const auto& component : components) {
    path.push_back('/');
    path += component;
  }
  return path;
}

} // namespace

blockio::Result<ResolvedPath> LookupPath(const VolumeContext& volume, const std::string_view path) {
  std::size_t path_components_walked = 0U;
  std::size_t directory_enumerations = 0U;

  auto current_inode_result = volume.GetInode(volume.root_directory_object_id());
  if (!current_inode_result.ok()) {
    volume.RecordPathLookup(path_components_walked, directory_enumerations);
    return current_inode_result.error();
  }

  ResolvedPath resolved;
  resolved.normalized_path = "/";
  resolved.inode = current_inode_result.value();
  std::vector<std::string> normalized_components;

  for (const auto& component : SplitPath(path)) {
    if (component == ".") {
      continue;
    }

    if (component == "..") {
      const auto parent_inode_id = resolved.inode.parent_id;
      auto parent_result = volume.GetInode(parent_inode_id);
      if (!parent_result.ok()) {
        volume.RecordPathLookup(path_components_walked, directory_enumerations);
        return parent_result.error();
      }
      resolved.inode = parent_result.value();
      if (!normalized_components.empty()) {
        normalized_components.pop_back();
      }
      resolved.normalized_path = JoinNormalizedPath(normalized_components);
      continue;
    }

    if (!IsDirectory(resolved.inode.kind)) {
      volume.RecordPathLookup(path_components_walked, directory_enumerations);
      return MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                           "Path traversal encountered a non-directory inode.");
    }

    ++directory_enumerations;
    auto entries_result = volume.ListDirectoryEntries(resolved.inode.key.header.object_id);
    if (!entries_result.ok()) {
      volume.RecordPathLookup(path_components_walked, directory_enumerations);
      return entries_result.error();
    }

    const auto match =
        std::find_if(entries_result.value().begin(), entries_result.value().end(),
                     [&component, &volume](const DirectoryEntryRecord& entry) {
                       return NamesEqual(entry.key.name, component, volume.info().case_insensitive);
                     });
    if (match == entries_result.value().end()) {
      std::ostringstream message;
      message << "Path component '" << component << "' was not found in the directory.";
      volume.RecordPathLookup(path_components_walked, directory_enumerations);
      return MakeApfsError(blockio::ErrorCode::kNotFound, message.str());
    }

    if (match->inode.has_value()) {
      resolved.inode = *match->inode;
    } else {
      auto inode_result = volume.GetInode(match->file_id);
      if (!inode_result.ok()) {
        volume.RecordPathLookup(path_components_walked, directory_enumerations);
        return inode_result.error();
      }

      resolved.inode = inode_result.value();
    }
    ++path_components_walked;
    normalized_components.push_back(match->key.name);
    resolved.normalized_path = JoinNormalizedPath(normalized_components);
  }

  volume.RecordPathLookup(path_components_walked, directory_enumerations);
  return resolved;
}

blockio::Result<std::vector<DirectoryEntryRecord>>
ListDirectory(const VolumeContext& volume, const std::uint64_t directory_inode_id) {
  auto inode_result = volume.GetInode(directory_inode_id);
  if (!inode_result.ok()) {
    return inode_result.error();
  }
  if (!IsDirectory(inode_result.value().kind)) {
    return MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                         "Requested inode is not a directory.");
  }
  return volume.ListDirectoryEntries(directory_inode_id);
}

blockio::Result<std::vector<DirectoryEntryRecord>> ListDirectory(const VolumeContext& volume,
                                                                 const std::string_view path) {
  auto resolved_result = LookupPath(volume, path);
  if (!resolved_result.ok()) {
    return resolved_result.error();
  }
  return ListDirectory(volume, resolved_result.value().inode.key.header.object_id);
}

} // namespace orchard::apfs
