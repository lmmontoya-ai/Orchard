#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "orchard/apfs/btree.h"
#include "orchard/apfs/discovery.h"
#include "orchard/apfs/file_read.h"
#include "orchard/apfs/fs_keys.h"
#include "orchard/apfs/inspection.h"
#include "orchard/apfs/link_read.h"
#include "orchard/apfs/object.h"
#include "orchard/apfs/omap.h"
#include "orchard/apfs/path_lookup.h"
#include "orchard/apfs/probe.h"
#include "orchard/apfs/volume.h"
#include "orchard/blockio/inspection_target.h"
#include "orchard/blockio/reader.h"
#include "orchard/fs_winfsp/directory_query.h"
#include "orchard/fs_winfsp/file_info.h"
#include "orchard/fs_winfsp/mount.h"
#include "orchard/fs_winfsp/path_bridge.h"
#include "orchard/fs_winfsp/reparse.h"
#include "orchard_test/test.h"

namespace {

constexpr std::uint32_t kApfsBlockSize = 4096U;
constexpr std::uint64_t kBlockCount = 15U;

constexpr std::uint64_t kContainerOmapObjectBlock = 2U;
constexpr std::uint64_t kContainerOmapRootBlock = 3U;
constexpr std::uint64_t kContainerOmapLeafBlock = 4U;
constexpr std::uint64_t kLegacyVolumeBlock = 5U;
constexpr std::uint64_t kCurrentVolumeBlock = 6U;
constexpr std::uint64_t kVolumeOmapBlock = 7U;
constexpr std::uint64_t kVolumeOmapRootBlock = 8U;
constexpr std::uint64_t kFsTreeBlock = 9U;
constexpr std::uint64_t kAlphaDataBlock1 = 10U;
constexpr std::uint64_t kAlphaDataBlock2 = 11U;
constexpr std::uint64_t kNoteDataBlock = 12U;
constexpr std::uint64_t kSparseDataBlock1 = 13U;
constexpr std::uint64_t kSparseDataBlock2 = 14U;

constexpr std::uint64_t kVolumeObjectId = 77U;
constexpr std::uint64_t kVolumeOmapObjectId = 88U;
constexpr std::uint64_t kFsTreeObjectId = 200U;
constexpr std::uint64_t kCurrentCheckpointXid = 42U;
constexpr std::uint64_t kDefaultTimestampUnixNanos = 1704067200000000000ULL;

constexpr std::uint64_t kRootInodeId = orchard::apfs::kApfsRootDirectoryObjectId;
constexpr std::uint64_t kAlphaInodeId = 20U;
constexpr std::uint64_t kDocsInodeId = 30U;
constexpr std::uint64_t kNoteInodeId = 31U;
constexpr std::uint64_t kSparseInodeId = 40U;
constexpr std::uint64_t kCompressedInodeId = 50U;
constexpr std::uint64_t kEmptyInodeId = 60U;

constexpr std::string_view kAlphaExtent1 = "Hello ";
constexpr std::string_view kAlphaExtent2 = "Orchard\n";
constexpr std::string_view kNoteText = "Nested note\n";
constexpr std::string_view kSparseExtent1 = "ABCD";
constexpr std::string_view kSparseExtent2 = "WXYZ";
constexpr std::string_view kCompressedText = "Compressed orchard\n";

constexpr std::uint64_t kSparseInternalFlag = 0x00000200ULL;

struct OmapLeafRecord {
  std::uint64_t oid = 0;
  std::uint64_t xid = 0;
  std::uint32_t flags = 0;
  std::uint64_t physical_block = 0;
};

struct VariableRecord {
  std::vector<std::uint8_t> key;
  std::vector<std::uint8_t> value;
};

struct FixtureOptions {
  std::string volume_name = "Orchard Data";
  std::uint64_t incompatible_features = orchard::apfs::kVolumeIncompatCaseInsensitive;
  std::uint16_t role = orchard::apfs::kVolumeRoleData;
  bool delete_latest_volume_mapping = false;
};

struct LoadedVolumeContext {
  orchard::blockio::ReaderHandle reader;
  orchard::apfs::VolumeContext volume;
};

std::filesystem::path SampleFixturePath(const std::string_view filename) {
  return std::filesystem::path(ORCHARD_SOURCE_DIR) / "tests" / "corpus" / "samples" /
         std::string(filename);
}

void WriteLe16(std::vector<std::uint8_t>& bytes, const std::size_t offset,
               const std::uint16_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value & 0xFFU);
  bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

void WriteLe32(std::vector<std::uint8_t>& bytes, const std::size_t offset,
               const std::uint32_t value) {
  WriteLe16(bytes, offset, static_cast<std::uint16_t>(value & 0xFFFFU));
  WriteLe16(bytes, offset + 2U, static_cast<std::uint16_t>((value >> 16U) & 0xFFFFU));
}

void WriteLe64(std::vector<std::uint8_t>& bytes, const std::size_t offset,
               const std::uint64_t value) {
  WriteLe32(bytes, offset, static_cast<std::uint32_t>(value & 0xFFFFFFFFULL));
  WriteLe32(bytes, offset + 4U, static_cast<std::uint32_t>((value >> 32U) & 0xFFFFFFFFULL));
}

void WriteAscii(std::vector<std::uint8_t>& bytes, const std::size_t offset,
                const std::string_view text) {
  for (std::size_t index = 0; index < text.size(); ++index) {
    bytes[offset + index] = static_cast<std::uint8_t>(text[index]);
  }
}

void WriteUtf16Le(std::vector<std::uint8_t>& bytes, const std::size_t offset,
                  const std::string_view text) {
  for (std::size_t index = 0; index < text.size(); ++index) {
    WriteLe16(bytes, offset + (index * 2U), static_cast<std::uint16_t>(text[index]));
  }
}

void WriteRawUuid(std::vector<std::uint8_t>& bytes, const std::size_t offset,
                  const std::array<std::uint8_t, 16>& uuid) {
  std::copy(uuid.begin(), uuid.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void WriteObjectHeader(std::vector<std::uint8_t>& bytes, const std::size_t base_offset,
                       const std::uint64_t oid, const std::uint64_t xid, const std::uint32_t type,
                       const std::uint32_t subtype) {
  WriteLe64(bytes, base_offset + 0x00U, 0U);
  WriteLe64(bytes, base_offset + 0x08U, oid);
  WriteLe64(bytes, base_offset + 0x10U, xid);
  WriteLe32(bytes, base_offset + 0x18U, type);
  WriteLe32(bytes, base_offset + 0x1CU, subtype);
}

void WriteNxSuperblock(std::vector<std::uint8_t>& bytes, const std::size_t base_offset,
                       const std::uint64_t xid) {
  WriteObjectHeader(bytes, base_offset, base_offset / kApfsBlockSize, xid,
                    orchard::apfs::kObjectTypeNxSuperblock, 0U);
  WriteAscii(bytes, base_offset + 0x20U, "NXSB");
  WriteLe32(bytes, base_offset + 0x24U, kApfsBlockSize);
  WriteLe64(bytes, base_offset + 0x28U, kBlockCount);
  WriteRawUuid(bytes, base_offset + 0x48U,
               std::array<std::uint8_t, 16>{0x10, 0x11, 0x12, 0x13, 0x20, 0x21, 0x22, 0x23, 0x30,
                                            0x31, 0x32, 0x33, 0x40, 0x41, 0x42, 0x43});
  WriteLe64(bytes, base_offset + 0x60U, xid + 1U);
  WriteLe32(bytes, base_offset + 0x68U, 1U);
  WriteLe64(bytes, base_offset + 0x70U, 1U);
  WriteLe64(bytes, base_offset + 0x98U, 5U);
  WriteLe64(bytes, base_offset + 0xA0U, kContainerOmapObjectBlock);
  WriteLe64(bytes, base_offset + 0xA8U, 7U);
  WriteLe32(bytes, base_offset + 0xB4U, 100U);
  WriteLe64(bytes, base_offset + 0xB8U, kVolumeObjectId);
}

void WriteOmapSuperblock(std::vector<std::uint8_t>& bytes, const std::size_t base_offset,
                         const std::uint64_t oid, const std::uint64_t xid,
                         const std::uint64_t tree_oid) {
  WriteObjectHeader(bytes, base_offset, oid, xid, orchard::apfs::kObjectTypeOmap,
                    orchard::apfs::kObjectTypeOmap);
  WriteLe32(bytes, base_offset + 0x20U, 0U);
  WriteLe32(bytes, base_offset + 0x24U, 0U);
  WriteLe32(bytes, base_offset + 0x28U, orchard::apfs::kObjectTypeOmap);
  WriteLe32(bytes, base_offset + 0x2CU, 0U);
  WriteLe64(bytes, base_offset + 0x30U, tree_oid);
}

void WriteBtreeInfoFooter(std::vector<std::uint8_t>& bytes, const std::size_t base_offset,
                          const std::uint32_t key_size, const std::uint32_t value_size,
                          const std::uint64_t key_count, const std::uint64_t node_count) {
  const auto footer_offset = base_offset + kApfsBlockSize - orchard::apfs::kBtreeInfoSize;
  WriteLe32(bytes, footer_offset + 0x00U, 0U);
  WriteLe32(bytes, footer_offset + 0x04U, kApfsBlockSize);
  WriteLe32(bytes, footer_offset + 0x08U, key_size);
  WriteLe32(bytes, footer_offset + 0x0CU, value_size);
  WriteLe32(bytes, footer_offset + 0x10U, key_size);
  WriteLe32(bytes, footer_offset + 0x14U, value_size);
  WriteLe64(bytes, footer_offset + 0x18U, key_count);
  WriteLe64(bytes, footer_offset + 0x20U, node_count);
}

void WriteOmapKey(std::vector<std::uint8_t>& bytes, const std::size_t offset,
                  const std::uint64_t oid, const std::uint64_t xid) {
  WriteLe64(bytes, offset + 0x00U, oid);
  WriteLe64(bytes, offset + 0x08U, xid);
}

void WriteOmapValue(std::vector<std::uint8_t>& bytes, const std::size_t offset,
                    const std::uint32_t flags, const std::uint64_t physical_block) {
  WriteLe32(bytes, offset + 0x00U, flags);
  WriteLe32(bytes, offset + 0x04U, kApfsBlockSize);
  WriteLe64(bytes, offset + 0x08U, physical_block);
}

void WriteOmapRootNode(std::vector<std::uint8_t>& bytes, const std::size_t base_offset,
                       const std::uint64_t oid, const std::uint64_t xid,
                       const std::uint64_t min_key_oid, const std::uint64_t min_key_xid,
                       const std::uint64_t child_block) {
  const auto table_length = 4U;
  const auto key_area_start = base_offset + orchard::apfs::kBtreeNodeHeaderSize + table_length;
  const auto value_area_end = base_offset + kApfsBlockSize - orchard::apfs::kBtreeInfoSize;
  const auto value_offset = value_area_end - sizeof(std::uint64_t);

  WriteObjectHeader(bytes, base_offset, oid, xid, orchard::apfs::kObjectTypeBtreeNode,
                    orchard::apfs::kObjectTypeOmap);
  WriteLe16(bytes, base_offset + 0x20U,
            orchard::apfs::kBtreeNodeFlagRoot | orchard::apfs::kBtreeNodeFlagFixedKv);
  WriteLe16(bytes, base_offset + 0x22U, 1U);
  WriteLe32(bytes, base_offset + 0x24U, 1U);
  WriteLe16(bytes, base_offset + 0x28U, 0U);
  WriteLe16(bytes, base_offset + 0x2AU, table_length);
  WriteLe16(bytes, base_offset + 0x2CU, 0U);
  WriteLe16(bytes, base_offset + 0x2EU, 0U);
  WriteLe16(bytes, base_offset + 0x30U, orchard::apfs::kBtreeOffsetInvalid);
  WriteLe16(bytes, base_offset + 0x32U, 0U);
  WriteLe16(bytes, base_offset + 0x34U, orchard::apfs::kBtreeOffsetInvalid);
  WriteLe16(bytes, base_offset + 0x36U, 0U);

  WriteLe16(bytes, base_offset + orchard::apfs::kBtreeNodeHeaderSize + 0x00U, 0U);
  WriteLe16(bytes, base_offset + orchard::apfs::kBtreeNodeHeaderSize + 0x02U,
            static_cast<std::uint16_t>(
                (base_offset + kApfsBlockSize - orchard::apfs::kBtreeInfoSize) - value_offset));

  WriteOmapKey(bytes, key_area_start, min_key_oid, min_key_xid);
  WriteLe64(bytes, value_offset, child_block);
  WriteBtreeInfoFooter(bytes, base_offset, 16U, 8U, 3U, 2U);
}

void WriteOmapLeafNode(std::vector<std::uint8_t>& bytes, const std::size_t base_offset,
                       const std::uint64_t oid, const std::uint64_t xid,
                       const std::vector<OmapLeafRecord>& records, const std::uint16_t flags) {
  const auto table_length = static_cast<std::uint16_t>(records.size() * 4U);
  const auto key_area_start = base_offset + orchard::apfs::kBtreeNodeHeaderSize + table_length;
  const auto value_area_end =
      base_offset + kApfsBlockSize -
      ((flags & orchard::apfs::kBtreeNodeFlagRoot) != 0U ? orchard::apfs::kBtreeInfoSize : 0U);

  WriteObjectHeader(bytes, base_offset, oid, xid, orchard::apfs::kObjectTypeBtreeNode,
                    orchard::apfs::kObjectTypeOmap);
  WriteLe16(bytes, base_offset + 0x20U,
            static_cast<std::uint16_t>(flags | orchard::apfs::kBtreeNodeFlagLeaf |
                                       orchard::apfs::kBtreeNodeFlagFixedKv));
  WriteLe16(bytes, base_offset + 0x22U, 0U);
  WriteLe32(bytes, base_offset + 0x24U, static_cast<std::uint32_t>(records.size()));
  WriteLe16(bytes, base_offset + 0x28U, 0U);
  WriteLe16(bytes, base_offset + 0x2AU, table_length);
  WriteLe16(bytes, base_offset + 0x2CU, 0U);
  WriteLe16(bytes, base_offset + 0x2EU, 0U);
  WriteLe16(bytes, base_offset + 0x30U, orchard::apfs::kBtreeOffsetInvalid);
  WriteLe16(bytes, base_offset + 0x32U, 0U);
  WriteLe16(bytes, base_offset + 0x34U, orchard::apfs::kBtreeOffsetInvalid);
  WriteLe16(bytes, base_offset + 0x36U, 0U);

  for (std::size_t index = 0; index < records.size(); ++index) {
    const auto toc_offset = base_offset + orchard::apfs::kBtreeNodeHeaderSize + (index * 4U);
    const auto key_offset = static_cast<std::uint16_t>(index * 16U);
    const auto value_offset = static_cast<std::uint16_t>((index + 1U) * 16U);
    const auto key_write_offset = key_area_start + key_offset;
    const auto value_write_offset = value_area_end - value_offset;

    WriteLe16(bytes, toc_offset + 0x00U, key_offset);
    WriteLe16(bytes, toc_offset + 0x02U, value_offset);
    WriteOmapKey(bytes, key_write_offset, records[index].oid, records[index].xid);
    WriteOmapValue(bytes, value_write_offset, records[index].flags, records[index].physical_block);
  }

  if ((flags & orchard::apfs::kBtreeNodeFlagRoot) != 0U) {
    WriteBtreeInfoFooter(bytes, base_offset, 16U, 16U, records.size(), 1U);
  }
}

std::uint64_t MakeFsKeyHeaderValue(const std::uint64_t object_id,
                                   const orchard::apfs::FsRecordType type) {
  return object_id | (static_cast<std::uint64_t>(type) << 60U);
}

std::vector<std::uint8_t> MakeInodeKey(const std::uint64_t object_id) {
  std::vector<std::uint8_t> bytes(sizeof(std::uint64_t), 0U);
  WriteLe64(bytes, 0U, MakeFsKeyHeaderValue(object_id, orchard::apfs::FsRecordType::kInode));
  return bytes;
}

std::vector<std::uint8_t> MakeNamedKey(const std::uint64_t object_id,
                                       const orchard::apfs::FsRecordType type,
                                       const std::string_view name) {
  std::vector<std::uint8_t> bytes(10U + name.size(), 0U);
  WriteLe64(bytes, 0U, MakeFsKeyHeaderValue(object_id, type));
  WriteLe16(bytes, 8U, static_cast<std::uint16_t>(name.size()));
  WriteAscii(bytes, 10U, name);
  return bytes;
}

std::vector<std::uint8_t> MakeFileExtentKey(const std::uint64_t object_id,
                                            const std::uint64_t logical_address) {
  std::vector<std::uint8_t> bytes(16U, 0U);
  WriteLe64(bytes, 0U, MakeFsKeyHeaderValue(object_id, orchard::apfs::FsRecordType::kFileExtent));
  WriteLe64(bytes, 8U, logical_address);
  return bytes;
}

std::vector<std::uint8_t>
MakeInodeValue(const std::uint64_t parent_id, const std::uint64_t logical_size,
               const std::uint64_t allocated_size, const std::uint64_t internal_flags,
               const std::uint32_t child_count, const std::uint16_t mode,
               const std::uint32_t link_count = 1U,
               const std::uint64_t creation_time_unix_nanos = kDefaultTimestampUnixNanos,
               const std::optional<std::uint64_t> last_access_time_unix_nanos = std::nullopt,
               const std::optional<std::uint64_t> last_write_time_unix_nanos = std::nullopt,
               const std::optional<std::uint64_t> change_time_unix_nanos = std::nullopt) {
  std::vector<std::uint8_t> bytes(0x60U, 0U);
  const auto access_time = last_access_time_unix_nanos.value_or(creation_time_unix_nanos);
  const auto write_time = last_write_time_unix_nanos.value_or(creation_time_unix_nanos);
  const auto change_time = change_time_unix_nanos.value_or(write_time);
  WriteLe64(bytes, 0x00U, parent_id);
  WriteLe64(bytes, 0x10U, allocated_size);
  WriteLe64(bytes, 0x18U, internal_flags);
  WriteLe32(bytes, 0x20U, child_count);
  WriteLe16(bytes, 0x24U, mode);
  WriteLe64(bytes, 0x28U, creation_time_unix_nanos);
  WriteLe64(bytes, 0x30U, access_time);
  WriteLe64(bytes, 0x38U, write_time);
  WriteLe64(bytes, 0x40U, change_time);
  WriteLe32(bytes, 0x48U, link_count);
  WriteLe64(bytes, 0x58U, logical_size);
  return bytes;
}

std::vector<std::uint8_t> MakeDirectoryValue(const std::uint64_t file_id) {
  std::vector<std::uint8_t> bytes(10U, 0U);
  WriteLe64(bytes, 0x00U, file_id);
  WriteLe16(bytes, 0x08U, 0U);
  return bytes;
}

std::vector<std::uint8_t> MakeFileExtentValue(const std::uint64_t length,
                                              const std::uint64_t physical_block) {
  std::vector<std::uint8_t> bytes(24U, 0U);
  WriteLe64(bytes, 0x00U, length);
  WriteLe64(bytes, 0x08U, physical_block);
  WriteLe64(bytes, 0x10U, 0U);
  return bytes;
}

std::vector<std::uint8_t> MakeCompressionPayload(const std::string_view text) {
  std::vector<std::uint8_t> bytes(16U + text.size(), 0U);
  WriteAscii(bytes, 0x00U, "cmpf");
  WriteLe32(bytes, 0x04U, 9U);
  WriteLe64(bytes, 0x08U, text.size());
  WriteAscii(bytes, 0x10U, text);
  return bytes;
}

std::vector<std::uint8_t> MakeXattrValue(const std::vector<std::uint8_t>& data) {
  std::vector<std::uint8_t> bytes(8U + data.size(), 0U);
  WriteLe16(bytes, 0x00U, 0U);
  WriteLe32(bytes, 0x04U, static_cast<std::uint32_t>(data.size()));
  std::copy(data.begin(), data.end(), bytes.begin() + 8);
  return bytes;
}

void WriteVariableBtreeRootLeafNode(std::vector<std::uint8_t>& bytes, const std::size_t base_offset,
                                    const std::uint64_t oid, const std::uint64_t xid,
                                    const std::uint32_t subtype,
                                    const std::vector<VariableRecord>& records) {
  const auto table_length = static_cast<std::uint16_t>(records.size() * 8U);
  const auto key_area_start = base_offset + orchard::apfs::kBtreeNodeHeaderSize + table_length;
  const auto value_area_end = base_offset + kApfsBlockSize - orchard::apfs::kBtreeInfoSize;

  WriteObjectHeader(bytes, base_offset, oid, xid, orchard::apfs::kObjectTypeBtreeNode, subtype);
  WriteLe16(bytes, base_offset + 0x20U,
            orchard::apfs::kBtreeNodeFlagRoot | orchard::apfs::kBtreeNodeFlagLeaf);
  WriteLe16(bytes, base_offset + 0x22U, 0U);
  WriteLe32(bytes, base_offset + 0x24U, static_cast<std::uint32_t>(records.size()));
  WriteLe16(bytes, base_offset + 0x28U, 0U);
  WriteLe16(bytes, base_offset + 0x2AU, table_length);
  WriteLe16(bytes, base_offset + 0x2CU, 0U);
  WriteLe16(bytes, base_offset + 0x2EU, 0U);
  WriteLe16(bytes, base_offset + 0x30U, orchard::apfs::kBtreeOffsetInvalid);
  WriteLe16(bytes, base_offset + 0x32U, 0U);
  WriteLe16(bytes, base_offset + 0x34U, orchard::apfs::kBtreeOffsetInvalid);
  WriteLe16(bytes, base_offset + 0x36U, 0U);

  std::uint16_t next_key_offset = 0U;
  std::uint16_t next_value_offset = 0U;
  std::uint32_t longest_key = 0U;
  std::uint32_t longest_value = 0U;

  for (std::size_t index = 0; index < records.size(); ++index) {
    const auto toc_offset = base_offset + orchard::apfs::kBtreeNodeHeaderSize + (index * 8U);
    const auto key_offset = next_key_offset;
    const auto key_length = static_cast<std::uint16_t>(records[index].key.size());
    next_key_offset = static_cast<std::uint16_t>(next_key_offset + key_length);
    const auto value_length = static_cast<std::uint16_t>(records[index].value.size());
    next_value_offset = static_cast<std::uint16_t>(next_value_offset + value_length);
    const auto value_offset = next_value_offset;
    const auto key_write_offset = key_area_start + key_offset;
    const auto value_write_offset = value_area_end - value_offset;

    WriteLe16(bytes, toc_offset + 0x00U, key_offset);
    WriteLe16(bytes, toc_offset + 0x02U, key_length);
    WriteLe16(bytes, toc_offset + 0x04U, value_offset);
    WriteLe16(bytes, toc_offset + 0x06U, value_length);

    std::copy(records[index].key.begin(), records[index].key.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(key_write_offset));
    std::copy(records[index].value.begin(), records[index].value.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(value_write_offset));

    longest_key = std::max(longest_key, static_cast<std::uint32_t>(records[index].key.size()));
    longest_value =
        std::max(longest_value, static_cast<std::uint32_t>(records[index].value.size()));
  }

  const auto footer_offset = base_offset + kApfsBlockSize - orchard::apfs::kBtreeInfoSize;
  WriteLe32(bytes, footer_offset + 0x00U, 0U);
  WriteLe32(bytes, footer_offset + 0x04U, kApfsBlockSize);
  WriteLe32(bytes, footer_offset + 0x08U, 0U);
  WriteLe32(bytes, footer_offset + 0x0CU, 0U);
  WriteLe32(bytes, footer_offset + 0x10U, longest_key);
  WriteLe32(bytes, footer_offset + 0x14U, longest_value);
  WriteLe64(bytes, footer_offset + 0x18U, records.size());
  WriteLe64(bytes, footer_offset + 0x20U, 1U);
}

void WriteVolumeSuperblock(std::vector<std::uint8_t>& bytes, const std::size_t base_offset,
                           const std::uint64_t xid, const std::uint64_t incompatible_features,
                           const std::uint16_t role, const std::string_view name) {
  WriteObjectHeader(bytes, base_offset, kVolumeObjectId, xid, orchard::apfs::kObjectTypeFs, 0U);
  WriteAscii(bytes, base_offset + 0x20U, "APSB");
  WriteLe64(bytes, base_offset + 0x38U, incompatible_features);
  WriteLe32(bytes, base_offset + 0x74U, orchard::apfs::kObjectTypeFs);
  WriteLe64(bytes, base_offset + 0x80U, kVolumeOmapObjectId);
  WriteLe64(bytes, base_offset + 0x88U, kFsTreeObjectId);
  WriteRawUuid(bytes, base_offset + 0xF0U,
               std::array<std::uint8_t, 16>{0x50, 0x51, 0x52, 0x53, 0x60, 0x61, 0x62, 0x63, 0x70,
                                            0x71, 0x72, 0x73, 0x80, 0x81, 0x82, 0x83});
  WriteAscii(bytes, base_offset + 0x2C0U, name);
  WriteLe16(bytes, base_offset + 0x3C4U, role);
}
// NOLINTEND(bugprone-easily-swappable-parameters)

void WriteRawDataBlock(std::vector<std::uint8_t>& bytes, const std::uint64_t block_index,
                       const std::string_view text) {
  const auto base_offset = static_cast<std::size_t>(block_index * kApfsBlockSize);
  WriteAscii(bytes, base_offset, text);
}

std::vector<VariableRecord> BuildFsRecords() {
  std::vector<VariableRecord> records;
  records.push_back(VariableRecord{
      .key = MakeInodeKey(kRootInodeId),
      .value = MakeInodeValue(kRootInodeId, 0U, 0U, 0U, 4U, 0x4000U),
  });
  records.push_back(VariableRecord{
      .key = MakeInodeKey(kAlphaInodeId),
      .value = MakeInodeValue(kRootInodeId, kAlphaExtent1.size() + kAlphaExtent2.size(),
                              kAlphaExtent1.size() + kAlphaExtent2.size(), 0U, 0U, 0x8000U),
  });
  records.push_back(VariableRecord{
      .key = MakeInodeKey(kDocsInodeId),
      .value = MakeInodeValue(kRootInodeId, 0U, 0U, 0U, 2U, 0x4000U),
  });
  records.push_back(VariableRecord{
      .key = MakeInodeKey(kNoteInodeId),
      .value = MakeInodeValue(kDocsInodeId, kNoteText.size(), kNoteText.size(), 0U, 0U, 0x8000U),
  });
  records.push_back(VariableRecord{
      .key = MakeInodeKey(kSparseInodeId),
      .value = MakeInodeValue(kRootInodeId, 12U, 8U, kSparseInternalFlag, 0U, 0x8000U),
  });
  records.push_back(VariableRecord{
      .key = MakeInodeKey(kCompressedInodeId),
      .value = MakeInodeValue(kRootInodeId, kCompressedText.size(), 0U, 0U, 0U, 0x8000U),
  });
  records.push_back(VariableRecord{
      .key = MakeInodeKey(kEmptyInodeId),
      .value = MakeInodeValue(kDocsInodeId, 0U, 0U, 0U, 0U, 0x8000U),
  });

  records.push_back(VariableRecord{
      .key = MakeNamedKey(kRootInodeId, orchard::apfs::FsRecordType::kDirRecord, "alpha.txt"),
      .value = MakeDirectoryValue(kAlphaInodeId),
  });
  records.push_back(VariableRecord{
      .key = MakeNamedKey(kRootInodeId, orchard::apfs::FsRecordType::kDirRecord, "compressed.txt"),
      .value = MakeDirectoryValue(kCompressedInodeId),
  });
  records.push_back(VariableRecord{
      .key = MakeNamedKey(kRootInodeId, orchard::apfs::FsRecordType::kDirRecord, "docs"),
      .value = MakeDirectoryValue(kDocsInodeId),
  });
  records.push_back(VariableRecord{
      .key = MakeNamedKey(kRootInodeId, orchard::apfs::FsRecordType::kDirRecord, "holes.bin"),
      .value = MakeDirectoryValue(kSparseInodeId),
  });
  records.push_back(VariableRecord{
      .key = MakeNamedKey(kDocsInodeId, orchard::apfs::FsRecordType::kDirRecord, "empty.txt"),
      .value = MakeDirectoryValue(kEmptyInodeId),
  });
  records.push_back(VariableRecord{
      .key = MakeNamedKey(kDocsInodeId, orchard::apfs::FsRecordType::kDirRecord, "note.txt"),
      .value = MakeDirectoryValue(kNoteInodeId),
  });

  records.push_back(VariableRecord{
      .key = MakeFileExtentKey(kAlphaInodeId, 0U),
      .value = MakeFileExtentValue(kAlphaExtent1.size(), kAlphaDataBlock1),
  });
  records.push_back(VariableRecord{
      .key = MakeFileExtentKey(kAlphaInodeId, kAlphaExtent1.size()),
      .value = MakeFileExtentValue(kAlphaExtent2.size(), kAlphaDataBlock2),
  });
  records.push_back(VariableRecord{
      .key = MakeFileExtentKey(kNoteInodeId, 0U),
      .value = MakeFileExtentValue(kNoteText.size(), kNoteDataBlock),
  });
  records.push_back(VariableRecord{
      .key = MakeFileExtentKey(kSparseInodeId, 0U),
      .value = MakeFileExtentValue(kSparseExtent1.size(), kSparseDataBlock1),
  });
  records.push_back(VariableRecord{
      .key = MakeFileExtentKey(kSparseInodeId, 8U),
      .value = MakeFileExtentValue(kSparseExtent2.size(), kSparseDataBlock2),
  });

  records.push_back(VariableRecord{
      .key = MakeNamedKey(kCompressedInodeId, orchard::apfs::FsRecordType::kXattr,
                          orchard::apfs::kCompressionXattrName),
      .value = MakeXattrValue(MakeCompressionPayload(kCompressedText)),
  });

  return records;
}

std::vector<std::uint8_t> MakeDirectFixture(const FixtureOptions& options = {}) {
  std::vector<std::uint8_t> bytes(
      static_cast<std::vector<std::uint8_t>::size_type>(kApfsBlockSize * kBlockCount), 0U);

  WriteNxSuperblock(bytes, 0U, 1U);
  WriteNxSuperblock(bytes, kApfsBlockSize, kCurrentCheckpointXid);
  WriteOmapSuperblock(bytes, kApfsBlockSize * kContainerOmapObjectBlock, kContainerOmapObjectBlock,
                      kCurrentCheckpointXid, kContainerOmapRootBlock);
  WriteOmapRootNode(bytes, kApfsBlockSize * kContainerOmapRootBlock, kContainerOmapRootBlock,
                    kCurrentCheckpointXid, kVolumeObjectId, 20U, kContainerOmapLeafBlock);
  WriteOmapLeafNode(
      bytes, kApfsBlockSize * kContainerOmapLeafBlock, kContainerOmapLeafBlock,
      kCurrentCheckpointXid,
      {
          OmapLeafRecord{
              .oid = kVolumeObjectId,
              .xid = 20U,
              .flags = 0U,
              .physical_block = kLegacyVolumeBlock,
          },
          OmapLeafRecord{
              .oid = kVolumeObjectId,
              .xid = kCurrentCheckpointXid,
              .flags = options.delete_latest_volume_mapping ? orchard::apfs::kOmapValueDeleted : 0U,
              .physical_block = kCurrentVolumeBlock,
          },
          OmapLeafRecord{
              .oid = kVolumeOmapObjectId,
              .xid = kCurrentCheckpointXid,
              .flags = 0U,
              .physical_block = kVolumeOmapBlock,
          },
      },
      0U);
  WriteVolumeSuperblock(bytes, kApfsBlockSize * kLegacyVolumeBlock, 20U,
                        orchard::apfs::kVolumeIncompatCaseInsensitive,
                        orchard::apfs::kVolumeRoleData, "Legacy Data");
  WriteVolumeSuperblock(bytes, kApfsBlockSize * kCurrentVolumeBlock, kCurrentCheckpointXid,
                        options.incompatible_features, options.role, options.volume_name);
  WriteOmapSuperblock(bytes, kApfsBlockSize * kVolumeOmapBlock, kVolumeOmapObjectId,
                      kCurrentCheckpointXid, kVolumeOmapRootBlock);
  WriteOmapLeafNode(bytes, kApfsBlockSize * kVolumeOmapRootBlock, kVolumeOmapRootBlock,
                    kCurrentCheckpointXid,
                    {
                        OmapLeafRecord{
                            .oid = kFsTreeObjectId,
                            .xid = kCurrentCheckpointXid,
                            .flags = 0U,
                            .physical_block = kFsTreeBlock,
                        },
                    },
                    orchard::apfs::kBtreeNodeFlagRoot);
  WriteVariableBtreeRootLeafNode(bytes, kApfsBlockSize * kFsTreeBlock, kFsTreeObjectId,
                                 kCurrentCheckpointXid, orchard::apfs::kObjectTypeFs,
                                 BuildFsRecords());
  WriteRawDataBlock(bytes, kAlphaDataBlock1, kAlphaExtent1);
  WriteRawDataBlock(bytes, kAlphaDataBlock2, kAlphaExtent2);
  WriteRawDataBlock(bytes, kNoteDataBlock, kNoteText);
  WriteRawDataBlock(bytes, kSparseDataBlock1, kSparseExtent1);
  WriteRawDataBlock(bytes, kSparseDataBlock2, kSparseExtent2);
  return bytes;
}

std::vector<std::uint8_t> MakeGptFixture() {
  constexpr std::uint32_t kLogicalBlockSize = 512U;
  constexpr std::uint64_t kFirstLba = 40U;
  constexpr std::uint64_t kLastLba = 159U;

  std::vector<std::uint8_t> bytes(
      static_cast<std::vector<std::uint8_t>::size_type>(kLogicalBlockSize * 256U), 0U);

  WriteAscii(bytes, kLogicalBlockSize, "EFI PART");
  WriteLe32(bytes, kLogicalBlockSize + 8U, 0x00010000U);
  WriteLe32(bytes, kLogicalBlockSize + 12U, 92U);
  WriteLe64(bytes, kLogicalBlockSize + 24U, 1U);
  WriteLe64(bytes, kLogicalBlockSize + 32U, 255U);
  WriteLe64(bytes, kLogicalBlockSize + 40U, 34U);
  WriteLe64(bytes, kLogicalBlockSize + 48U, 200U);
  WriteLe64(bytes, kLogicalBlockSize + 72U, 2U);
  WriteLe32(bytes, kLogicalBlockSize + 80U, 1U);
  WriteLe32(bytes, kLogicalBlockSize + 84U, 128U);

  const auto partition_offset = kLogicalBlockSize * 2U;
  const std::array<std::uint8_t, 16> apfs_guid{0xEF, 0x57, 0x34, 0x7C, 0x00, 0x00, 0xAA, 0x11,
                                               0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC};
  std::copy(apfs_guid.begin(), apfs_guid.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(partition_offset));
  WriteRawUuid(bytes, partition_offset + 16U,
               std::array<std::uint8_t, 16>{0x91, 0x92, 0x93, 0x94, 0xA0, 0xA1, 0xA2, 0xA3, 0xB0,
                                            0xB1, 0xB2, 0xB3, 0xC0, 0xC1, 0xC2, 0xC3});
  WriteLe64(bytes, partition_offset + 32U, kFirstLba);
  WriteLe64(bytes, partition_offset + 40U, kLastLba);
  WriteUtf16Le(bytes, partition_offset + 56U, "Orchard GPT");

  const auto container_offset = static_cast<std::size_t>(kFirstLba * kLogicalBlockSize);
  const auto container_bytes = MakeDirectFixture(FixtureOptions{.volume_name = "GPT Data"});
  std::copy(container_bytes.begin(), container_bytes.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(container_offset));
  return bytes;
}

orchard::apfs::VolumeContext LoadVolumeContext(orchard::blockio::Reader& reader) {
  const auto discovery_result = orchard::apfs::Discover(reader);
  ORCHARD_TEST_REQUIRE(discovery_result.ok());
  ORCHARD_TEST_REQUIRE(discovery_result.value().containers.size() == 1U);

  const auto& container = discovery_result.value().containers[0];
  orchard::apfs::PhysicalObjectReader object_reader(reader, container.byte_offset,
                                                    container.block_size);
  const auto container_omap_result =
      orchard::apfs::OmapResolver::Load(object_reader, container.omap_oid);
  ORCHARD_TEST_REQUIRE(container_omap_result.ok());

  const auto volume_result = orchard::apfs::VolumeContext::Load(
      reader, container, container.volumes[0], container_omap_result.value());
  ORCHARD_TEST_REQUIRE(volume_result.ok());
  return volume_result.value();
}

LoadedVolumeContext LoadVolumeContextFromPath(const std::filesystem::path& image_path) {
  const auto target_info = orchard::blockio::InspectTargetPath(image_path);
  auto reader_result = orchard::blockio::OpenReader(target_info);
  ORCHARD_TEST_REQUIRE(reader_result.ok());
  auto reader = std::move(reader_result).value();
  auto volume = LoadVolumeContext(*reader);
  return LoadedVolumeContext{
      .reader = std::move(reader),
      .volume = std::move(volume),
  };
}

void DetectsNxsbMagicAtObjectOffset() {
  std::vector<std::uint8_t> bytes(64U, 0U);
  WriteAscii(bytes, orchard::apfs::kApfsObjectMagicOffset, "NXSB");

  ORCHARD_TEST_REQUIRE(orchard::apfs::ProbeContainerMagic(bytes));
  ORCHARD_TEST_REQUIRE(!orchard::apfs::ProbeVolumeMagic(bytes));
}

void ParsesObjectHeaderAndRejectsShortBlock() {
  std::vector<std::uint8_t> block(kApfsBlockSize, 0U);
  WriteObjectHeader(block, 0U, 99U, 123U, orchard::apfs::kObjectTypeOmap,
                    orchard::apfs::kObjectTypeOmap);

  const auto header_result = orchard::apfs::ParseObjectHeader(block);
  ORCHARD_TEST_REQUIRE(header_result.ok());
  ORCHARD_TEST_REQUIRE(header_result.value().oid == 99U);
  ORCHARD_TEST_REQUIRE(header_result.value().xid == 123U);
  ORCHARD_TEST_REQUIRE(header_result.value().type == orchard::apfs::kObjectTypeOmap);

  const auto short_result = orchard::apfs::ParseObjectHeader(std::span(block.begin(), 8U));
  ORCHARD_TEST_REQUIRE(!short_result.ok());
  ORCHARD_TEST_REQUIRE(short_result.error().code == orchard::blockio::ErrorCode::kShortRead);
}

void ParsesLeafAndInternalBtreeNodes() {
  auto reader = orchard::blockio::MakeMemoryReader(MakeDirectFixture(), "btree-fixture");
  orchard::apfs::PhysicalObjectReader object_reader(*reader, 0U, kApfsBlockSize);

  const auto root_object_result = object_reader.ReadPhysicalObject(kContainerOmapRootBlock);
  ORCHARD_TEST_REQUIRE(root_object_result.ok());
  const auto root_node_result =
      orchard::apfs::ParseNode(root_object_result.value().view(), kApfsBlockSize);
  ORCHARD_TEST_REQUIRE(root_node_result.ok());
  ORCHARD_TEST_REQUIRE(root_node_result.value().is_root());
  ORCHARD_TEST_REQUIRE(!root_node_result.value().is_leaf());
  ORCHARD_TEST_REQUIRE(root_node_result.value().fixed_key_size() == 16U);

  const auto leaf_object_result = object_reader.ReadPhysicalObject(kContainerOmapLeafBlock);
  ORCHARD_TEST_REQUIRE(leaf_object_result.ok());
  const auto leaf_node_result =
      orchard::apfs::ParseNode(leaf_object_result.value().view(), kApfsBlockSize);
  ORCHARD_TEST_REQUIRE(leaf_node_result.ok());
  ORCHARD_TEST_REQUIRE(leaf_node_result.value().is_leaf());
  ORCHARD_TEST_REQUIRE(leaf_node_result.value().fixed_value_size() == 16U);

  const auto fs_object_result = object_reader.ReadPhysicalObject(kFsTreeBlock);
  ORCHARD_TEST_REQUIRE(fs_object_result.ok());
  const auto fs_node_result =
      orchard::apfs::ParseNode(fs_object_result.value().view(), kApfsBlockSize);
  ORCHARD_TEST_REQUIRE(fs_node_result.ok());
  ORCHARD_TEST_REQUIRE(fs_node_result.value().is_root());
  ORCHARD_TEST_REQUIRE(fs_node_result.value().is_leaf());
  ORCHARD_TEST_REQUIRE(!fs_node_result.value().uses_fixed_kv());
}

void OmapLookupExactAndOlderFallback() {
  auto reader = orchard::blockio::MakeMemoryReader(MakeDirectFixture(), "omap-fixture");
  orchard::apfs::PhysicalObjectReader object_reader(*reader, 0U, kApfsBlockSize);

  const auto resolver_result =
      orchard::apfs::OmapResolver::Load(object_reader, kContainerOmapObjectBlock);
  ORCHARD_TEST_REQUIRE(resolver_result.ok());

  const auto exact_result =
      resolver_result.value().ResolveOidToBlock(kVolumeObjectId, kCurrentCheckpointXid);
  ORCHARD_TEST_REQUIRE(exact_result.ok());
  ORCHARD_TEST_REQUIRE(exact_result.value() == kCurrentVolumeBlock);

  const auto fallback_result = resolver_result.value().ResolveOidToBlock(kVolumeObjectId, 30U);
  ORCHARD_TEST_REQUIRE(fallback_result.ok());
  ORCHARD_TEST_REQUIRE(fallback_result.value() == kLegacyVolumeBlock);

  const auto volume_omap_result =
      resolver_result.value().ResolveOidToBlock(kVolumeOmapObjectId, kCurrentCheckpointXid);
  ORCHARD_TEST_REQUIRE(volume_omap_result.ok());
  ORCHARD_TEST_REQUIRE(volume_omap_result.value() == kVolumeOmapBlock);
}

void OmapLookupRejectsDeletedMapping() {
  auto reader = orchard::blockio::MakeMemoryReader(
      MakeDirectFixture(FixtureOptions{.delete_latest_volume_mapping = true}),
      "omap-deleted-fixture");
  orchard::apfs::PhysicalObjectReader object_reader(*reader, 0U, kApfsBlockSize);

  const auto resolver_result =
      orchard::apfs::OmapResolver::Load(object_reader, kContainerOmapObjectBlock);
  ORCHARD_TEST_REQUIRE(resolver_result.ok());

  const auto deleted_result = resolver_result.value().ResolveOidToBlock(kVolumeObjectId, 50U);
  ORCHARD_TEST_REQUIRE(!deleted_result.ok());
  ORCHARD_TEST_REQUIRE(deleted_result.error().code == orchard::blockio::ErrorCode::kNotFound);
}

void DiscoversDirectContainerAndCheckpoint() {
  auto reader = orchard::blockio::MakeMemoryReader(MakeDirectFixture(), "direct-fixture");
  const auto result = orchard::apfs::Discover(*reader);

  ORCHARD_TEST_REQUIRE(result.ok());
  ORCHARD_TEST_REQUIRE(result.value().layout == orchard::apfs::LayoutKind::kDirectContainer);
  ORCHARD_TEST_REQUIRE(result.value().containers.size() == 1U);
  ORCHARD_TEST_REQUIRE(result.value().containers[0].selected_checkpoint.xid ==
                       kCurrentCheckpointXid);
  ORCHARD_TEST_REQUIRE(result.value().containers[0].volumes_resolved_via_omap);
  ORCHARD_TEST_REQUIRE(result.value().containers[0].volumes[0].policy.action ==
                       orchard::apfs::MountDisposition::kMountReadWrite);
}

void DiscoversGptWrappedContainer() {
  auto reader = orchard::blockio::MakeMemoryReader(MakeGptFixture(), "gpt-fixture");
  const auto result = orchard::apfs::Discover(*reader);

  ORCHARD_TEST_REQUIRE(result.ok());
  ORCHARD_TEST_REQUIRE(result.value().layout == orchard::apfs::LayoutKind::kGuidPartitionTable);
  ORCHARD_TEST_REQUIRE(result.value().containers.size() == 1U);
  ORCHARD_TEST_REQUIRE(result.value().containers[0].volumes[0].name == "GPT Data");
  ORCHARD_TEST_REQUIRE(result.value().containers[0].volumes[0].policy.action ==
                       orchard::apfs::MountDisposition::kMountReadWrite);
}

void VolumePathLookupAndDirectoryEnumerationWork() {
  auto reader = orchard::blockio::MakeMemoryReader(MakeDirectFixture(), "path-fixture");
  auto volume = LoadVolumeContext(*reader);

  const auto root_entries_result = orchard::apfs::ListDirectory(volume, "/");
  ORCHARD_TEST_REQUIRE(root_entries_result.ok());
  ORCHARD_TEST_REQUIRE(root_entries_result.value().size() == 4U);
  ORCHARD_TEST_REQUIRE(root_entries_result.value()[0].key.name == "alpha.txt");
  ORCHARD_TEST_REQUIRE(root_entries_result.value()[1].key.name == "compressed.txt");
  ORCHARD_TEST_REQUIRE(root_entries_result.value()[2].key.name == "docs");
  ORCHARD_TEST_REQUIRE(root_entries_result.value()[3].key.name == "holes.bin");

  const auto lookup_result = orchard::apfs::LookupPath(volume, "/DOCS/NOTE.TXT");
  ORCHARD_TEST_REQUIRE(lookup_result.ok());
  ORCHARD_TEST_REQUIRE(lookup_result.value().inode.key.header.object_id == kNoteInodeId);

  const auto missing_result = orchard::apfs::LookupPath(volume, "/docs/missing.txt");
  ORCHARD_TEST_REQUIRE(!missing_result.ok());
  ORCHARD_TEST_REQUIRE(missing_result.error().code == orchard::blockio::ErrorCode::kNotFound);

  const auto not_directory_result = orchard::apfs::LookupPath(volume, "/alpha.txt/nope");
  ORCHARD_TEST_REQUIRE(!not_directory_result.ok());
  ORCHARD_TEST_REQUIRE(not_directory_result.error().code ==
                       orchard::blockio::ErrorCode::kInvalidArgument);
}

void FileReadPathHandlesPlainSparseCompressedAndEmptyFiles() {
  auto reader = orchard::blockio::MakeMemoryReader(MakeDirectFixture(), "file-fixture");
  auto volume = LoadVolumeContext(*reader);

  const auto alpha_lookup = orchard::apfs::LookupPath(volume, "/alpha.txt");
  ORCHARD_TEST_REQUIRE(alpha_lookup.ok());
  const auto alpha_bytes =
      orchard::apfs::ReadWholeFile(volume, alpha_lookup.value().inode.key.header.object_id);
  ORCHARD_TEST_REQUIRE(alpha_bytes.ok());
  ORCHARD_TEST_REQUIRE(std::string(alpha_bytes.value().begin(), alpha_bytes.value().end()) ==
                       std::string(kAlphaExtent1) + std::string(kAlphaExtent2));

  const auto alpha_partial = orchard::apfs::ReadFileRange(
      volume, orchard::apfs::FileReadRequest{
                  .inode_id = alpha_lookup.value().inode.key.header.object_id,
                  .offset = 6U,
                  .size = 8U,
              });
  ORCHARD_TEST_REQUIRE(alpha_partial.ok());
  ORCHARD_TEST_REQUIRE(std::string(alpha_partial.value().begin(), alpha_partial.value().end()) ==
                       std::string(kAlphaExtent2));

  const auto sparse_lookup = orchard::apfs::LookupPath(volume, "/holes.bin");
  ORCHARD_TEST_REQUIRE(sparse_lookup.ok());
  const auto sparse_bytes =
      orchard::apfs::ReadWholeFile(volume, sparse_lookup.value().inode.key.header.object_id);
  ORCHARD_TEST_REQUIRE(sparse_bytes.ok());
  ORCHARD_TEST_REQUIRE(sparse_bytes.value().size() == 12U);
  ORCHARD_TEST_REQUIRE(
      std::string(sparse_bytes.value().begin(), sparse_bytes.value().begin() + 4) ==
      std::string(kSparseExtent1));
  ORCHARD_TEST_REQUIRE(sparse_bytes.value()[4] == 0U);
  ORCHARD_TEST_REQUIRE(sparse_bytes.value()[7] == 0U);
  ORCHARD_TEST_REQUIRE(std::string(sparse_bytes.value().begin() + 8, sparse_bytes.value().end()) ==
                       std::string(kSparseExtent2));

  const auto compressed_lookup = orchard::apfs::LookupPath(volume, "/compressed.txt");
  ORCHARD_TEST_REQUIRE(compressed_lookup.ok());
  const auto compressed_bytes =
      orchard::apfs::ReadWholeFile(volume, compressed_lookup.value().inode.key.header.object_id);
  ORCHARD_TEST_REQUIRE(compressed_bytes.ok());
  ORCHARD_TEST_REQUIRE(std::string(compressed_bytes.value().begin(),
                                   compressed_bytes.value().end()) == std::string(kCompressedText));

  const auto compressed_metadata =
      orchard::apfs::GetFileMetadata(volume, compressed_lookup.value().inode.key.header.object_id);
  ORCHARD_TEST_REQUIRE(compressed_metadata.ok());
  ORCHARD_TEST_REQUIRE(compressed_metadata.value().compression.kind ==
                       orchard::apfs::CompressionKind::kDecmpfsUncompressedAttribute);

  const auto empty_lookup = orchard::apfs::LookupPath(volume, "/docs/empty.txt");
  ORCHARD_TEST_REQUIRE(empty_lookup.ok());
  const auto empty_bytes =
      orchard::apfs::ReadWholeFile(volume, empty_lookup.value().inode.key.header.object_id);
  ORCHARD_TEST_REQUIRE(empty_bytes.ok());
  ORCHARD_TEST_REQUIRE(empty_bytes.value().empty());
}

void PolicyEngineClassifiesSyntheticVolumes() {
  auto snapshot_reader = orchard::blockio::MakeMemoryReader(
      MakeDirectFixture(FixtureOptions{
          .volume_name = "Snapshot Data",
          .incompatible_features = orchard::apfs::kVolumeIncompatCaseInsensitive |
                                   orchard::apfs::kVolumeIncompatDatalessSnaps,
      }),
      "snapshot-fixture");
  const auto snapshot_result = orchard::apfs::Discover(*snapshot_reader);
  ORCHARD_TEST_REQUIRE(snapshot_result.ok());
  ORCHARD_TEST_REQUIRE(snapshot_result.value().containers[0].volumes[0].policy.action ==
                       orchard::apfs::MountDisposition::kMountReadOnly);

  auto sealed_reader = orchard::blockio::MakeMemoryReader(
      MakeDirectFixture(FixtureOptions{
          .volume_name = "System",
          .incompatible_features =
              orchard::apfs::kVolumeIncompatCaseInsensitive | orchard::apfs::kVolumeIncompatSealed,
          .role = orchard::apfs::kVolumeRoleSystem,
      }),
      "sealed-fixture");
  const auto sealed_result = orchard::apfs::Discover(*sealed_reader);
  ORCHARD_TEST_REQUIRE(sealed_result.ok());
  ORCHARD_TEST_REQUIRE(sealed_result.value().containers[0].volumes[0].policy.action ==
                       orchard::apfs::MountDisposition::kReject);
}

void InspectTargetUsesRealReaderPathAndEnrichesOutput() {
  const auto temp_path = std::filesystem::temp_directory_path() / "orchard_apfs_direct.img";
  const auto bytes = MakeDirectFixture();

  {
    std::ofstream output(temp_path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }

  const auto target_info = orchard::blockio::InspectTargetPath(temp_path);
  const auto result = orchard::apfs::InspectTarget(target_info);

  ORCHARD_TEST_REQUIRE(result.status == orchard::apfs::InspectionStatus::kSuccess);
  ORCHARD_TEST_REQUIRE(result.report.containers.size() == 1U);
  ORCHARD_TEST_REQUIRE(result.report.containers[0].volumes[0].policy.action ==
                       orchard::apfs::MountDisposition::kMountReadWrite);
  ORCHARD_TEST_REQUIRE(result.report.containers[0].volumes[0].root_entries.size() == 4U);
  ORCHARD_TEST_REQUIRE(result.report.containers[0].volumes[0].root_entries[0].name == "alpha.txt");
  ORCHARD_TEST_REQUIRE(result.report.containers[0].volumes[0].root_file_probes.size() >= 2U);
  ORCHARD_TEST_REQUIRE(result.report.containers[0].volumes[0].root_file_probes[0].path ==
                       "/alpha.txt");
  ORCHARD_TEST_REQUIRE(std::any_of(result.report.containers[0].volumes[0].root_file_probes.begin(),
                                   result.report.containers[0].volumes[0].root_file_probes.end(),
                                   [](const orchard::apfs::FileProbeInfo& probe) {
                                     return probe.compression == "decmpfs_uncompressed_attribute";
                                   }));

  std::filesystem::remove(temp_path);
}

void WinFspPathBridgeNormalizesPaths() {
  const auto root_result = orchard::fs_winfsp::NormalizeWindowsPath(L"\\");
  ORCHARD_TEST_REQUIRE(root_result.ok());
  ORCHARD_TEST_REQUIRE(root_result.value() == "/");

  const auto nested_result = orchard::fs_winfsp::NormalizeWindowsPath(L"\\docs\\empty.txt");
  ORCHARD_TEST_REQUIRE(nested_result.ok());
  ORCHARD_TEST_REQUIRE(nested_result.value() == "/docs/empty.txt");

  const auto normalized_parent_result =
      orchard::fs_winfsp::NormalizeWindowsPath(L"\\docs\\\\.\\..\\alpha.txt");
  ORCHARD_TEST_REQUIRE(normalized_parent_result.ok());
  ORCHARD_TEST_REQUIRE(normalized_parent_result.value() == "/alpha.txt");

  const auto windows_path_result = orchard::fs_winfsp::OrchardPathToWindowsPath("/docs/empty.txt");
  ORCHARD_TEST_REQUIRE(windows_path_result.ok());
  ORCHARD_TEST_REQUIRE(windows_path_result.value() == L"\\docs\\empty.txt");

  const auto stream_result = orchard::fs_winfsp::NormalizeWindowsPath(L"\\bad:name");
  ORCHARD_TEST_REQUIRE(!stream_result.ok());
  ORCHARD_TEST_REQUIRE(stream_result.error().code == orchard::blockio::ErrorCode::kNotImplemented);

  const auto invalid_character_result = orchard::fs_winfsp::NormalizeWindowsPath(L"\\bad*name");
  ORCHARD_TEST_REQUIRE(!invalid_character_result.ok());
  ORCHARD_TEST_REQUIRE(invalid_character_result.error().code ==
                       orchard::blockio::ErrorCode::kInvalidArgument);
}

void WinFspFileInfoUsesApfsMetadata() {
  const auto temp_path = std::filesystem::temp_directory_path() / "orchard_apfs_query_info.img";
  const auto bytes = MakeDirectFixture();

  {
    std::ofstream output(temp_path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }

  orchard::fs_winfsp::MountConfig config;
  config.target_path = temp_path;
  config.mount_point = L"Q:";

  const auto mounted_volume_result = orchard::fs_winfsp::OpenMountedVolume(config);
  ORCHARD_TEST_REQUIRE(mounted_volume_result.ok());

  const auto alpha_result = mounted_volume_result.value()->ResolveFileNode("/alpha.txt");
  ORCHARD_TEST_REQUIRE(alpha_result.ok());
  const auto alpha_info = orchard::fs_winfsp::BuildBasicFileInfo(
      alpha_result.value(), mounted_volume_result.value()->volume_context().block_size());
  ORCHARD_TEST_REQUIRE(alpha_info.file_size == kAlphaExtent1.size() + kAlphaExtent2.size());
  ORCHARD_TEST_REQUIRE(alpha_info.allocation_size == kAlphaExtent1.size() + kAlphaExtent2.size());
  ORCHARD_TEST_REQUIRE(alpha_info.hard_links == 1U);
  ORCHARD_TEST_REQUIRE(alpha_info.creation_time != 0U);
  ORCHARD_TEST_REQUIRE((alpha_info.file_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U);

  const auto docs_result = mounted_volume_result.value()->ResolveFileNode("/docs");
  ORCHARD_TEST_REQUIRE(docs_result.ok());
  ORCHARD_TEST_REQUIRE(docs_result.value().metadata.child_count == 2U);
  const auto docs_info = orchard::fs_winfsp::BuildBasicFileInfo(
      docs_result.value(), mounted_volume_result.value()->volume_context().block_size());
  ORCHARD_TEST_REQUIRE((docs_info.file_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U);
  ORCHARD_TEST_REQUIRE(docs_info.allocation_size == 0U);
  ORCHARD_TEST_REQUIRE(docs_info.creation_time != 0U);

  std::filesystem::remove(temp_path);
}

void WinFspDirectoryQueryFiltersDeterministically() {
  const auto temp_path = std::filesystem::temp_directory_path() / "orchard_apfs_query_dir.img";
  const auto bytes = MakeDirectFixture();

  {
    std::ofstream output(temp_path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }

  orchard::fs_winfsp::MountConfig config;
  config.target_path = temp_path;
  config.mount_point = L"T:";

  const auto mounted_volume_result = orchard::fs_winfsp::OpenMountedVolume(config);
  ORCHARD_TEST_REQUIRE(mounted_volume_result.ok());

  const auto root_result = mounted_volume_result.value()->ResolveFileNode("/");
  ORCHARD_TEST_REQUIRE(root_result.ok());
  const auto entries_result =
      mounted_volume_result.value()->ListDirectoryEntries(root_result.value().inode_id);
  ORCHARD_TEST_REQUIRE(entries_result.ok());

  const auto query_entries_result = orchard::fs_winfsp::BuildDirectoryQueryEntries(
      mounted_volume_result.value()->volume_context(), root_result.value(), entries_result.value());
  ORCHARD_TEST_REQUIRE(query_entries_result.ok());
  ORCHARD_TEST_REQUIRE(query_entries_result.value().size() == 4U);
  ORCHARD_TEST_REQUIRE(query_entries_result.value()[0].file_name == L"alpha.txt");
  ORCHARD_TEST_REQUIRE(query_entries_result.value()[1].file_name == L"compressed.txt");
  ORCHARD_TEST_REQUIRE(query_entries_result.value()[2].file_name == L"docs");
  ORCHARD_TEST_REQUIRE(query_entries_result.value()[3].file_name == L"holes.bin");

  const auto marker_filtered = orchard::fs_winfsp::FilterDirectoryQueryEntries(
      query_entries_result.value(), orchard::fs_winfsp::DirectoryQueryRequest{
                                        .marker = std::wstring(L"alpha.txt"),
                                        .pattern = L"*.txt",
                                        .case_insensitive = true,
                                    });
  ORCHARD_TEST_REQUIRE(marker_filtered.size() == 1U);
  ORCHARD_TEST_REQUIRE(marker_filtered[0].file_name == L"compressed.txt");

  const auto pattern_filtered = orchard::fs_winfsp::FilterDirectoryQueryEntries(
      query_entries_result.value(), orchard::fs_winfsp::DirectoryQueryRequest{
                                        .marker = std::nullopt,
                                        .pattern = L"ALPHA.*",
                                        .case_insensitive = true,
                                    });
  ORCHARD_TEST_REQUIRE(pattern_filtered.size() == 1U);
  ORCHARD_TEST_REQUIRE(pattern_filtered[0].file_name == L"alpha.txt");

  std::filesystem::remove(temp_path);
}

void WinFspMountedVolumeReusesOpenNodeIdentity() {
  const auto temp_path = std::filesystem::temp_directory_path() / "orchard_apfs_query_node.img";
  const auto bytes = MakeDirectFixture();

  {
    std::ofstream output(temp_path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }

  orchard::fs_winfsp::MountConfig config;
  config.target_path = temp_path;
  config.mount_point = L"U:";

  const auto mounted_volume_result = orchard::fs_winfsp::OpenMountedVolume(config);
  ORCHARD_TEST_REQUIRE(mounted_volume_result.ok());

  const auto first_open_result = mounted_volume_result.value()->AcquireOpenNode("/alpha.txt");
  ORCHARD_TEST_REQUIRE(first_open_result.ok());
  const auto second_open_result = mounted_volume_result.value()->AcquireOpenNode("/ALPHA.TXT");
  ORCHARD_TEST_REQUIRE(second_open_result.ok());
  ORCHARD_TEST_REQUIRE(first_open_result.value().get() == second_open_result.value().get());

  mounted_volume_result.value()->ReleaseOpenNode(first_open_result.value().get());
  mounted_volume_result.value()->ReleaseOpenNode(second_open_result.value().get());

  std::filesystem::remove(temp_path);
}

void LargeFixtureSupportsLargeDirectoryPathLookupAndFileRead() {
  auto loaded = LoadVolumeContextFromPath(SampleFixturePath("explorer-large.img"));
  auto& volume = loaded.volume;

  const auto root_entries_result = orchard::apfs::ListDirectory(volume, "/");
  ORCHARD_TEST_REQUIRE(root_entries_result.ok());
  ORCHARD_TEST_REQUIRE(root_entries_result.value().size() == 4U);
  ORCHARD_TEST_REQUIRE(root_entries_result.value()[0].key.name == "alpha.txt");
  ORCHARD_TEST_REQUIRE(root_entries_result.value()[1].key.name == "bulk items");
  ORCHARD_TEST_REQUIRE(root_entries_result.value()[2].key.name == "copy-source.bin");
  ORCHARD_TEST_REQUIRE(root_entries_result.value()[3].key.name == "preview.txt");

  const auto bulk_entries_result = orchard::apfs::ListDirectory(volume, "/bulk items");
  ORCHARD_TEST_REQUIRE(bulk_entries_result.ok());
  ORCHARD_TEST_REQUIRE(bulk_entries_result.value().size() == 181U);

  const auto nested_lookup_result =
      orchard::apfs::LookupPath(volume, "/bulk items/Nested Folder/deep-note.txt");
  ORCHARD_TEST_REQUIRE(nested_lookup_result.ok());
  const auto nested_bytes =
      orchard::apfs::ReadWholeFile(volume, nested_lookup_result.value().inode.key.header.object_id);
  ORCHARD_TEST_REQUIRE(nested_bytes.ok());
  ORCHARD_TEST_REQUIRE(std::string(nested_bytes.value().begin(), nested_bytes.value().end()) ==
                       "Explorer deep note\n");

  const auto copy_lookup_result = orchard::apfs::LookupPath(volume, "/copy-source.bin");
  ORCHARD_TEST_REQUIRE(copy_lookup_result.ok());
  const auto copy_bytes =
      orchard::apfs::ReadWholeFile(volume, copy_lookup_result.value().inode.key.header.object_id);
  ORCHARD_TEST_REQUIRE(copy_bytes.ok());
  ORCHARD_TEST_REQUIRE(copy_bytes.value().size() ==
                       (static_cast<std::size_t>(4U) * kApfsBlockSize));
  ORCHARD_TEST_REQUIRE(std::string(copy_bytes.value().begin(), copy_bytes.value().begin() + 22) ==
                       "ORCHARD-COPY-BLOCK-00\n");
}

void LinkFixtureSupportsSymlinkTargetsAndHardLinks() {
  auto loaded = LoadVolumeContextFromPath(SampleFixturePath("link-behavior.img"));
  auto& volume = loaded.volume;

  const auto relative_link_lookup = orchard::apfs::LookupPath(volume, "/a-relative-note-link.txt");
  ORCHARD_TEST_REQUIRE(relative_link_lookup.ok());
  ORCHARD_TEST_REQUIRE(relative_link_lookup.value().inode.kind ==
                       orchard::apfs::InodeKind::kSymlink);

  const auto relative_target = orchard::apfs::ReadSymlinkTarget(
      volume, relative_link_lookup.value().inode.key.header.object_id);
  ORCHARD_TEST_REQUIRE(relative_target.ok());
  ORCHARD_TEST_REQUIRE(relative_target.value() == "docs/note.txt");

  const auto absolute_link_lookup = orchard::apfs::LookupPath(volume, "/absolute-alpha-link.txt");
  ORCHARD_TEST_REQUIRE(absolute_link_lookup.ok());
  const auto absolute_target = orchard::apfs::ReadSymlinkTarget(
      volume, absolute_link_lookup.value().inode.key.header.object_id);
  ORCHARD_TEST_REQUIRE(absolute_target.ok());
  ORCHARD_TEST_REQUIRE(absolute_target.value() == "/alpha.txt");

  const auto hard_a_lookup = orchard::apfs::LookupPath(volume, "/hard-a.txt");
  ORCHARD_TEST_REQUIRE(hard_a_lookup.ok());
  const auto hard_b_lookup = orchard::apfs::LookupPath(volume, "/hard-b.txt");
  ORCHARD_TEST_REQUIRE(hard_b_lookup.ok());
  ORCHARD_TEST_REQUIRE(hard_a_lookup.value().inode.key.header.object_id ==
                       hard_b_lookup.value().inode.key.header.object_id);
  ORCHARD_TEST_REQUIRE(hard_a_lookup.value().inode.link_count == 2U);
  ORCHARD_TEST_REQUIRE(hard_b_lookup.value().inode.link_count == 2U);

  const auto hard_a_bytes =
      orchard::apfs::ReadWholeFile(volume, hard_a_lookup.value().inode.key.header.object_id);
  ORCHARD_TEST_REQUIRE(hard_a_bytes.ok());
  ORCHARD_TEST_REQUIRE(std::string(hard_a_bytes.value().begin(), hard_a_bytes.value().end()) ==
                       "Shared hard-link payload\n");

  const auto note_lookup = orchard::apfs::LookupPath(volume, "/docs/note.txt");
  ORCHARD_TEST_REQUIRE(note_lookup.ok());
  const auto note_alias_lookup = orchard::apfs::LookupPath(volume, "/note-link.txt");
  ORCHARD_TEST_REQUIRE(note_alias_lookup.ok());
  ORCHARD_TEST_REQUIRE(note_lookup.value().inode.key.header.object_id ==
                       note_alias_lookup.value().inode.key.header.object_id);
  ORCHARD_TEST_REQUIRE(note_lookup.value().inode.link_count == 2U);
}

void InspectTargetReportsLinkMetadata() {
  const auto target_info =
      orchard::blockio::InspectTargetPath(SampleFixturePath("link-behavior.img"));
  const auto result = orchard::apfs::InspectTarget(target_info);

  ORCHARD_TEST_REQUIRE(result.status == orchard::apfs::InspectionStatus::kSuccess);
  ORCHARD_TEST_REQUIRE(result.report.containers.size() == 1U);
  const auto& probes = result.report.containers[0].volumes[0].root_file_probes;
  const auto symlink_probe =
      std::find_if(probes.begin(), probes.end(), [](const orchard::apfs::FileProbeInfo& probe) {
        return probe.path == "/a-relative-note-link.txt";
      });
  ORCHARD_TEST_REQUIRE(symlink_probe != probes.end());
  ORCHARD_TEST_REQUIRE(symlink_probe->symlink_target.has_value());
  const auto symlink_target = symlink_probe->symlink_target.value_or(std::string{});
  ORCHARD_TEST_REQUIRE(symlink_target == "docs/note.txt");

  const auto hard_link_probe =
      std::find_if(probes.begin(), probes.end(), [](const orchard::apfs::FileProbeInfo& probe) {
        return probe.path == "/hard-a.txt";
      });
  ORCHARD_TEST_REQUIRE(hard_link_probe != probes.end());
  ORCHARD_TEST_REQUIRE(hard_link_probe->link_count == 2U);
  ORCHARD_TEST_REQUIRE(std::find(hard_link_probe->aliases.begin(), hard_link_probe->aliases.end(),
                                 "/hard-b.txt") != hard_link_probe->aliases.end());
}

void WinFspSymlinkReparseTranslationSupportsRelativeAndAbsoluteTargets() {
  const auto relative_result =
      orchard::fs_winfsp::TranslateSymlinkTarget(orchard::fs_winfsp::SymlinkReparseRequest{
          .mount_point = L"R:",
          .target = "docs/note.txt",
      });
  ORCHARD_TEST_REQUIRE(relative_result.ok());
  ORCHARD_TEST_REQUIRE(relative_result.value().relative);
  ORCHARD_TEST_REQUIRE(relative_result.value().print_name == L"docs\\note.txt");

  const auto absolute_result =
      orchard::fs_winfsp::TranslateSymlinkTarget(orchard::fs_winfsp::SymlinkReparseRequest{
          .mount_point = L"R:",
          .target = "/alpha.txt",
      });
  ORCHARD_TEST_REQUIRE(absolute_result.ok());
  ORCHARD_TEST_REQUIRE(!absolute_result.value().relative);
  ORCHARD_TEST_REQUIRE(absolute_result.value().print_name == L"R:\\alpha.txt");
  ORCHARD_TEST_REQUIRE(absolute_result.value().substitute_name == L"\\??\\R:\\alpha.txt");

  const auto invalid_result =
      orchard::fs_winfsp::TranslateSymlinkTarget(orchard::fs_winfsp::SymlinkReparseRequest{
          .mount_point = L"R:",
          .target = "bad:name",
      });
  ORCHARD_TEST_REQUIRE(!invalid_result.ok());
  ORCHARD_TEST_REQUIRE(invalid_result.error().code ==
                       orchard::blockio::ErrorCode::kUnsupportedTarget);
}

void WinFspMountedVolumePreservesHardLinkIdentityAndSymlinkTargets() {
  orchard::fs_winfsp::MountConfig config;
  config.target_path = SampleFixturePath("link-behavior.img");
  config.mount_point = L"W:";

  const auto mounted_volume_result = orchard::fs_winfsp::OpenMountedVolume(config);
  ORCHARD_TEST_REQUIRE(mounted_volume_result.ok());

  const auto hard_a_result = mounted_volume_result.value()->ResolveFileNode("/hard-a.txt");
  ORCHARD_TEST_REQUIRE(hard_a_result.ok());
  const auto hard_b_result = mounted_volume_result.value()->ResolveFileNode("/hard-b.txt");
  ORCHARD_TEST_REQUIRE(hard_b_result.ok());
  ORCHARD_TEST_REQUIRE(hard_a_result.value().inode_id == hard_b_result.value().inode_id);

  const auto hard_a_info = orchard::fs_winfsp::BuildBasicFileInfo(
      hard_a_result.value(), mounted_volume_result.value()->volume_context().block_size());
  const auto hard_b_info = orchard::fs_winfsp::BuildBasicFileInfo(
      hard_b_result.value(), mounted_volume_result.value()->volume_context().block_size());
  ORCHARD_TEST_REQUIRE(hard_a_info.index_number == hard_b_info.index_number);
  ORCHARD_TEST_REQUIRE(hard_a_info.hard_links == 2U);
  ORCHARD_TEST_REQUIRE(hard_b_info.hard_links == 2U);

  const auto relative_link_result =
      mounted_volume_result.value()->ResolveFileNode("/a-relative-note-link.txt");
  ORCHARD_TEST_REQUIRE(relative_link_result.ok());
  ORCHARD_TEST_REQUIRE(relative_link_result.value().symlink_target.has_value());
  const auto relative_link_target =
      relative_link_result.value().symlink_target.value_or(std::string{});
  ORCHARD_TEST_REQUIRE(relative_link_target == "docs/note.txt");
  const auto relative_link_info = orchard::fs_winfsp::BuildBasicFileInfo(
      relative_link_result.value(), mounted_volume_result.value()->volume_context().block_size());
  ORCHARD_TEST_REQUIRE((relative_link_info.file_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U);
  ORCHARD_TEST_REQUIRE(relative_link_info.reparse_tag == IO_REPARSE_TAG_SYMLINK);
}

void WinFspLargeDirectoryPaginationResumesDeterministically() {
  orchard::fs_winfsp::MountConfig config;
  config.target_path = SampleFixturePath("explorer-large.img");
  config.mount_point = L"V:";

  const auto mounted_volume_result = orchard::fs_winfsp::OpenMountedVolume(config);
  ORCHARD_TEST_REQUIRE(mounted_volume_result.ok());

  const auto bulk_result = mounted_volume_result.value()->ResolveFileNode("/bulk items");
  ORCHARD_TEST_REQUIRE(bulk_result.ok());
  const auto entries_result =
      mounted_volume_result.value()->ListDirectoryEntries(bulk_result.value().inode_id);
  ORCHARD_TEST_REQUIRE(entries_result.ok());

  const auto query_entries_result = orchard::fs_winfsp::BuildDirectoryQueryEntries(
      mounted_volume_result.value()->volume_context(), bulk_result.value(), entries_result.value());
  ORCHARD_TEST_REQUIRE(query_entries_result.ok());
  ORCHARD_TEST_REQUIRE(query_entries_result.value().size() == 181U);

  std::vector<std::wstring> expected_names;
  expected_names.reserve(query_entries_result.value().size());
  for (const auto& query_entry : query_entries_result.value()) {
    expected_names.push_back(query_entry.file_name);
  }

  std::vector<std::wstring> paged_names;
  std::optional<std::wstring> marker;
  for (std::size_t iteration = 0; iteration < 64U; ++iteration) {
    const auto filtered_entries = orchard::fs_winfsp::FilterDirectoryQueryEntries(
        query_entries_result.value(), orchard::fs_winfsp::DirectoryQueryRequest{
                                          .marker = marker,
                                          .pattern = L"*",
                                          .case_insensitive = true,
                                      });
    const auto page = orchard::fs_winfsp::PaginateDirectoryQueryEntries(
        filtered_entries, orchard::fs_winfsp::DirectoryQueryPaginationConfig{
                              .max_bytes = 1024U,
                              .base_entry_size = 64U,
                          });
    ORCHARD_TEST_REQUIRE(!page.entries.empty());
    for (const auto& entry : page.entries) {
      paged_names.push_back(entry.file_name);
    }
    if (!page.truncated) {
      break;
    }
    ORCHARD_TEST_REQUIRE(page.last_emitted_name.has_value());
    marker = page.last_emitted_name;
  }

  ORCHARD_TEST_REQUIRE(paged_names == expected_names);
  std::set<std::wstring> unique_names(paged_names.begin(), paged_names.end());
  ORCHARD_TEST_REQUIRE(unique_names.size() == paged_names.size());
}

void WinFspMountedVolumeOpensSyntheticFixture() {
  const auto temp_path = std::filesystem::temp_directory_path() / "orchard_apfs_mount.img";
  const auto bytes = MakeDirectFixture();

  {
    std::ofstream output(temp_path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }

  orchard::fs_winfsp::MountConfig config;
  config.target_path = temp_path;
  config.mount_point = L"R:";

  const auto mounted_volume_result = orchard::fs_winfsp::OpenMountedVolume(config);
  ORCHARD_TEST_REQUIRE(mounted_volume_result.ok());
  ORCHARD_TEST_REQUIRE(mounted_volume_result.value()->volume_info().name == "Orchard Data");

  const auto root_result = mounted_volume_result.value()->ResolveFileNode("/");
  ORCHARD_TEST_REQUIRE(root_result.ok());
  ORCHARD_TEST_REQUIRE(root_result.value().metadata.kind == orchard::apfs::InodeKind::kDirectory);

  const auto alpha_result = mounted_volume_result.value()->ResolveFileNode("/alpha.txt");
  ORCHARD_TEST_REQUIRE(alpha_result.ok());
  ORCHARD_TEST_REQUIRE(alpha_result.value().metadata.kind ==
                       orchard::apfs::InodeKind::kRegularFile);

  orchard::apfs::FileReadRequest request;
  request.inode_id = alpha_result.value().inode_id;
  request.offset = 0U;
  request.size = 8U;
  const auto bytes_result = mounted_volume_result.value()->ReadFileRange(request);
  ORCHARD_TEST_REQUIRE(bytes_result.ok());
  ORCHARD_TEST_REQUIRE(bytes_result.value().size() == 8U);

  std::filesystem::remove(temp_path);
}

void WinFspMountedVolumeRejectsReadWritePolicyWhenDowngradeDisabled() {
  const auto temp_path = std::filesystem::temp_directory_path() / "orchard_apfs_mount_policy.img";
  const auto bytes = MakeDirectFixture();

  {
    std::ofstream output(temp_path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }

  orchard::fs_winfsp::MountConfig config;
  config.target_path = temp_path;
  config.mount_point = L"S:";
  config.allow_downgrade_from_readwrite = false;

  const auto mounted_volume_result = orchard::fs_winfsp::OpenMountedVolume(config);
  ORCHARD_TEST_REQUIRE(!mounted_volume_result.ok());
  ORCHARD_TEST_REQUIRE(mounted_volume_result.error().code ==
                       orchard::blockio::ErrorCode::kUnsupportedTarget);

  std::filesystem::remove(temp_path);
}

} // namespace

int main() {
  return orchard_test::RunTests({
      {"DetectsNxsbMagicAtObjectOffset", &DetectsNxsbMagicAtObjectOffset},
      {"ParsesObjectHeaderAndRejectsShortBlock", &ParsesObjectHeaderAndRejectsShortBlock},
      {"ParsesLeafAndInternalBtreeNodes", &ParsesLeafAndInternalBtreeNodes},
      {"OmapLookupExactAndOlderFallback", &OmapLookupExactAndOlderFallback},
      {"OmapLookupRejectsDeletedMapping", &OmapLookupRejectsDeletedMapping},
      {"DiscoversDirectContainerAndCheckpoint", &DiscoversDirectContainerAndCheckpoint},
      {"DiscoversGptWrappedContainer", &DiscoversGptWrappedContainer},
      {"VolumePathLookupAndDirectoryEnumerationWork", &VolumePathLookupAndDirectoryEnumerationWork},
      {"FileReadPathHandlesPlainSparseCompressedAndEmptyFiles",
       &FileReadPathHandlesPlainSparseCompressedAndEmptyFiles},
      {"PolicyEngineClassifiesSyntheticVolumes", &PolicyEngineClassifiesSyntheticVolumes},
      {"InspectTargetUsesRealReaderPathAndEnrichesOutput",
       &InspectTargetUsesRealReaderPathAndEnrichesOutput},
      {"WinFspPathBridgeNormalizesPaths", &WinFspPathBridgeNormalizesPaths},
      {"WinFspFileInfoUsesApfsMetadata", &WinFspFileInfoUsesApfsMetadata},
      {"WinFspDirectoryQueryFiltersDeterministically",
       &WinFspDirectoryQueryFiltersDeterministically},
      {"WinFspMountedVolumeReusesOpenNodeIdentity", &WinFspMountedVolumeReusesOpenNodeIdentity},
      {"LargeFixtureSupportsLargeDirectoryPathLookupAndFileRead",
       &LargeFixtureSupportsLargeDirectoryPathLookupAndFileRead},
      {"LinkFixtureSupportsSymlinkTargetsAndHardLinks",
       &LinkFixtureSupportsSymlinkTargetsAndHardLinks},
      {"InspectTargetReportsLinkMetadata", &InspectTargetReportsLinkMetadata},
      {"WinFspSymlinkReparseTranslationSupportsRelativeAndAbsoluteTargets",
       &WinFspSymlinkReparseTranslationSupportsRelativeAndAbsoluteTargets},
      {"WinFspMountedVolumePreservesHardLinkIdentityAndSymlinkTargets",
       &WinFspMountedVolumePreservesHardLinkIdentityAndSymlinkTargets},
      {"WinFspLargeDirectoryPaginationResumesDeterministically",
       &WinFspLargeDirectoryPaginationResumesDeterministically},
      {"WinFspMountedVolumeOpensSyntheticFixture", &WinFspMountedVolumeOpensSyntheticFixture},
      {"WinFspMountedVolumeRejectsReadWritePolicyWhenDowngradeDisabled",
       &WinFspMountedVolumeRejectsReadWritePolicyWhenDowngradeDisabled},
  });
}
