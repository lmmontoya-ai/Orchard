#include "orchard/apfs/format.h"

#include <algorithm>
#include <array>
#include <string_view>

#include "orchard/apfs/probe.h"

namespace orchard::apfs {
namespace {

constexpr std::uint64_t kNxIncompatFusion = 0x100ULL;

constexpr std::uint64_t kVolumeIncompatCaseInsensitive = 0x1ULL;
constexpr std::uint64_t kVolumeIncompatDatalessSnaps = 0x2ULL;
constexpr std::uint64_t kVolumeIncompatEncRolled = 0x4ULL;
constexpr std::uint64_t kVolumeIncompatNormalizationInsensitive = 0x8ULL;
constexpr std::uint64_t kVolumeIncompatIncompleteRestore = 0x10ULL;
constexpr std::uint64_t kVolumeIncompatSealed = 0x20ULL;

constexpr std::uint16_t kRoleSystem = 0x0001U;
constexpr std::uint16_t kRoleUser = 0x0002U;
constexpr std::uint16_t kRoleRecovery = 0x0004U;
constexpr std::uint16_t kRoleVm = 0x0008U;
constexpr std::uint16_t kRolePreboot = 0x0010U;
constexpr std::uint16_t kRoleInstaller = 0x0020U;
constexpr std::uint16_t kRoleData = 0x0040U;

} // namespace

blockio::Error MakeApfsError(const blockio::ErrorCode code, std::string message) {
  return blockio::Error{
      .code = code,
      .message = std::move(message),
  };
}

bool HasRange(const std::span<const std::uint8_t> bytes,
              const std::size_t offset,
              const std::size_t length) noexcept {
  return offset <= bytes.size() && length <= bytes.size() - offset;
}

std::uint16_t ReadLe16(const std::span<const std::uint8_t> bytes,
                       const std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(bytes[offset]) |
         (static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t ReadLe32(const std::span<const std::uint8_t> bytes,
                       const std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(ReadLe16(bytes, offset)) |
         (static_cast<std::uint32_t>(ReadLe16(bytes, offset + 2U)) << 16U);
}

std::uint64_t ReadLe64(const std::span<const std::uint8_t> bytes,
                       const std::size_t offset) noexcept {
  return static_cast<std::uint64_t>(ReadLe32(bytes, offset)) |
         (static_cast<std::uint64_t>(ReadLe32(bytes, offset + 4U)) << 32U);
}

std::string FormatRawUuid(const std::span<const std::uint8_t> bytes) {
  static constexpr char kHexDigits[] = "0123456789ABCDEF";

  std::string text;
  text.reserve(36);

  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4U || index == 6U || index == 8U || index == 10U) {
      text.push_back('-');
    }

    text.push_back(kHexDigits[(bytes[index] >> 4U) & 0x0FU]);
    text.push_back(kHexDigits[bytes[index] & 0x0FU]);
  }

  return text;
}

std::string FormatGuidFromGptBytes(const std::span<const std::uint8_t, 16> bytes) {
  std::array<std::uint8_t, 16> canonical{};
  canonical[0] = bytes[3];
  canonical[1] = bytes[2];
  canonical[2] = bytes[1];
  canonical[3] = bytes[0];
  canonical[4] = bytes[5];
  canonical[5] = bytes[4];
  canonical[6] = bytes[7];
  canonical[7] = bytes[6];
  std::copy(bytes.begin() + 8, bytes.end(), canonical.begin() + 8);
  return FormatRawUuid(canonical);
}

std::string DecodeUtf16LeName(const std::span<const std::uint8_t> bytes) {
  std::string name;
  name.reserve(bytes.size() / 2U);

  for (std::size_t offset = 0; offset + 1U < bytes.size(); offset += 2U) {
    const auto code_unit = ReadLe16(bytes, offset);
    if (code_unit == 0U) {
      break;
    }

    if (code_unit <= 0x7FU) {
      name.push_back(static_cast<char>(code_unit));
    } else {
      name.push_back('?');
    }
  }

  return name;
}

std::string DecodeUtf8Name(const std::span<const std::uint8_t> bytes) {
  auto end = std::find(bytes.begin(), bytes.end(), static_cast<std::uint8_t>(0));
  return std::string(bytes.begin(), end);
}

bool IsReasonableApfsBlockSize(const std::uint32_t block_size) noexcept {
  if (block_size < kApfsMinimumBlockSize || block_size > kApfsMaximumBlockSize) {
    return false;
  }

  return (block_size & (block_size - 1U)) == 0U;
}

FeatureFlags MakeContainerFeatures(const std::uint64_t compatible,
                                   const std::uint64_t readonly_compatible,
                                   const std::uint64_t incompatible) {
  FeatureFlags features;
  features.compatible = compatible;
  features.readonly_compatible = readonly_compatible;
  features.incompatible = incompatible;

  if ((incompatible & kNxIncompatFusion) != 0U) {
    features.incompatible_names.emplace_back("fusion");
  }

  return features;
}

FeatureFlags MakeVolumeFeatures(const std::uint64_t compatible,
                                const std::uint64_t readonly_compatible,
                                const std::uint64_t incompatible) {
  FeatureFlags features;
  features.compatible = compatible;
  features.readonly_compatible = readonly_compatible;
  features.incompatible = incompatible;

  if ((incompatible & kVolumeIncompatCaseInsensitive) != 0U) {
    features.incompatible_names.emplace_back("case_insensitive");
  }
  if ((incompatible & kVolumeIncompatDatalessSnaps) != 0U) {
    features.incompatible_names.emplace_back("dataless_snapshots");
  }
  if ((incompatible & kVolumeIncompatEncRolled) != 0U) {
    features.incompatible_names.emplace_back("encryption_rolled");
  }
  if ((incompatible & kVolumeIncompatNormalizationInsensitive) != 0U) {
    features.incompatible_names.emplace_back("normalization_insensitive");
  }
  if ((incompatible & kVolumeIncompatIncompleteRestore) != 0U) {
    features.incompatible_names.emplace_back("incomplete_restore");
  }
  if ((incompatible & kVolumeIncompatSealed) != 0U) {
    features.incompatible_names.emplace_back("sealed");
  }

  return features;
}

std::vector<std::string> DecodeVolumeRoles(const std::uint16_t role) {
  std::vector<std::string> roles;

  if ((role & kRoleSystem) != 0U) {
    roles.emplace_back("system");
  }
  if ((role & kRoleUser) != 0U) {
    roles.emplace_back("user");
  }
  if ((role & kRoleRecovery) != 0U) {
    roles.emplace_back("recovery");
  }
  if ((role & kRoleVm) != 0U) {
    roles.emplace_back("vm");
  }
  if ((role & kRolePreboot) != 0U) {
    roles.emplace_back("preboot");
  }
  if ((role & kRoleInstaller) != 0U) {
    roles.emplace_back("installer");
  }
  if ((role & kRoleData) != 0U) {
    roles.emplace_back("data");
  }

  return roles;
}

blockio::Result<ParsedNxSuperblock> ParseNxSuperblock(const std::span<const std::uint8_t> block) {
  if (!HasRange(block, 0x20U, 4U) || !ProbeContainerMagic(block)) {
    return MakeApfsError(blockio::ErrorCode::kInvalidFormat,
                         "Container superblock magic is missing at the APFS object offset.");
  }

  if (!HasRange(block, 0x3E8U, 0x10U)) {
    return MakeApfsError(blockio::ErrorCode::kShortRead,
                         "Container superblock block is too small for required fields.");
  }

  const auto block_size = ReadLe32(block, 0x24U);
  if (!IsReasonableApfsBlockSize(block_size)) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "Container superblock advertises an invalid APFS block size.");
  }

  ParsedNxSuperblock parsed;
  parsed.features = MakeContainerFeatures(ReadLe64(block, 0x30U),
                                          ReadLe64(block, 0x38U),
                                          ReadLe64(block, 0x40U));
  parsed.block_size = block_size;
  parsed.block_count = ReadLe64(block, 0x28U);
  parsed.uuid = FormatRawUuid(std::span(block.begin() + 0x48U, 16U));
  parsed.xid = ReadLe64(block, 0x10U);
  parsed.next_xid = ReadLe64(block, 0x60U);
  parsed.checkpoint_descriptor_blocks = ReadLe32(block, 0x68U);
  parsed.checkpoint_descriptor_base = ReadLe64(block, 0x70U);
  parsed.spaceman_oid = ReadLe64(block, 0x98U);
  parsed.omap_oid = ReadLe64(block, 0xA0U);
  parsed.reaper_oid = ReadLe64(block, 0xA8U);

  const auto max_file_systems = std::min<std::uint32_t>(ReadLe32(block, 0xB4U), 100U);
  for (std::uint32_t index = 0; index < max_file_systems; ++index) {
    const auto entry_offset = static_cast<std::size_t>(0xB8U + (index * 8U));
    if (!HasRange(block, entry_offset, 8U)) {
      break;
    }

    const auto object_id = ReadLe64(block, entry_offset);
    if (object_id != 0U) {
      parsed.volume_object_ids.push_back(object_id);
    }
  }

  return parsed;
}

