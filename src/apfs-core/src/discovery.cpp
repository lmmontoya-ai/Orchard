#include "orchard/apfs/discovery.h"

#include <algorithm>
#include <array>
#include <optional>
#include <sstream>

#include "orchard/apfs/object.h"
#include "orchard/apfs/omap.h"
#include "orchard/apfs/probe.h"

namespace orchard::apfs {
namespace {

constexpr std::array<std::uint8_t, 8> kGptSignature{
    static_cast<std::uint8_t>('E'), static_cast<std::uint8_t>('F'), static_cast<std::uint8_t>('I'),
    static_cast<std::uint8_t>(' '), static_cast<std::uint8_t>('P'), static_cast<std::uint8_t>('A'),
    static_cast<std::uint8_t>('R'), static_cast<std::uint8_t>('T'),
};

constexpr std::array<std::uint8_t, 16> kApfsPartitionTypeGuid{
    0xEF, 0x57, 0x34, 0x7C, 0x00, 0x00, 0xAA, 0x11, 0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC,
};

constexpr std::uint32_t kGptMinimumEntrySize = 128U;
constexpr std::uint32_t kGptMaximumEntryCount = 256U;
constexpr std::uint64_t kMaximumGptEntryTableBytes = 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumCheckpointScanBlocks = 2048ULL;

struct GptHeader {
  std::uint32_t logical_block_size = 0;
  std::uint64_t partition_entries_lba = 0;
  std::uint32_t partition_entry_count = 0;
  std::uint32_t partition_entry_size = 0;
};

struct CandidateContainer {
  ParsedNxSuperblock superblock;
  std::uint64_t block_index = 0;
  CheckpointSource source = CheckpointSource::kMainSuperblock;
};

[[nodiscard]] std::optional<std::uint64_t> TryGetReaderSize(const blockio::Reader& reader) {
  auto size_result = reader.size_bytes();
  if (!size_result.ok()) {
    return std::nullopt;
  }

  return size_result.value();
}

[[nodiscard]] VolumeInfo MakeVolumeInfo(const ParsedVolumeSuperblock& parsed) {
  return VolumeInfo{
      .object_id = parsed.object_id,
      .filesystem_index = parsed.filesystem_index,
      .name = parsed.name,
      .uuid = parsed.uuid,
      .features = parsed.features,
      .role = parsed.role,
      .role_names = parsed.role_names,
      .case_insensitive = parsed.case_insensitive,
      .sealed = parsed.sealed,
  };
}

blockio::Result<std::vector<VolumeInfo>>
ResolveVolumes(const blockio::Reader& reader, const std::uint64_t byte_offset,
               const CandidateContainer& selected_container) {
  PhysicalObjectReader object_reader(reader, byte_offset, selected_container.superblock.block_size);
  auto omap_result = OmapResolver::Load(object_reader, selected_container.superblock.omap_oid);
  if (!omap_result.ok()) {
    return omap_result.error();
  }

  std::vector<VolumeInfo> volumes;
  volumes.reserve(selected_container.superblock.volume_object_ids.size());

  for (const auto volume_object_id : selected_container.superblock.volume_object_ids) {
    auto block_result =
        omap_result.value().ResolveOidToBlock(volume_object_id, selected_container.superblock.xid);
    if (!block_result.ok()) {
      std::ostringstream message;
      message << "Failed to resolve APFS volume object id " << volume_object_id
              << " through the container object map: " << block_result.error().message;
      return MakeApfsError(block_result.error().code, message.str());
    }

    auto object_result = object_reader.ReadPhysicalObject(block_result.value());
    if (!object_result.ok()) {
      std::ostringstream message;
      message << "Failed to read APFS volume superblock block " << block_result.value()
              << " for object id " << volume_object_id << ": " << object_result.error().message;
      return MakeApfsError(object_result.error().code, message.str());
    }

    auto volume_result = ParseVolumeSuperblock(object_result.value().view());
    if (!volume_result.ok()) {
      return volume_result.error();
    }

    if (volume_result.value().object_id != volume_object_id) {
      std::ostringstream message;
      message << "Container object map resolved APFS volume object id " << volume_object_id
              << " to block " << block_result.value()
              << ", but the volume superblock advertises object id "
              << volume_result.value().object_id << ".";
      return MakeApfsError(blockio::ErrorCode::kCorruptData, message.str());
    }

    volumes.push_back(MakeVolumeInfo(volume_result.value()));
  }

  return volumes;
}

blockio::Result<std::optional<GptHeader>> TryReadGptHeader(const blockio::Reader& reader,
                                                           const std::uint32_t logical_block_size) {
  const auto reader_size = TryGetReaderSize(reader);
  const auto header_offset = static_cast<std::uint64_t>(logical_block_size);
  if (reader_size.has_value() && *reader_size < header_offset + 92U) {
    return std::optional<GptHeader>{};
  }

  auto header_bytes_result = blockio::ReadExact(
      reader, blockio::ReadRequest{.offset = header_offset, .size = logical_block_size});
  if (!header_bytes_result.ok()) {
    if (header_bytes_result.error().code == blockio::ErrorCode::kShortRead) {
      return std::optional<GptHeader>{};
    }
    return header_bytes_result.error();
  }

  const auto& header_bytes = header_bytes_result.value();
  if (!std::equal(kGptSignature.begin(), kGptSignature.end(), header_bytes.begin(),
                  header_bytes.begin() + static_cast<std::ptrdiff_t>(kGptSignature.size()))) {
    return std::optional<GptHeader>{};
  }

  const auto header_size = ReadLe32(header_bytes, 12U);
  if (header_size < 92U || header_size > logical_block_size) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "GPT header size is outside the supported range.");
  }

