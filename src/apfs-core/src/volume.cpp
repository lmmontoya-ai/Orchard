#include "orchard/apfs/volume.h"

#include <algorithm>
#include <limits>

namespace orchard::apfs {
namespace {

blockio::Result<std::uint64_t> ComputeAbsoluteByteOffset(const std::uint64_t container_byte_offset,
                                                         const std::uint32_t block_size,
                                                         const std::uint64_t block_index,
                                                         const std::uint64_t block_offset) {
  constexpr auto kMaxUint64 = std::numeric_limits<std::uint64_t>::max();
  if (block_index > (kMaxUint64 - container_byte_offset) / block_size) {
    return MakeApfsError(blockio::ErrorCode::kOutOfRange,
                         "Physical read would overflow the APFS container byte range.");
  }

  const auto block_base = container_byte_offset + (block_index * block_size);
  if (block_offset > kMaxUint64 - block_base) {
    return MakeApfsError(
        blockio::ErrorCode::kOutOfRange,
        "Physical read block offset would overflow the APFS container byte range.");
  }

  return block_base + block_offset;
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

} // namespace

VolumeContext::VolumeContext(const blockio::Reader& reader,
                             const std::uint64_t container_byte_offset,
                             const std::uint32_t block_size, const std::uint64_t xid_limit,
                             VolumeInfo info, const std::uint64_t root_tree_block_index)
    : reader_(&reader), container_byte_offset_(container_byte_offset), block_size_(block_size),
      xid_limit_(xid_limit), info_(std::move(info)), root_tree_block_index_(root_tree_block_index) {
}

blockio::Result<VolumeContext>
VolumeContext::Load(const blockio::Reader& reader, const std::uint64_t container_byte_offset,
                    const std::uint32_t block_size, const std::uint64_t xid_limit,
                    const VolumeInfo& info, const OmapResolver& container_omap) {
  if (info.omap_oid == 0U || info.root_tree_oid == 0U) {
    return MakeApfsError(
        blockio::ErrorCode::kCorruptData,
        "Volume superblock is missing the omap or filesystem-tree object identifiers.");
  }

  PhysicalObjectReader object_reader(reader, container_byte_offset, block_size);
  auto volume_omap_block_result = container_omap.ResolveOidToBlock(info.omap_oid, xid_limit);
  if (!volume_omap_block_result.ok()) {
    return volume_omap_block_result.error();
  }

  auto volume_omap_result = OmapResolver::Load(object_reader, volume_omap_block_result.value());
  if (!volume_omap_result.ok()) {
    return volume_omap_result.error();
  }

  auto root_tree_block_result =
      volume_omap_result.value().ResolveOidToBlock(info.root_tree_oid, xid_limit);
  if (!root_tree_block_result.ok()) {
    return root_tree_block_result.error();
  }

  return VolumeContext(reader, container_byte_offset, block_size, xid_limit, info,
                       root_tree_block_result.value());
}

PhysicalObjectReader VolumeContext::MakeObjectReader() const {
  return PhysicalObjectReader(*reader_, container_byte_offset_, block_size_);
}

blockio::Result<std::size_t>
VolumeContext::VisitFilesystemRecords(const BtreeWalker::VisitFn& visitor) const {
  auto object_reader = MakeObjectReader();
  BtreeWalker walker(object_reader);
  return walker.VisitInOrder(root_tree_block_index_, visitor);
}

blockio::Result<InodeRecord> VolumeContext::GetInode(const std::uint64_t inode_id) const {
  std::optional<InodeRecord> found;
  auto visit_result = VisitFilesystemRecords(
      [&found, inode_id](const NodeRecordView& record) -> blockio::Result<bool> {
        auto key_result = ParseFsKeyHeader(record.key);
        if (!key_result.ok()) {
          return key_result.error();
        }
        if (key_result.value().type != FsRecordType::kInode ||
            key_result.value().object_id != inode_id) {
          return true;
        }

        auto inode_result = ParseInodeRecord(record.key, record.value);
        if (!inode_result.ok()) {
          return inode_result.error();
        }
        found = inode_result.value();
        return false;
      });
  if (!visit_result.ok()) {
    return visit_result.error();
  }
  if (!found.has_value()) {
    return MakeApfsError(blockio::ErrorCode::kNotFound,
                         "Filesystem tree did not contain the requested inode.");
  }

  return *found;
}

blockio::Result<std::vector<DirectoryEntryRecord>>
VolumeContext::ListDirectoryEntries(const std::uint64_t directory_inode_id) const {
  std::vector<DirectoryEntryRecord> entries;
  auto visit_result = VisitFilesystemRecords(
      [&entries, directory_inode_id](const NodeRecordView& record) -> blockio::Result<bool> {
        auto key_result = ParseFsKeyHeader(record.key);
        if (!key_result.ok()) {
          return key_result.error();
        }
        if (key_result.value().type != FsRecordType::kDirRecord ||
            key_result.value().object_id != directory_inode_id) {
          return true;
        }

        auto entry_result = ParseDirectoryEntryRecord(record.key, record.value);
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
  auto visit_result = VisitFilesystemRecords(
      [&extents, inode_id](const NodeRecordView& record) -> blockio::Result<bool> {
        auto key_result = ParseFsKeyHeader(record.key);
        if (!key_result.ok()) {
          return key_result.error();
        }
        if (key_result.value().type != FsRecordType::kFileExtent ||
            key_result.value().object_id != inode_id) {
          return true;
        }

        auto extent_result = ParseFileExtentRecord(record.key, record.value);
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
  auto visit_result = VisitFilesystemRecords(
      [&found, inode_id, name](const NodeRecordView& record) -> blockio::Result<bool> {
        auto key_result = ParseFsKeyHeader(record.key);
        if (!key_result.ok()) {
          return key_result.error();
        }
        if (key_result.value().type != FsRecordType::kXattr ||
            key_result.value().object_id != inode_id) {
          return true;
        }

        auto xattr_result = ParseXattrRecord(record.key, record.value);
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
VolumeContext::ReadPhysicalBytes(const std::uint64_t physical_block_index,
                                 const std::uint64_t block_offset, const std::size_t size) const {
  auto absolute_offset_result = ComputeAbsoluteByteOffset(container_byte_offset_, block_size_,
                                                          physical_block_index, block_offset);
  if (!absolute_offset_result.ok()) {
    return absolute_offset_result.error();
  }

  return blockio::ReadExact(
      *reader_, blockio::ReadRequest{.offset = absolute_offset_result.value(), .size = size});
}

} // namespace orchard::apfs
