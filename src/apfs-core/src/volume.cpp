#include "orchard/apfs/volume.h"

#include <algorithm>
#include <limits>

#include "orchard/apfs/fs_search.h"

namespace orchard::apfs {
namespace {

blockio::Result<std::uint64_t> ComputeAbsoluteByteOffset(const VolumeRuntimeConfig& runtime,
                                                         const PhysicalReadRequest& request) {
  constexpr auto kMaxUint64 = std::numeric_limits<std::uint64_t>::max();
  if (request.physical_block_index >
      (kMaxUint64 - runtime.container_byte_offset) / runtime.block_size) {
    return MakeApfsError(blockio::ErrorCode::kOutOfRange,
                         "Physical read would overflow the APFS container byte range.");
  }

  const auto block_base =
      runtime.container_byte_offset + (request.physical_block_index * runtime.block_size);
  if (request.block_offset > kMaxUint64 - block_base) {
    return MakeApfsError(
        blockio::ErrorCode::kOutOfRange,
        "Physical read block offset would overflow the APFS container byte range.");
  }

  return block_base + request.block_offset;
}

std::string AsciiFold(const std::string_view text) {
  std::string folded(text);
  std::transform(folded.begin(), folded.end(), folded.begin(), [](const unsigned char value) {
    if (value >= 'A' && value <= 'Z') {
      return static_cast<char>(value - 'A' + 'a');
    }
    return static_cast<char>(value);
  });
  return folded;
}

blockio::Result<std::uint64_t> ResolveVolumeOmapBlock(const PhysicalObjectReader& object_reader,
                                                      const OmapResolver& container_omap,
                                                      const VolumeRuntimeConfig& runtime,
                                                      const VolumeInfo& info) {
  auto volume_omap_block_result =
      container_omap.ResolveOidToBlock(info.omap_oid, runtime.xid_limit);
  if (volume_omap_block_result.ok()) {
    return volume_omap_block_result.value();
  }

  if (volume_omap_block_result.error().code != blockio::ErrorCode::kNotFound) {
    return volume_omap_block_result.error();
  }

  auto direct_object_result = object_reader.ReadPhysicalObject(info.omap_oid);
  if (!direct_object_result.ok()) {
    return volume_omap_block_result.error();
  }

  auto direct_omap_result = ParseOmapSuperblock(direct_object_result.value().view());
  if (!direct_omap_result.ok()) {
    return volume_omap_block_result.error();
  }

  return info.omap_oid;
}

blockio::Result<std::uint64_t> ResolveRootTreeBlock(const PhysicalObjectReader& object_reader,
                                                    const OmapResolver& volume_omap,
                                                    const VolumeRuntimeConfig& runtime,
                                                    const VolumeInfo& info) {
  auto root_tree_block_result =
      volume_omap.ResolveOidToBlock(info.root_tree_oid, runtime.xid_limit);
  if (root_tree_block_result.ok()) {
    return root_tree_block_result.value();
  }

  if (root_tree_block_result.error().code != blockio::ErrorCode::kNotFound) {
    return root_tree_block_result.error();
  }

  auto direct_object_result = object_reader.ReadPhysicalObject(info.root_tree_oid);
  if (!direct_object_result.ok()) {
    return root_tree_block_result.error();
  }

  auto direct_node_result = ParseNode(direct_object_result.value().view(), runtime.block_size);
  if (!direct_node_result.ok()) {
    return root_tree_block_result.error();
  }

  return info.root_tree_oid;
}

} // namespace

VolumeContext::VolumeContext(const blockio::Reader& reader, const VolumeRuntimeConfig runtime,
                             VolumeInfo info, const TreeBlockIndexes tree_blocks)
    : reader_(&reader), container_byte_offset_(runtime.container_byte_offset),
      block_size_(runtime.block_size), xid_limit_(runtime.xid_limit), info_(std::move(info)),
      root_tree_block_index_(tree_blocks.root_tree_block_index),
      volume_omap_block_index_(tree_blocks.volume_omap_block_index) {}

blockio::Result<VolumeContext> VolumeContext::Load(const blockio::Reader& reader,
                                                   const ContainerInfo& container,
                                                   const VolumeInfo& info,
                                                   const OmapResolver& container_omap) {
  if (info.omap_oid == 0U || info.root_tree_oid == 0U) {
    return MakeApfsError(
        blockio::ErrorCode::kCorruptData,
        "Volume superblock is missing the omap or filesystem-tree object identifiers.");
  }

  const VolumeRuntimeConfig runtime{
      .container_byte_offset = container.byte_offset,
      .block_size = container.block_size,
      .xid_limit = container.selected_checkpoint.xid,
  };

  PhysicalObjectReader object_reader(reader, runtime.container_byte_offset, runtime.block_size);
  auto volume_omap_block_result =
      ResolveVolumeOmapBlock(object_reader, container_omap, runtime, info);
  if (!volume_omap_block_result.ok()) {
    return MakeApfsError(volume_omap_block_result.error().code,
                         "Failed to resolve the APFS volume omap object: " +
                             volume_omap_block_result.error().message);
  }

  auto volume_omap_result = OmapResolver::Load(object_reader, volume_omap_block_result.value());
  if (!volume_omap_result.ok()) {
    return MakeApfsError(volume_omap_result.error().code, "Failed to load the APFS volume omap: " +
                                                              volume_omap_result.error().message);
  }

  auto root_tree_block_result =
      ResolveRootTreeBlock(object_reader, volume_omap_result.value(), runtime, info);
  if (!root_tree_block_result.ok()) {
    return MakeApfsError(root_tree_block_result.error().code,
                         "Failed to resolve the APFS filesystem tree root object: " +
                             root_tree_block_result.error().message);
  }

  return VolumeContext(reader, runtime, info,
                       TreeBlockIndexes{
                           .root_tree_block_index = root_tree_block_result.value(),
                           .volume_omap_block_index = volume_omap_block_result.value(),
                       });
}

PhysicalObjectReader VolumeContext::MakeObjectReader() const {
  return PhysicalObjectReader(*reader_, container_byte_offset_, block_size_);
}

blockio::Result<std::size_t>
VolumeContext::VisitFilesystemRange(const BtreeWalker::CompareFn& compare,
                                    const BtreeWalker::VisitFn& visitor) const {
  auto object_reader = MakeObjectReader();
  auto volume_omap_result = OmapResolver::Load(object_reader, volume_omap_block_index_);
  if (!volume_omap_result.ok()) {
    return MakeApfsError(volume_omap_result.error().code,
                         "Failed to load the APFS volume omap for filesystem traversal: " +
                             volume_omap_result.error().message);
  }

  const auto& volume_omap = volume_omap_result.value();
  BtreeWalker walker(
      object_reader,
      [this, &object_reader,
       &volume_omap](const std::uint64_t child_identifier) -> blockio::Result<std::uint64_t> {
        auto child_block_result = volume_omap.ResolveOidToBlock(child_identifier, xid_limit_);
        if (child_block_result.ok()) {
          return child_block_result.value();
        }

        if (child_block_result.error().code != blockio::ErrorCode::kNotFound) {
          return MakeApfsError(
              child_block_result.error().code,
              "Failed to resolve the APFS filesystem tree child object through the volume omap: " +
                  child_block_result.error().message);
        }

        auto direct_object_result = object_reader.ReadPhysicalObject(child_identifier);
        if (!direct_object_result.ok()) {
          return MakeApfsError(
              child_block_result.error().code,
              "Failed to resolve the APFS filesystem tree child object through the volume omap: " +
                  child_block_result.error().message);
        }

        auto direct_node_result = ParseNode(direct_object_result.value().view(), block_size_);
        if (!direct_node_result.ok()) {
          return MakeApfsError(
              child_block_result.error().code,
              "Failed to resolve the APFS filesystem tree child object through the volume omap: " +
                  child_block_result.error().message);
        }

        return child_identifier;
      });
  return walker.VisitRange(root_tree_block_index_, compare, visitor);
}

blockio::Result<std::optional<NodeRecordCopy>>
VolumeContext::LowerBoundFilesystemRecord(const BtreeWalker::CompareFn& compare) const {
  auto object_reader = MakeObjectReader();
  auto volume_omap_result = OmapResolver::Load(object_reader, volume_omap_block_index_);
  if (!volume_omap_result.ok()) {
    return MakeApfsError(volume_omap_result.error().code,
                         "Failed to load the APFS volume omap for filesystem traversal: " +
                             volume_omap_result.error().message);
  }

  const auto& volume_omap = volume_omap_result.value();
  BtreeWalker walker(
      object_reader,
      [this, &object_reader,
       &volume_omap](const std::uint64_t child_identifier) -> blockio::Result<std::uint64_t> {
        auto child_block_result = volume_omap.ResolveOidToBlock(child_identifier, xid_limit_);
        if (child_block_result.ok()) {
          return child_block_result.value();
        }

        if (child_block_result.error().code != blockio::ErrorCode::kNotFound) {
          return MakeApfsError(
              child_block_result.error().code,
              "Failed to resolve the APFS filesystem tree child object through the volume omap: " +
                  child_block_result.error().message);
        }

        auto direct_object_result = object_reader.ReadPhysicalObject(child_identifier);
        if (!direct_object_result.ok()) {
          return MakeApfsError(
              child_block_result.error().code,
              "Failed to resolve the APFS filesystem tree child object through the volume omap: " +
                  child_block_result.error().message);
        }

        auto direct_node_result = ParseNode(direct_object_result.value().view(), block_size_);
        if (!direct_node_result.ok()) {
          return MakeApfsError(
              child_block_result.error().code,
              "Failed to resolve the APFS filesystem tree child object through the volume omap: " +
                  child_block_result.error().message);
        }

        return child_identifier;
      });
  auto cursor_result = walker.LowerBound(root_tree_block_index_, compare);
  if (!cursor_result.ok()) {
    return cursor_result.error();
  }
  if (!cursor_result.value().has_value()) {
    return std::optional<NodeRecordCopy>{};
  }

  auto cursor = std::move(cursor_result.value()).value_or(BtreeWalker::Cursor{});
  auto record_copy_result = cursor.CurrentCopy();
  if (!record_copy_result.ok()) {
    return record_copy_result.error();
  }

  return std::optional<NodeRecordCopy>(std::move(record_copy_result.value()));
}

blockio::Result<InodeRecord> VolumeContext::GetInode(const std::uint64_t inode_id) const {
  auto record_result = LowerBoundFilesystemRecord(MakeInodeLowerBoundCompare(inode_id));
  if (!record_result.ok()) {
    return record_result.error();
  }
  if (!record_result.value().has_value()) {
    return MakeApfsError(blockio::ErrorCode::kNotFound,
                         "Filesystem tree did not contain the requested inode.");
  }

  auto record_copy = std::move(record_result.value()).value_or(NodeRecordCopy{});
  auto matches_result = IsInodeKeyFor(
      std::span<const std::uint8_t>(record_copy.key.data(), record_copy.key.size()), inode_id);
  if (!matches_result.ok()) {
    return matches_result.error();
  }
  if (!matches_result.value()) {
    return MakeApfsError(blockio::ErrorCode::kNotFound,
                         "Filesystem tree did not contain the requested inode.");
  }

  return ParseInodeRecord(FsTreeRecordView{
      .key = std::span<const std::uint8_t>(record_copy.key.data(), record_copy.key.size()),
      .value = std::span<const std::uint8_t>(record_copy.value.data(), record_copy.value.size()),
  });
}

blockio::Result<std::vector<DirectoryEntryRecord>>
VolumeContext::ListDirectoryEntries(const std::uint64_t directory_inode_id) const {
  std::vector<DirectoryEntryRecord> entries;
  auto visit_result = VisitFilesystemRange(
      MakeDirectoryRecordLowerBoundCompare(directory_inode_id),
      [&entries, directory_inode_id](const NodeRecordView& record) -> blockio::Result<bool> {
        auto matches_result = IsDirectoryRecordKeyFor(record.key, directory_inode_id);
        if (!matches_result.ok()) {
          return matches_result.error();
        }
        if (!matches_result.value()) {
          return false;
        }

        auto entry_result = ParseDirectoryEntryRecord(FsTreeRecordView{
            .key = record.key,
            .value = record.value,
        });
        if (!entry_result.ok()) {
          return entry_result.error();
        }
        entries.push_back(entry_result.value());
        return true;
      });
  if (!visit_result.ok()) {
    return visit_result.error();
  }

  for (auto& entry : entries) {
    auto inode_result = GetInode(entry.file_id);
    if (inode_result.ok()) {
      entry.kind = inode_result.value().kind;
    }
  }

  std::sort(entries.begin(), entries.end(),
            [this](const DirectoryEntryRecord& left, const DirectoryEntryRecord& right) {
              const auto left_name =
                  info_.case_insensitive ? AsciiFold(left.key.name) : left.key.name;
              const auto right_name =
                  info_.case_insensitive ? AsciiFold(right.key.name) : right.key.name;
              if (left_name == right_name) {
                return left.file_id < right.file_id;
              }
              return left_name < right_name;
            });
  return entries;
}

blockio::Result<std::vector<FileExtentRecord>>
VolumeContext::ListFileExtents(const std::uint64_t inode_id) const {
  std::vector<FileExtentRecord> extents;
  auto visit_result = VisitFilesystemRange(
      MakeFileExtentLowerBoundCompare(inode_id),
      [&extents, inode_id](const NodeRecordView& record) -> blockio::Result<bool> {
        auto matches_result = IsFileExtentKeyFor(record.key, inode_id);
        if (!matches_result.ok()) {
          return matches_result.error();
        }
        if (!matches_result.value()) {
          return false;
        }

        auto extent_result = ParseFileExtentRecord(FsTreeRecordView{
            .key = record.key,
            .value = record.value,
        });
        if (!extent_result.ok()) {
          return extent_result.error();
        }
        extents.push_back(extent_result.value());
        return true;
      });
  if (!visit_result.ok()) {
    return visit_result.error();
  }

  std::sort(extents.begin(), extents.end(),
            [](const FileExtentRecord& left, const FileExtentRecord& right) {
              if (left.key.logical_address == right.key.logical_address) {
                return left.physical_block < right.physical_block;
              }
              return left.key.logical_address < right.key.logical_address;
            });
  return extents;
}

blockio::Result<std::optional<XattrRecord>>
VolumeContext::FindXattr(const std::uint64_t inode_id, const std::string_view name) const {
  std::optional<XattrRecord> found;
  auto visit_result = VisitFilesystemRange(
      MakeXattrLowerBoundCompare(inode_id),
      [&found, inode_id, name](const NodeRecordView& record) -> blockio::Result<bool> {
        auto matches_result = IsXattrKeyFor(record.key, inode_id);
        if (!matches_result.ok()) {
          return matches_result.error();
        }
        if (!matches_result.value()) {
          return false;
        }

        auto xattr_result = ParseXattrRecord(FsTreeRecordView{
            .key = record.key,
            .value = record.value,
        });
        if (!xattr_result.ok()) {
          return xattr_result.error();
        }
        if (xattr_result.value().key.name != name) {
          return true;
        }

        found = xattr_result.value();
        return false;
      });
  if (!visit_result.ok()) {
    return visit_result.error();
  }

  return found;
}

blockio::Result<std::vector<std::uint8_t>>
VolumeContext::ReadPhysicalBytes(const PhysicalReadRequest& request) const {
  auto absolute_offset_result = ComputeAbsoluteByteOffset(
      VolumeRuntimeConfig{
          .container_byte_offset = container_byte_offset_,
          .block_size = block_size_,
          .xid_limit = xid_limit_,
      },
      request);
  if (!absolute_offset_result.ok()) {
    return absolute_offset_result.error();
  }

  return blockio::ReadExact(*reader_, blockio::ReadRequest{.offset = absolute_offset_result.value(),
                                                           .size = request.size});
}

} // namespace orchard::apfs