blockio::Result<ParsedVolumeSuperblock> ParseVolumeSuperblock(
    const std::span<const std::uint8_t> block) {
  if (!HasRange(block, 0x400U, 8U) || !ProbeVolumeMagic(block)) {
    return MakeApfsError(blockio::ErrorCode::kInvalidFormat,
                         "Volume superblock magic is missing at the APFS object offset.");
  }

  ParsedVolumeSuperblock parsed;
  parsed.object_id = ReadLe64(block, 0x08U);
  parsed.xid = ReadLe64(block, 0x10U);
  parsed.filesystem_index = ReadLe32(block, 0x24U);
  parsed.features = MakeVolumeFeatures(ReadLe64(block, 0x28U),
                                       ReadLe64(block, 0x30U),
                                       ReadLe64(block, 0x38U));
  parsed.uuid = FormatRawUuid(std::span(block.begin() + 0xF0U, 16U));
  parsed.name = DecodeUtf8Name(std::span(block.begin() + 0x2C0U, 256U));
  parsed.role = ReadLe16(block, 0x3C4U);
  parsed.role_names = DecodeVolumeRoles(parsed.role);
  parsed.case_insensitive =
      (parsed.features.incompatible & kVolumeIncompatCaseInsensitive) != 0U;
  parsed.sealed = (parsed.features.incompatible & kVolumeIncompatSealed) != 0U;
  return parsed;
}

} // namespace orchard::apfs
