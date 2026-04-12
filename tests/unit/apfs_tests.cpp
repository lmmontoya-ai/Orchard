#include <array>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

#include "orchard/apfs/btree.h"
#include "orchard/apfs/discovery.h"
#include "orchard/apfs/inspection.h"
#include "orchard/apfs/object.h"
#include "orchard/apfs/omap.h"
#include "orchard/apfs/probe.h"
#include "orchard/blockio/inspection_target.h"
#include "orchard/blockio/reader.h"
#include "orchard_test/test.h"

namespace {

constexpr std::uint32_t kApfsBlockSize = 4096U;
constexpr std::uint64_t kOmapObjectBlock = 2U;
constexpr std::uint64_t kOmapRootBlock = 3U;
constexpr std::uint64_t kOmapLeafBlock = 4U;
constexpr std::uint64_t kLegacyVolumeBlock = 5U;
constexpr std::uint64_t kCurrentVolumeBlock = 6U;
constexpr std::uint64_t kVolumeObjectId = 77U;
constexpr std::uint64_t kCurrentCheckpointXid = 42U;

enum class FixtureMode : std::uint8_t {
  kCurrentMapping,
  kDeletedLatestMapping,
};

struct OmapLeafRecord {
  std::uint64_t oid = 0;
  std::uint64_t xid = 0;
  std::uint32_t flags = 0;
  std::uint64_t physical_block = 0;
};

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
                       const std::uint64_t xid, const std::uint64_t block_count) {
  WriteObjectHeader(bytes, base_offset, base_offset / kApfsBlockSize, xid,
                    orchard::apfs::kObjectTypeNxSuperblock, 0U);
  WriteAscii(bytes, base_offset + 0x20U, "NXSB");
  WriteLe32(bytes, base_offset + 0x24U, kApfsBlockSize);
  WriteLe64(bytes, base_offset + 0x28U, block_count);
  WriteRawUuid(bytes, base_offset + 0x48U,
               std::array<std::uint8_t, 16>{0x10, 0x11, 0x12, 0x13, 0x20, 0x21, 0x22, 0x23, 0x30,
                                            0x31, 0x32, 0x33, 0x40, 0x41, 0x42, 0x43});
  WriteLe64(bytes, base_offset + 0x60U, xid + 1U);
  WriteLe32(bytes, base_offset + 0x68U, 1U);
  WriteLe64(bytes, base_offset + 0x70U, 1U);
  WriteLe64(bytes, base_offset + 0x98U, 5U);
  WriteLe64(bytes, base_offset + 0xA0U, kOmapObjectBlock);
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
  WriteLe64(bytes, base_offset + 0x38U, 0U);
  WriteLe64(bytes, base_offset + 0x40U, 0U);
  WriteLe64(bytes, base_offset + 0x48U, 0U);
  WriteLe64(bytes, base_offset + 0x50U, 0U);
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
  WriteBtreeInfoFooter(bytes, base_offset, 16U, 8U, 2U, 2U);
}

void WriteOmapLeafNode(std::vector<std::uint8_t>& bytes, const std::size_t base_offset,
                       const std::uint64_t oid, const std::uint64_t xid,
                       const std::vector<OmapLeafRecord>& records) {
  const auto table_length = static_cast<std::uint16_t>(records.size() * 4U);
  const auto key_area_start = base_offset + orchard::apfs::kBtreeNodeHeaderSize + table_length;
  const auto value_area_end = base_offset + kApfsBlockSize;

  WriteObjectHeader(bytes, base_offset, oid, xid, orchard::apfs::kObjectTypeBtreeNode,
                    orchard::apfs::kObjectTypeOmap);
  WriteLe16(bytes, base_offset + 0x20U,
            orchard::apfs::kBtreeNodeFlagLeaf | orchard::apfs::kBtreeNodeFlagFixedKv);
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
}

void WriteVolumeSuperblock(std::vector<std::uint8_t>& bytes, const std::size_t base_offset,
                           const std::uint64_t object_id, const std::uint64_t xid,
                           const std::uint64_t incompatible_features, const std::uint16_t role,
                           const std::string_view name) {
  WriteObjectHeader(bytes, base_offset, object_id, xid, orchard::apfs::kObjectTypeFs, 0U);
  WriteAscii(bytes, base_offset + 0x20U, "APSB");
  WriteLe32(bytes, base_offset + 0x24U, 0U);
  WriteLe64(bytes, base_offset + 0x28U, 0U);
  WriteLe64(bytes, base_offset + 0x30U, 0U);
  WriteLe64(bytes, base_offset + 0x38U, incompatible_features);
  WriteRawUuid(bytes, base_offset + 0xF0U,
               std::array<std::uint8_t, 16>{0x50, 0x51, 0x52, 0x53, 0x60, 0x61, 0x62, 0x63, 0x70,
                                            0x71, 0x72, 0x73, 0x80, 0x81, 0x82, 0x83});
  WriteAscii(bytes, base_offset + 0x2C0U, name);
  WriteLe16(bytes, base_offset + 0x3C4U, role);
}
// NOLINTEND(bugprone-easily-swappable-parameters)

