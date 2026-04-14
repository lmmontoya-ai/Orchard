#include "orchard/apfs/inspection.h"

#include <algorithm>
#include <string>
#include <utility>

#include "orchard/apfs/file_read.h"
#include "orchard/apfs/link_read.h"
#include "orchard/apfs/object.h"
#include "orchard/apfs/omap.h"
#include "orchard/apfs/path_lookup.h"
#include "orchard/apfs/volume.h"
#include "orchard/blockio/reader.h"

namespace orchard::apfs {
namespace {

InspectionResult MakeStatusOnly(const InspectionStatus status, std::string note) {
  InspectionResult result;
  result.status = status;
  result.notes.push_back(std::move(note));
  return result;
}

InspectionStatus MapErrorToStatus(const blockio::ErrorCode code) noexcept {
  switch (code) {
  case blockio::ErrorCode::kReadFailed:
  case blockio::ErrorCode::kShortRead:
  case blockio::ErrorCode::kOutOfRange:
  case blockio::ErrorCode::kIoctlFailed:
    return InspectionStatus::kReadFailed;
  case blockio::ErrorCode::kInvalidFormat:
  case blockio::ErrorCode::kCorruptData:
    return InspectionStatus::kParseFailed;
  case blockio::ErrorCode::kNotFound:
  case blockio::ErrorCode::kAccessDenied:
  case blockio::ErrorCode::kOpenFailed:
  case blockio::ErrorCode::kUnsupportedTarget:
  case blockio::ErrorCode::kInvalidArgument:
  case blockio::ErrorCode::kNotImplemented:
    return InspectionStatus::kOpenFailed;
  }

  return InspectionStatus::kOpenFailed;
}

std::string ToHexPreview(const std::span<const std::uint8_t> bytes) {
  static constexpr char kHexDigits[] = "0123456789ABCDEF";
  std::string text;
  text.reserve(bytes.size() * 2U);

  for (const auto value : bytes) {
    text.push_back(kHexDigits[(value >> 4U) & 0x0FU]);
    text.push_back(kHexDigits[value & 0x0FU]);
  }

  return text;
}

std::string ToUtf8Preview(const std::span<const std::uint8_t> bytes) {
  std::string text;
  text.reserve(bytes.size());
  for (const auto value : bytes) {
    const auto is_ascii_printable = value >= 0x20U && value <= 0x7EU;
    const auto is_ascii_whitespace = value == '\n' || value == '\r' || value == '\t';
    if (is_ascii_printable || is_ascii_whitespace) {
      text.push_back(static_cast<char>(value));
    } else {
      text.push_back('.');
    }
  }
  return text;
}

void PopulateDirectorySamples(VolumeInfo& volume,
                              const std::vector<DirectoryEntryRecord>& entries) {
  volume.root_entries.clear();
  const auto sample_count = std::min<std::size_t>(entries.size(), 8U);
  volume.root_entries.reserve(sample_count);
  for (std::size_t index = 0; index < sample_count; ++index) {
    volume.root_entries.push_back(DirectoryEntrySample{
        .name = entries[index].key.name,
        .inode_id = entries[index].file_id,
        .kind = std::string(ToString(entries[index].kind)),
    });
  }
}

void PopulateFileProbes(VolumeInfo& volume, const VolumeContext& context,
                        const std::vector<DirectoryEntryRecord>& entries) {
  volume.root_file_probes.clear();

  for (const auto& entry : entries) {
    if (entry.kind == InodeKind::kDirectory) {
      continue;
    }

    auto metadata_result = GetFileMetadata(context, entry.file_id);
    if (!metadata_result.ok()) {
      volume.notes.push_back(metadata_result.error().message);
      continue;
    }

    std::optional<std::string> symlink_target;
    std::string preview_utf8;
    std::string preview_hex;
    if (entry.kind == InodeKind::kSymlink) {
      auto target_result = ReadSymlinkTarget(context, entry.file_id);
      if (!target_result.ok()) {
        volume.notes.push_back(target_result.error().message);
        continue;
      }
      symlink_target = std::move(target_result.value());
      preview_utf8 = *symlink_target;
      preview_hex = ToHexPreview(std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(symlink_target->data()), symlink_target->size()));
    } else {
      const auto preview_size = static_cast<std::size_t>(
          std::min<std::uint64_t>(metadata_result.value().logical_size, 24U));
      auto bytes_result = ReadFileRange(context, FileReadRequest{
                                                     .inode_id = entry.file_id,
                                                     .offset = 0U,
                                                     .size = preview_size,
                                                 });
      if (!bytes_result.ok()) {
        volume.notes.push_back(bytes_result.error().message);
        continue;
      }
      preview_utf8 = ToUtf8Preview(bytes_result.value());
      preview_hex = ToHexPreview(bytes_result.value());
    }

    std::vector<std::string> aliases;
    for (const auto& other_entry : entries) {
      if (other_entry.file_id == entry.file_id && other_entry.key.name != entry.key.name) {
        aliases.push_back("/" + other_entry.key.name);
      }
    }

    volume.root_file_probes.push_back(FileProbeInfo{
        .path = "/" + entry.key.name,
        .inode_id = entry.file_id,
        .size_bytes = metadata_result.value().logical_size,
        .kind = std::string(ToString(entry.kind)),
        .link_count = metadata_result.value().link_count,
        .compression = std::string(ToString(metadata_result.value().compression.kind)),
        .sparse = metadata_result.value().sparse,
        .symlink_target = std::move(symlink_target),
        .aliases = std::move(aliases),
        .preview_utf8 = std::move(preview_utf8),
        .preview_hex = std::move(preview_hex),
    });

    if (volume.root_file_probes.size() >= 8U) {
      break;
    }
  }
}

void EnrichVolumeInfo(const blockio::Reader& reader, const ContainerInfo& container,
                      VolumeInfo& volume) {
  PhysicalObjectReader object_reader(reader, container.byte_offset, container.block_size);
  auto container_omap_result = OmapResolver::Load(object_reader, container.omap_oid);
  if (!container_omap_result.ok()) {
    volume.notes.push_back(container_omap_result.error().message);
    return;
  }

  auto volume_result =
      VolumeContext::Load(reader, container, volume, container_omap_result.value());
  if (!volume_result.ok()) {
    volume.notes.push_back(volume_result.error().message);
    return;
  }

  auto root_entries_result = ListDirectory(volume_result.value(), "/");
  if (!root_entries_result.ok()) {
    volume.notes.push_back(root_entries_result.error().message);
    return;
  }

  PopulateDirectorySamples(volume, root_entries_result.value());
  PopulateFileProbes(volume, volume_result.value(), root_entries_result.value());
}

void EnrichInspectionReport(const blockio::Reader& reader, DiscoveryReport& report) {
  for (auto& container : report.containers) {
    for (auto& volume : container.volumes) {
      EnrichVolumeInfo(reader, container, volume);
    }
  }
}

} // namespace