  const auto partition_entry_count = ReadLe32(header_bytes, 80U);
  const auto partition_entry_size = ReadLe32(header_bytes, 84U);
  if (partition_entry_size < kGptMinimumEntrySize ||
      partition_entry_count > kGptMaximumEntryCount) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "GPT header advertises unsupported partition-entry metadata.");
  }

  const auto entry_table_bytes =
      static_cast<std::uint64_t>(partition_entry_count) * partition_entry_size;
  if (entry_table_bytes > kMaximumGptEntryTableBytes) {
    return MakeApfsError(blockio::ErrorCode::kCorruptData,
                         "GPT partition-entry table exceeds the supported scan budget.");
  }

  return std::optional<GptHeader>(GptHeader{
      .logical_block_size = logical_block_size,
      .partition_entries_lba = ReadLe64(header_bytes, 72U),
      .partition_entry_count = partition_entry_count,
      .partition_entry_size = partition_entry_size,
  });
}

blockio::Result<std::vector<PartitionInfo>> ReadGptPartitions(const blockio::Reader& reader,
                                                              const GptHeader& header) {
  const auto table_offset = header.partition_entries_lba * header.logical_block_size;
  const auto table_bytes = static_cast<std::size_t>(
      static_cast<std::uint64_t>(header.partition_entry_count) * header.partition_entry_size);
  auto entries_result =
      blockio::ReadExact(reader, blockio::ReadRequest{.offset = table_offset, .size = table_bytes});
  if (!entries_result.ok()) {
    return entries_result.error();
  }

  std::vector<PartitionInfo> partitions;
  partitions.reserve(header.partition_entry_count);

  const std::span<const std::uint8_t> entries = entries_result.value();
  for (std::uint32_t index = 0; index < header.partition_entry_count; ++index) {
    const auto entry_offset = static_cast<std::size_t>(index) * header.partition_entry_size;
    const auto entry = entries.subspan(entry_offset, header.partition_entry_size);

    if (std::all_of(entry.begin(), entry.begin() + 16,
                    [](const std::uint8_t value) { return value == 0U; })) {
      continue;
    }

    std::array<std::uint8_t, 16> type_guid_bytes{};
    std::copy_n(entry.begin(), 16, type_guid_bytes.begin());

    std::array<std::uint8_t, 16> unique_guid_bytes{};
    std::copy_n(entry.begin() + 16, 16, unique_guid_bytes.begin());

    const auto first_lba = ReadLe64(entry, 32U);
    const auto last_lba = ReadLe64(entry, 40U);
    if (last_lba < first_lba) {
      return MakeApfsError(blockio::ErrorCode::kCorruptData,
                           "GPT partition has an invalid LBA range.");
    }

    partitions.push_back(PartitionInfo{
        .type_guid = FormatGuidFromGptBytes(type_guid_bytes),
        .unique_guid = FormatGuidFromGptBytes(unique_guid_bytes),
        .name = DecodeUtf16LeName(std::span(entry.begin() + 56, 72U)),
        .first_lba = first_lba,
        .last_lba = last_lba,
        .byte_offset = first_lba * header.logical_block_size,
        .byte_length = ((last_lba - first_lba) + 1U) * header.logical_block_size,
        .is_apfs_partition = std::equal(type_guid_bytes.begin(), type_guid_bytes.end(),
                                        kApfsPartitionTypeGuid.begin()),
    });
  }

  return partitions;
}