std::vector<std::uint8_t> MakeDirectFixture(const FixtureMode mode = FixtureMode::kCurrentMapping,
                                            const std::string_view volume_name = "Orchard Data",
                                            const std::uint64_t incompatible_features = 0x1U,
                                            const std::uint16_t role = 0x0040U) {
  std::vector<std::uint8_t> bytes(
      static_cast<std::vector<std::uint8_t>::size_type>(kApfsBlockSize) * 8U, 0U);
  WriteNxSuperblock(bytes, 0U, 1U, 8U);
  WriteNxSuperblock(bytes, kApfsBlockSize, kCurrentCheckpointXid, 8U);
  WriteOmapSuperblock(bytes, kApfsBlockSize * kOmapObjectBlock, kOmapObjectBlock,
                      kCurrentCheckpointXid, kOmapRootBlock);
  WriteOmapRootNode(bytes, kApfsBlockSize * kOmapRootBlock, kOmapRootBlock, kCurrentCheckpointXid,
                    kVolumeObjectId, 20U, kOmapLeafBlock);
  WriteOmapLeafNode(
      bytes, kApfsBlockSize * kOmapLeafBlock, kOmapLeafBlock, kCurrentCheckpointXid,
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
              .flags = mode == FixtureMode::kDeletedLatestMapping ? orchard::apfs::kOmapValueDeleted
                                                                  : 0U,
              .physical_block = kCurrentVolumeBlock,
          },
      });
  WriteVolumeSuperblock(bytes, kApfsBlockSize * kLegacyVolumeBlock, kVolumeObjectId, 20U, 0x1U,
                        0x0040U, "Legacy Data");
  WriteVolumeSuperblock(bytes, kApfsBlockSize * kCurrentVolumeBlock, kVolumeObjectId,
                        kCurrentCheckpointXid, incompatible_features, role, volume_name);
  return bytes;
}

