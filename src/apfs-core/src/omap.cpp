#include "orchard/apfs/omap.h"

namespace orchard::apfs {

blockio::Result<OmapSuperblock> ParseOmapSuperblock(const std::span<const std::uint8_t> block) {
  auto header_result = ParseObjectHeader(block);
  if (!header_result.ok()) {
    return header_result.error();
  }

  if (header_result.value().type != kObjectTypeOmap) {
    return MakeApfsError(blockio::ErrorCode::kInvalidFormat,
                         "APFS object is not an object-map superblock.");
  }

  if (!HasRange(block, 0x40U, 24U)) {
    return MakeApfsError(blockio::ErrorCode::kShortRead,
                         "APFS object-map superblock is too small for required fields.");
  }

  OmapSuperblock omap;
  omap.object = header_result.value();
  omap.flags = ReadLe32(block, 0x20U);
  omap.snap_count = ReadLe32(block, 0x24U);
  omap.tree_type = ReadLe32(block, 0x28U);
  omap.snapshot_tree_type = ReadLe32(block, 0x2CU);
  omap.tree_oid = ReadLe64(block, 0x30U);
  omap.snapshot_tree_oid = ReadLe64(block, 0x38U);
  omap.most_recent_snap = ReadLe64(block, 0x40U);
  omap.pending_revert_min = ReadLe64(block, 0x48U);
  omap.pending_revert_max = ReadLe64(block, 0x50U);
  return omap;
}

blockio::Result<OmapKey> ParseOmapKey(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() != 16U) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "APFS omap key does not have the required 16-byte size.");
  }

  return OmapKey{
      .oid = ReadLe64(bytes, 0x00U),
      .xid = ReadLe64(bytes, 0x08U),
  };
}

blockio::Result<OmapValue> ParseOmapValue(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() != 16U) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "APFS omap value does not have the required 16-byte size.");
  }

  return OmapValue{
      .flags = ReadLe32(bytes, 0x00U),
      .size = ReadLe32(bytes, 0x04U),
      .physical_address = ReadLe64(bytes, 0x08U),
  };
}

OmapResolver::OmapResolver(const PhysicalObjectReader& reader, OmapSuperblock superblock)
    : superblock_(std::move(superblock)), walker_(reader) {}

blockio::Result<OmapResolver> OmapResolver::Load(const PhysicalObjectReader& reader,
                                                 const std::uint64_t omap_block_index) {
  auto object_result = reader.ReadPhysicalObject(omap_block_index);
  if (!object_result.ok()) {
    return object_result.error();
  }

  auto omap_result = ParseOmapSuperblock(object_result.value().view());
  if (!omap_result.ok()) {
    return omap_result.error();
  }

  return OmapResolver(reader, omap_result.value());
}

blockio::Result<OmapRecord> OmapResolver::Lookup(const std::uint64_t oid,
                                                 const std::uint64_t xid_limit) const {
  auto compare = [oid, xid_limit](const std::span<const std::uint8_t> key_bytes)
      -> blockio::Result<int> {
    auto key_result = ParseOmapKey(key_bytes);
    if (!key_result.ok()) {
      return key_result.error();
    }

    if (key_result.value().oid != oid) {
      return key_result.value().oid < oid ? -1 : 1;
    }
    if (key_result.value().xid != xid_limit) {
      return key_result.value().xid < xid_limit ? -1 : 1;
    }
    return 0;
  };

  auto record_result = walker_.Find(superblock_.tree_oid, compare);
  if (!record_result.ok()) {
    return record_result.error();
  }
  if (!record_result.value().has_value()) {
    return MakeApfsError(blockio::ErrorCode::kNotFound,
                         "APFS omap did not contain a mapping at or below the requested xid.");
  }

  auto key_result =
      ParseOmapKey(std::span<const std::uint8_t>(record_result.value()->key.data(),
                                                 record_result.value()->key.size()));
  if (!key_result.ok()) {
    return key_result.error();
  }
  if (key_result.value().oid != oid || key_result.value().xid > xid_limit) {
    return MakeApfsError(blockio::ErrorCode::kNotFound,
                         "APFS omap did not resolve the requested object identifier.");
  }

  auto value_result =
      ParseOmapValue(std::span<const std::uint8_t>(record_result.value()->value.data(),
                                                   record_result.value()->value.size()));
  if (!value_result.ok()) {
    return value_result.error();
  }

  if ((value_result.value().flags & kOmapValueDeleted) != 0U) {
    return MakeApfsError(blockio::ErrorCode::kNotFound,
                         "APFS omap resolved the requested object to a deleted tombstone record.");
  }

  return OmapRecord{
      .key = key_result.value(),
      .value = value_result.value(),
  };
}

blockio::Result<std::uint64_t> OmapResolver::ResolveOidToBlock(const std::uint64_t oid,
                                                               const std::uint64_t xid_limit) const {
  auto lookup_result = Lookup(oid, xid_limit);
  if (!lookup_result.ok()) {
    return lookup_result.error();
  }

  return lookup_result.value().value.physical_address;
}

} // namespace orchard::apfs