blockio::Result<std::optional<ContainerInfo>>
TryReadContainerAt(const blockio::Reader& reader, const std::uint64_t byte_offset,
                   const LayoutKind source_layout, const std::optional<PartitionInfo>& partition) {
  const auto reader_size = TryGetReaderSize(reader);
  if (reader_size.has_value() && *reader_size < byte_offset + kApfsMinimumBlockSize) {
    return std::optional<ContainerInfo>{};
  }

  auto initial_block_result = blockio::ReadExact(
      reader, blockio::ReadRequest{.offset = byte_offset, .size = kApfsMinimumBlockSize});
  if (!initial_block_result.ok()) {
    if (initial_block_result.error().code == blockio::ErrorCode::kShortRead) {
      return std::optional<ContainerInfo>{};
    }
    return initial_block_result.error();
  }

  if (!ProbeContainerMagic(initial_block_result.value())) {
    return std::optional<ContainerInfo>{};
  }

  auto initial_parse_result = ParseNxSuperblock(initial_block_result.value());
  if (!initial_parse_result.ok()) {
    return initial_parse_result.error();
  }

  const auto block_size = initial_parse_result.value().block_size;
  auto selected_candidate = CandidateContainer{
      .superblock = initial_parse_result.value(),
      .block_index = 0,
      .source = CheckpointSource::kMainSuperblock,
  };

  const auto max_candidate_blocks = std::min<std::uint64_t>(
      selected_candidate.superblock.checkpoint_descriptor_blocks, kMaximumCheckpointScanBlocks);

  for (std::uint64_t block_index = 0; block_index < max_candidate_blocks; ++block_index) {
    const auto candidate_block =
        selected_candidate.superblock.checkpoint_descriptor_base + block_index;
    const auto candidate_offset = byte_offset + (candidate_block * block_size);
    auto candidate_bytes_result = blockio::ReadExact(
        reader, blockio::ReadRequest{.offset = candidate_offset, .size = block_size});
    if (!candidate_bytes_result.ok()) {
      continue;
    }

    if (!ProbeContainerMagic(candidate_bytes_result.value())) {
      continue;
    }

    auto candidate_parse_result = ParseNxSuperblock(candidate_bytes_result.value());
    if (!candidate_parse_result.ok()) {
      continue;
    }

    if (candidate_parse_result.value().xid > selected_candidate.superblock.xid) {
      selected_candidate = CandidateContainer{
          .superblock = candidate_parse_result.value(),
          .block_index = candidate_block,
          .source = CheckpointSource::kCheckpointDescriptorArea,
      };
    }
  }

  ContainerInfo container;
  container.source_layout = source_layout;
  container.byte_offset = byte_offset;
  container.byte_length = static_cast<std::uint64_t>(selected_candidate.superblock.block_size) *
                          selected_candidate.superblock.block_count;
  container.block_size = selected_candidate.superblock.block_size;
  container.block_count = selected_candidate.superblock.block_count;
  container.uuid = selected_candidate.superblock.uuid;
  container.features = selected_candidate.superblock.features;
  container.selected_checkpoint.block_index = selected_candidate.block_index;
  container.selected_checkpoint.xid = selected_candidate.superblock.xid;
  container.selected_checkpoint.source = selected_candidate.source;
  container.spaceman_oid = selected_candidate.superblock.spaceman_oid;
  container.omap_oid = selected_candidate.superblock.omap_oid;
  container.reaper_oid = selected_candidate.superblock.reaper_oid;
  container.volume_object_ids = selected_candidate.superblock.volume_object_ids;
  container.partition = partition;

  auto volumes_result = ResolveVolumes(reader, byte_offset, selected_candidate);
  if (!volumes_result.ok()) {
    return volumes_result.error();
  }

  container.volumes = std::move(volumes_result.value());
  container.volumes_resolved_via_omap = true;
  container.notes.push_back(
      "Resolved referenced volume superblocks through the container object map.");

  return std::optional<ContainerInfo>(std::move(container));
}

} // namespace