std::vector<std::uint8_t> MakeGptFixture() {
  constexpr std::uint32_t logical_block_size = 512U;
  constexpr std::uint64_t first_lba = 40U;
  constexpr std::uint64_t last_lba = 103U;
  std::vector<std::uint8_t> bytes(
      static_cast<std::vector<std::uint8_t>::size_type>(logical_block_size) * 256U, 0U);

  WriteAscii(bytes, logical_block_size, "EFI PART");
  WriteLe32(bytes, logical_block_size + 8U, 0x00010000U);
  WriteLe32(bytes, logical_block_size + 12U, 92U);
  WriteLe64(bytes, logical_block_size + 24U, 1U);
  WriteLe64(bytes, logical_block_size + 32U, 255U);
  WriteLe64(bytes, logical_block_size + 40U, 34U);
  WriteLe64(bytes, logical_block_size + 48U, 200U);
  WriteLe64(bytes, logical_block_size + 72U, 2U);
  WriteLe32(bytes, logical_block_size + 80U, 1U);
  WriteLe32(bytes, logical_block_size + 84U, 128U);

  const auto partition_offset = logical_block_size * 2U;
  const std::array<std::uint8_t, 16> apfs_guid{0xEF, 0x57, 0x34, 0x7C, 0x00, 0x00, 0xAA, 0x11,
                                               0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC};
  std::copy(apfs_guid.begin(), apfs_guid.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(partition_offset));
  WriteRawUuid(bytes, partition_offset + 16U,
               std::array<std::uint8_t, 16>{0x91, 0x92, 0x93, 0x94, 0xA0, 0xA1, 0xA2, 0xA3, 0xB0,
                                            0xB1, 0xB2, 0xB3, 0xC0, 0xC1, 0xC2, 0xC3});
  WriteLe64(bytes, partition_offset + 32U, first_lba);
  WriteLe64(bytes, partition_offset + 40U, last_lba);
  WriteUtf16Le(bytes, partition_offset + 56U, "Orchard GPT");

  const auto container_offset = static_cast<std::size_t>(first_lba * logical_block_size);
  const auto container_bytes = MakeDirectFixture(FixtureMode::kCurrentMapping, "GPT Data");
  std::copy(container_bytes.begin(), container_bytes.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(container_offset));
  return bytes;
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

  const auto root_object_result = object_reader.ReadPhysicalObject(kOmapRootBlock);
  ORCHARD_TEST_REQUIRE(root_object_result.ok());
  const auto root_node_result =
      orchard::apfs::ParseNode(root_object_result.value().view(), kApfsBlockSize);
  ORCHARD_TEST_REQUIRE(root_node_result.ok());
  ORCHARD_TEST_REQUIRE(root_node_result.value().is_root());
  ORCHARD_TEST_REQUIRE(!root_node_result.value().is_leaf());
  ORCHARD_TEST_REQUIRE(root_node_result.value().fixed_key_size() == 16U);
  ORCHARD_TEST_REQUIRE(root_node_result.value().fixed_value_size() == 8U);

  const auto root_record_result = root_node_result.value().RecordAt(0U);
  ORCHARD_TEST_REQUIRE(root_record_result.ok());
  const auto child_block_result =
      orchard::apfs::ParseChildBlockIndex(root_record_result.value().value);
  ORCHARD_TEST_REQUIRE(child_block_result.ok());
  ORCHARD_TEST_REQUIRE(child_block_result.value() == kOmapLeafBlock);

  const auto leaf_object_result = object_reader.ReadPhysicalObject(kOmapLeafBlock);
  ORCHARD_TEST_REQUIRE(leaf_object_result.ok());
  const auto leaf_node_result =
      orchard::apfs::ParseNode(leaf_object_result.value().view(), kApfsBlockSize);
  ORCHARD_TEST_REQUIRE(leaf_node_result.ok());
  ORCHARD_TEST_REQUIRE(!leaf_node_result.value().is_root());
  ORCHARD_TEST_REQUIRE(leaf_node_result.value().is_leaf());
  ORCHARD_TEST_REQUIRE(leaf_node_result.value().fixed_key_size() == 16U);
  ORCHARD_TEST_REQUIRE(leaf_node_result.value().fixed_value_size() == 16U);

  const auto first_leaf_record_result = leaf_node_result.value().RecordAt(0U);
  ORCHARD_TEST_REQUIRE(first_leaf_record_result.ok());
  const auto first_key_result = orchard::apfs::ParseOmapKey(first_leaf_record_result.value().key);
  ORCHARD_TEST_REQUIRE(first_key_result.ok());
  ORCHARD_TEST_REQUIRE(first_key_result.value().oid == kVolumeObjectId);
  ORCHARD_TEST_REQUIRE(first_key_result.value().xid == 20U);
}

void OmapLookupExactAndOlderFallback() {
  auto reader = orchard::blockio::MakeMemoryReader(MakeDirectFixture(), "omap-fixture");
  orchard::apfs::PhysicalObjectReader object_reader(*reader, 0U, kApfsBlockSize);

  const auto resolver_result = orchard::apfs::OmapResolver::Load(object_reader, kOmapObjectBlock);
  ORCHARD_TEST_REQUIRE(resolver_result.ok());

  const auto exact_result =
      resolver_result.value().ResolveOidToBlock(kVolumeObjectId, kCurrentCheckpointXid);
  ORCHARD_TEST_REQUIRE(exact_result.ok());
  ORCHARD_TEST_REQUIRE(exact_result.value() == kCurrentVolumeBlock);

  const auto fallback_result = resolver_result.value().ResolveOidToBlock(kVolumeObjectId, 30U);
  ORCHARD_TEST_REQUIRE(fallback_result.ok());
  ORCHARD_TEST_REQUIRE(fallback_result.value() == kLegacyVolumeBlock);

  const auto missing_result =
      resolver_result.value().ResolveOidToBlock(999U, kCurrentCheckpointXid);
  ORCHARD_TEST_REQUIRE(!missing_result.ok());
  ORCHARD_TEST_REQUIRE(missing_result.error().code == orchard::blockio::ErrorCode::kNotFound);
}