InspectionResult InspectTarget(const blockio::InspectionTargetInfo& target_info) {
  switch (target_info.kind) {
  case blockio::TargetKind::kMissing:
    return MakeStatusOnly(InspectionStatus::kMissingTarget,
                          "Target path does not exist or could not be resolved.");
  case blockio::TargetKind::kDirectory:
    return MakeStatusOnly(InspectionStatus::kUnsupportedTarget,
                          "Directories are not valid APFS inspection targets.");
  case blockio::TargetKind::kUnknown:
    return MakeStatusOnly(InspectionStatus::kUnsupportedTarget,
                          "Target kind is not supported by the inspection path.");
  case blockio::TargetKind::kRegularFile:
  case blockio::TargetKind::kRawDevice:
    break;
  }

  InspectionResult result;

  auto reader_result = blockio::OpenReader(target_info);
  if (!reader_result.ok()) {
    result.status = InspectionStatus::kOpenFailed;
    result.error = reader_result.error();
    result.notes.push_back(reader_result.error().message);
    return result;
  }

  auto reader = std::move(reader_result).value();
  result.reader_backend = std::string(reader->backend_name());

  auto size_result = reader->size_bytes();
  if (size_result.ok()) {
    result.reader_size_bytes = size_result.value();
  } else {
    result.notes.push_back("Reader size is not available; discovery will use bounded scans.");
  }

  auto discovery_result = Discover(*reader);
  if (!discovery_result.ok()) {
    result.status = MapErrorToStatus(discovery_result.error().code);
    result.error = discovery_result.error();
    result.notes.push_back(discovery_result.error().message);
    return result;
  }

  result.report = std::move(discovery_result.value());
  EnrichInspectionReport(*reader, result.report);
  result.notes.insert(result.notes.end(), result.report.notes.begin(), result.report.notes.end());
  result.status = result.report.containers.empty() ? InspectionStatus::kNoApfsContainer
                                                   : InspectionStatus::kSuccess;
  return result;
}

std::string_view ToString(const InspectionStatus status) noexcept {
  switch (status) {
  case InspectionStatus::kMissingTarget:
    return "missing_target";
  case InspectionStatus::kUnsupportedTarget:
    return "unsupported_target";
  case InspectionStatus::kOpenFailed:
    return "open_failed";
  case InspectionStatus::kReadFailed:
    return "read_failed";
  case InspectionStatus::kParseFailed:
    return "parse_failed";
  case InspectionStatus::kNoApfsContainer:
    return "no_apfs_container";
  case InspectionStatus::kSuccess:
    return "success";
  }

  return "unsupported_target";
}

} // namespace orchard::apfs