blockio::Result<DiscoveryReport> Discover(const blockio::Reader& reader) {
  DiscoveryReport report;

  auto direct_container_result =
      TryReadContainerAt(reader, 0U, LayoutKind::kDirectContainer, std::nullopt);
  if (!direct_container_result.ok()) {
    return direct_container_result.error();
  }

  auto direct_container = std::move(direct_container_result.value());
  if (direct_container.has_value()) {
    report.layout = LayoutKind::kDirectContainer;
    report.containers.push_back(std::move(direct_container.value()));
    report.notes.push_back("Detected a direct APFS container image at byte offset 0.");
    return report;
  }

  for (const auto logical_block_size : {512U, 4096U}) {
    auto header_result = TryReadGptHeader(reader, logical_block_size);
    if (!header_result.ok()) {
      return header_result.error();
    }

    auto header = header_result.value();
    if (!header.has_value()) {
      continue;
    }

    auto partitions_result = ReadGptPartitions(reader, header.value());
    if (!partitions_result.ok()) {
      return partitions_result.error();
    }

    report.layout = LayoutKind::kGuidPartitionTable;
    report.gpt_block_size = header->logical_block_size;
    report.partitions = partitions_result.value();

    for (const auto& partition : report.partitions) {
      if (!partition.is_apfs_partition) {
        continue;
      }

      auto container_result = TryReadContainerAt(reader, partition.byte_offset,
                                                 LayoutKind::kGuidPartitionTable, partition);
      if (!container_result.ok()) {
        return container_result.error();
      }

      auto container = std::move(container_result.value());
      if (container.has_value()) {
        report.containers.push_back(std::move(container.value()));
      }
    }

    if (report.containers.empty()) {
      report.notes.push_back(
          "GPT was detected, but no APFS container superblock was found in APFS-typed partitions.");
    } else {
      report.notes.push_back("Detected APFS container(s) inside a GPT-partitioned disk image.");
    }
    return report;
  }

  report.notes.push_back(
      "No direct APFS container or supported GPT-partitioned APFS container was detected.");
  return report;
}

std::string_view ToString(const LayoutKind layout) noexcept {
  switch (layout) {
  case LayoutKind::kUnknown:
    return "unknown";
  case LayoutKind::kDirectContainer:
    return "direct_container";
  case LayoutKind::kGuidPartitionTable:
    return "guid_partition_table";
  }

  return "unknown";
}

std::string_view ToString(const CheckpointSource source) noexcept {
  switch (source) {
  case CheckpointSource::kMainSuperblock:
    return "main_superblock";
  case CheckpointSource::kCheckpointDescriptorArea:
    return "checkpoint_descriptor_area";
  }

  return "main_superblock";
}

} // namespace orchard::apfs