void OmapLookupRejectsDeletedMapping() {
  auto reader = orchard::blockio::MakeMemoryReader(
      MakeDirectFixture(FixtureMode::kDeletedLatestMapping), "omap-deleted-fixture");
  orchard::apfs::PhysicalObjectReader object_reader(*reader, 0U, kApfsBlockSize);

  const auto resolver_result = orchard::apfs::OmapResolver::Load(object_reader, kOmapObjectBlock);
  ORCHARD_TEST_REQUIRE(resolver_result.ok());

  const auto deleted_result = resolver_result.value().ResolveOidToBlock(kVolumeObjectId, 50U);
  ORCHARD_TEST_REQUIRE(!deleted_result.ok());
  ORCHARD_TEST_REQUIRE(deleted_result.error().code == orchard::blockio::ErrorCode::kNotFound);

  const auto fallback_before_delete =
      resolver_result.value().ResolveOidToBlock(kVolumeObjectId, 30U);
  ORCHARD_TEST_REQUIRE(fallback_before_delete.ok());
  ORCHARD_TEST_REQUIRE(fallback_before_delete.value() == kLegacyVolumeBlock);
}

void DiscoversDirectContainerAndCheckpoint() {
  auto reader = orchard::blockio::MakeMemoryReader(MakeDirectFixture(), "direct-fixture");
  const auto result = orchard::apfs::Discover(*reader);

  ORCHARD_TEST_REQUIRE(result.ok());
  ORCHARD_TEST_REQUIRE(result.value().layout == orchard::apfs::LayoutKind::kDirectContainer);
  ORCHARD_TEST_REQUIRE(result.value().containers.size() == 1U);

  const auto& container = result.value().containers[0];
  ORCHARD_TEST_REQUIRE(container.block_size == kApfsBlockSize);
  ORCHARD_TEST_REQUIRE(container.selected_checkpoint.xid == kCurrentCheckpointXid);
  ORCHARD_TEST_REQUIRE(container.selected_checkpoint.source ==
                       orchard::apfs::CheckpointSource::kCheckpointDescriptorArea);
  ORCHARD_TEST_REQUIRE(container.volume_object_ids.size() == 1U);
  ORCHARD_TEST_REQUIRE(container.volumes_resolved_via_omap);
  ORCHARD_TEST_REQUIRE(container.volumes.size() == 1U);
  ORCHARD_TEST_REQUIRE(container.volumes[0].name == "Orchard Data");
  ORCHARD_TEST_REQUIRE(container.volumes[0].role_names.size() == 1U);
  ORCHARD_TEST_REQUIRE(container.volumes[0].role_names[0] == "data");
  ORCHARD_TEST_REQUIRE(container.volumes[0].case_insensitive);
}

void DiscoversGptWrappedContainer() {
  auto reader = orchard::blockio::MakeMemoryReader(MakeGptFixture(), "gpt-fixture");
  const auto result = orchard::apfs::Discover(*reader);

  ORCHARD_TEST_REQUIRE(result.ok());
  ORCHARD_TEST_REQUIRE(result.value().layout == orchard::apfs::LayoutKind::kGuidPartitionTable);
  const auto gpt_block_size = result.value().gpt_block_size;
  ORCHARD_TEST_REQUIRE(gpt_block_size.has_value());
  const auto gpt_block_size_value = gpt_block_size.value_or(0U);
  ORCHARD_TEST_REQUIRE(gpt_block_size_value == 512U);
  ORCHARD_TEST_REQUIRE(result.value().partitions.size() == 1U);
  ORCHARD_TEST_REQUIRE(result.value().partitions[0].is_apfs_partition);
  ORCHARD_TEST_REQUIRE(result.value().containers.size() == 1U);
  const auto partition = result.value().containers[0].partition;
  ORCHARD_TEST_REQUIRE(partition.has_value());
  std::string partition_name;
  if (partition.has_value()) {
    partition_name = partition->name;
  }
  ORCHARD_TEST_REQUIRE(partition_name == "Orchard GPT");
  ORCHARD_TEST_REQUIRE(result.value().containers[0].volumes_resolved_via_omap);
  ORCHARD_TEST_REQUIRE(result.value().containers[0].volumes.size() == 1U);
  ORCHARD_TEST_REQUIRE(result.value().containers[0].volumes[0].name == "GPT Data");
}

void InspectTargetUsesRealReaderPath() {
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
  ORCHARD_TEST_REQUIRE(result.report.layout == orchard::apfs::LayoutKind::kDirectContainer);
  ORCHARD_TEST_REQUIRE(result.report.containers.size() == 1U);
  ORCHARD_TEST_REQUIRE(result.report.containers[0].volumes_resolved_via_omap);
  ORCHARD_TEST_REQUIRE(result.report.containers[0].volumes.size() == 1U);

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
      {"InspectTargetUsesRealReaderPath", &InspectTargetUsesRealReaderPath},
  });
}
