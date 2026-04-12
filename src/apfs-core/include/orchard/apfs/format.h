#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "orchard/blockio/result.h"

namespace orchard::apfs {

constexpr std::uint32_t kApfsMinimumBlockSize = 4096U;
constexpr std::uint32_t kApfsMaximumBlockSize = 65536U;

constexpr std::uint32_t kObjectTypeMask = 0x0000FFFFU;
constexpr std::uint32_t kObjectTypeNxSuperblock = 0x00000001U;
constexpr std::uint32_t kObjectTypeBtree = 0x00000002U;
constexpr std::uint32_t kObjectTypeBtreeNode = 0x00000003U;
constexpr std::uint32_t kObjectTypeOmap = 0x0000000BU;
constexpr std::uint32_t kObjectTypeFs = 0x0000000DU;

constexpr std::size_t kApfsObjectHeaderSize = 0x20U;
constexpr std::size_t kBtreeNodeHeaderSize = 0x38U;
constexpr std::size_t kBtreeInfoSize = 0x28U;

constexpr std::uint16_t kBtreeNodeFlagRoot = 0x0001U;
constexpr std::uint16_t kBtreeNodeFlagLeaf = 0x0002U;
constexpr std::uint16_t kBtreeNodeFlagFixedKv = 0x0004U;
constexpr std::uint16_t kBtreeNodeFlagCheckKoffInvalid = 0x8000U;
constexpr std::uint16_t kBtreeNodeFlagMask = 0x8007U;
constexpr std::uint16_t kBtreeOffsetInvalid = 0xFFFFU;

constexpr std::uint32_t kOmapValueDeleted = 0x00000001U;

struct FeatureFlags {
  std::uint64_t compatible = 0;
  std::uint64_t readonly_compatible = 0;
  std::uint64_t incompatible = 0;
  std::vector<std::string> compatible_names;
  std::vector<std::string> readonly_compatible_names;
  std::vector<std::string> incompatible_names;
};

struct ParsedNxSuperblock {
  FeatureFlags features;
  std::uint32_t block_size = 0;
  std::uint64_t block_count = 0;
  std::string uuid;
  std::uint64_t xid = 0;
  std::uint64_t next_xid = 0;
  std::uint32_t checkpoint_descriptor_blocks = 0;
  std::uint64_t checkpoint_descriptor_base = 0;
  std::uint64_t spaceman_oid = 0;
  std::uint64_t omap_oid = 0;
  std::uint64_t reaper_oid = 0;
  std::vector<std::uint64_t> volume_object_ids;
};

struct ParsedVolumeSuperblock {
  std::uint64_t object_id = 0;
  std::uint64_t xid = 0;
  std::uint32_t filesystem_index = 0;
  std::string name;
  std::string uuid;
  FeatureFlags features;
  std::uint16_t role = 0;
  std::vector<std::string> role_names;
  bool case_insensitive = false;
  bool sealed = false;
};

[[nodiscard]] blockio::Error MakeApfsError(blockio::ErrorCode code, std::string message);

[[nodiscard]] bool HasRange(std::span<const std::uint8_t> bytes,
                            std::size_t offset,
                            std::size_t length) noexcept;
[[nodiscard]] std::uint16_t ReadLe16(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept;
[[nodiscard]] std::uint32_t ReadLe32(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept;
[[nodiscard]] std::uint64_t ReadLe64(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept;

[[nodiscard]] std::string FormatRawUuid(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::string FormatGuidFromGptBytes(std::span<const std::uint8_t, 16> bytes);
[[nodiscard]] std::string DecodeUtf16LeName(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::string DecodeUtf8Name(std::span<const std::uint8_t> bytes);
[[nodiscard]] bool IsReasonableApfsBlockSize(std::uint32_t block_size) noexcept;

[[nodiscard]] FeatureFlags MakeContainerFeatures(std::uint64_t compatible,
                                                 std::uint64_t readonly_compatible,
                                                 std::uint64_t incompatible);
[[nodiscard]] FeatureFlags MakeVolumeFeatures(std::uint64_t compatible,
                                              std::uint64_t readonly_compatible,
                                              std::uint64_t incompatible);
[[nodiscard]] std::vector<std::string> DecodeVolumeRoles(std::uint16_t role);

blockio::Result<ParsedNxSuperblock> ParseNxSuperblock(std::span<const std::uint8_t> block);
blockio::Result<ParsedVolumeSuperblock> ParseVolumeSuperblock(std::span<const std::uint8_t> block);

} // namespace orchard::apfs
