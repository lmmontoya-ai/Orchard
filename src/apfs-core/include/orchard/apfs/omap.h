#pragma once

#include <cstdint>
#include <span>

#include "orchard/apfs/btree.h"

namespace orchard::apfs {

struct OmapSuperblock {
  ObjectHeader object;
  std::uint32_t flags = 0;
  std::uint32_t snap_count = 0;
  std::uint32_t tree_type = 0;
  std::uint32_t snapshot_tree_type = 0;
  std::uint64_t tree_oid = 0;
  std::uint64_t snapshot_tree_oid = 0;
  std::uint64_t most_recent_snap = 0;
  std::uint64_t pending_revert_min = 0;
  std::uint64_t pending_revert_max = 0;
};

struct OmapKey {
  std::uint64_t oid = 0;
  std::uint64_t xid = 0;
};

struct OmapValue {
  std::uint32_t flags = 0;
  std::uint32_t size = 0;
  std::uint64_t physical_address = 0;
};

struct OmapRecord {
  OmapKey key;
  OmapValue value;
};

blockio::Result<OmapSuperblock> ParseOmapSuperblock(std::span<const std::uint8_t> block);
blockio::Result<OmapKey> ParseOmapKey(std::span<const std::uint8_t> bytes);
blockio::Result<OmapValue> ParseOmapValue(std::span<const std::uint8_t> bytes);

class OmapResolver {
public:
  static blockio::Result<OmapResolver> Load(const PhysicalObjectReader& reader,
                                            std::uint64_t omap_block_index);

  [[nodiscard]] blockio::Result<OmapRecord> Lookup(std::uint64_t oid, std::uint64_t xid_limit) const;
  [[nodiscard]] blockio::Result<std::uint64_t> ResolveOidToBlock(std::uint64_t oid,
                                                                 std::uint64_t xid_limit) const;
  [[nodiscard]] const OmapSuperblock& superblock() const noexcept { return superblock_; }

private:
  OmapResolver(const PhysicalObjectReader& reader, OmapSuperblock superblock);

  OmapSuperblock superblock_;
  BtreeWalker walker_;
};

} // namespace orchard::apfs
