#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "orchard/apfs/discovery.h"
#include "orchard/apfs/inspection.h"
#include "orchard/apfs/object.h"
#include "orchard/apfs/omap.h"
#include "orchard/apfs/path_lookup.h"
#include "orchard/apfs/volume.h"
#include "orchard/blockio/error.h"
#include "orchard/blockio/inspection_target.h"
#include "orchard/blockio/reader.h"

namespace {

std::string EscapeJson(std::string_view input) {
  std::string escaped;
  escaped.reserve(input.size());

  for (const char character : input) {
    switch (character) {
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped += character;
      break;
    }
  }

  return escaped;
}

std::string ToHexString(const std::uint64_t value) {
  std::ostringstream stream;
  stream << "0x" << std::hex << std::uppercase << value;
  return stream.str();
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

struct CommandLineOptions {
  std::string target;
  std::optional<std::string> list_path;
  orchard::apfs::InspectionOptions inspection;
};

void PrintUsage(const std::string_view program_name) {
  std::cout << "Usage: " << program_name << " --target <path> [--enrich-raw]"
            << " [--volume-oid <id>] [--list-path <orchard-path>]\n";
  std::cout << "   or: " << program_name
            << " <path> [--enrich-raw] [--volume-oid <id>] [--list-path <orchard-path>]\n";
}

std::optional<std::uint64_t> ParseUint64(const std::string_view text) {
  try {
    std::size_t parsed = 0;
    const auto value = std::stoull(std::string(text), &parsed, 0);
    if (parsed != text.size()) {
      return std::nullopt;
    }
    return value;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<CommandLineOptions> ParseCommandLine(int argc, char** argv) {
  CommandLineOptions options;
  bool target_consumed = false;

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      return CommandLineOptions{};
    }
    if (argument == "--target") {
      if (index + 1 >= argc) {
        return std::nullopt;
      }
      options.target = argv[++index];
      target_consumed = true;
      continue;
    }
    if (argument == "--enrich-raw") {
      options.inspection.enrich_raw_device_volumes = true;
      continue;
    }
    if (argument == "--volume-oid") {
      if (index + 1 >= argc) {
        return std::nullopt;
      }
      const auto parsed = ParseUint64(argv[++index]);
      if (!parsed.has_value()) {
        return std::nullopt;
      }
      options.inspection.enrich_raw_device_volumes = true;
      options.inspection.volume_object_id = *parsed;
      continue;
    }
    if (argument == "--list-path") {
      if (index + 1 >= argc) {
        return std::nullopt;
      }
      options.list_path = std::string(argv[++index]);
      continue;
    }
    if (!target_consumed && !argument.starts_with("-")) {
      options.target = std::string(argument);
      target_consumed = true;
      continue;
    }

    return std::nullopt;
  }

  if (options.target.empty()) {
    return std::nullopt;
  }
  return options;
}

void PrintQuotedStringArray(const std::vector<std::string>& values, const std::string_view indent) {
  std::cout << "[";
  if (!values.empty()) {
    std::cout << "\n";
  }

  for (std::size_t index = 0; index < values.size(); ++index) {
    std::cout << indent << "\"" << EscapeJson(values[index]) << "\"";
    if (index + 1U != values.size()) {
      std::cout << ",";
    }
    std::cout << "\n";
  }

  if (!values.empty()) {
    std::cout << std::string(indent.substr(0, indent.size() - 2U));
  }
  std::cout << "]";
}

void PrintPolicyReasonArray(const std::vector<orchard::apfs::PolicyReason>& values,
                            const std::string_view indent) {
  std::cout << "[";
  if (!values.empty()) {
    std::cout << "\n";
  }

  for (std::size_t index = 0; index < values.size(); ++index) {
    std::cout << indent << "\"" << orchard::apfs::ToString(values[index]) << "\"";
    if (index + 1U != values.size()) {
      std::cout << ",";
    }
    std::cout << "\n";
  }

  if (!values.empty()) {
    std::cout << std::string(indent.substr(0, indent.size() - 2U));
  }
  std::cout << "]";
}

void PrintErrorObject(const std::optional<orchard::blockio::Error>& error,
                      const std::string_view indent) {
  if (!error.has_value()) {
    std::cout << "null";
    return;
  }

  std::cout << "{\n";
  std::cout << indent << "\"code\": \"" << orchard::blockio::ToString(error->code) << "\",\n";
  std::cout << indent << "\"message\": \"" << EscapeJson(error->message) << "\",\n";
  std::cout << indent << "\"system_code\": " << error->system_code << "\n";
  std::cout << std::string(indent.substr(0, indent.size() - 2U)) << "}";
}

void PrintPolicyObject(const orchard::apfs::PolicyDecision& policy, const std::string_view indent) {
  std::cout << "{\n";
  std::cout << indent << "\"action\": \"" << orchard::apfs::ToString(policy.action) << "\",\n";
  std::cout << indent << "\"reasons\": ";
  PrintPolicyReasonArray(policy.reasons, std::string(indent) + "  ");
  std::cout << ",\n";
  std::cout << indent << "\"summary\": \"" << EscapeJson(policy.summary) << "\"\n";
  std::cout << std::string(indent.substr(0, indent.size() - 2U)) << "}";
}

void PrintFeatureFlags(const orchard::apfs::FeatureFlags& flags, const std::string_view indent) {
  std::cout << "{\n";
  std::cout << indent << "\"compatible\": \"" << ToHexString(flags.compatible) << "\",\n";
  std::cout << indent << "\"readonly_compatible\": \"" << ToHexString(flags.readonly_compatible)
            << "\",\n";
  std::cout << indent << "\"incompatible\": \"" << ToHexString(flags.incompatible) << "\",\n";
  std::cout << indent << "\"compatible_names\": ";
  PrintQuotedStringArray(flags.compatible_names, std::string(indent) + "  ");
  std::cout << ",\n";
  std::cout << indent << "\"readonly_compatible_names\": ";
  PrintQuotedStringArray(flags.readonly_compatible_names, std::string(indent) + "  ");
  std::cout << ",\n";
  std::cout << indent << "\"incompatible_names\": ";
  PrintQuotedStringArray(flags.incompatible_names, std::string(indent) + "  ");
  std::cout << "\n";
  std::cout << std::string(indent.substr(0, indent.size() - 2U)) << "}";
}

void PrintPartitionObject(const orchard::apfs::PartitionInfo& partition,
                          const std::string_view indent) {
  std::cout << "{\n";
  std::cout << indent << "\"type_guid\": \"" << EscapeJson(partition.type_guid) << "\",\n";
  std::cout << indent << "\"unique_guid\": \"" << EscapeJson(partition.unique_guid) << "\",\n";
  std::cout << indent << "\"name\": \"" << EscapeJson(partition.name) << "\",\n";
  std::cout << indent << "\"first_lba\": " << partition.first_lba << ",\n";
  std::cout << indent << "\"last_lba\": " << partition.last_lba << ",\n";
  std::cout << indent << "\"byte_offset\": " << partition.byte_offset << ",\n";
  std::cout << indent << "\"byte_length\": " << partition.byte_length << ",\n";
  std::cout << indent
            << "\"is_apfs_partition\": " << (partition.is_apfs_partition ? "true" : "false")
            << "\n";
  std::cout << std::string(indent.substr(0, indent.size() - 2U)) << "}";
}

void PrintPartitions(const std::vector<orchard::apfs::PartitionInfo>& partitions,
                     const std::string_view indent) {
  std::cout << "[";
  if (!partitions.empty()) {
    std::cout << "\n";
  }

  for (std::size_t index = 0; index < partitions.size(); ++index) {
    std::cout << indent;
    PrintPartitionObject(partitions[index], std::string(indent) + "  ");
    if (index + 1U != partitions.size()) {
      std::cout << ",";
    }
    std::cout << "\n";
  }

  if (!partitions.empty()) {
    std::cout << std::string(indent.substr(0, indent.size() - 2U));
  }
  std::cout << "]";
}

void PrintDirectoryEntrySamples(const std::vector<orchard::apfs::DirectoryEntrySample>& entries,
                                const std::string_view indent) {
  std::cout << "[";
  if (!entries.empty()) {
    std::cout << "\n";
  }

  for (std::size_t index = 0; index < entries.size(); ++index) {
    std::cout << indent << "{\n";
    std::cout << indent << "  \"name\": \"" << EscapeJson(entries[index].name) << "\",\n";
    std::cout << indent << "  \"inode_id\": " << entries[index].inode_id << ",\n";
    std::cout << indent << "  \"kind\": \"" << EscapeJson(entries[index].kind) << "\"\n";
    std::cout << indent << "}";
    if (index + 1U != entries.size()) {
      std::cout << ",";
    }
    std::cout << "\n";
  }

  if (!entries.empty()) {
    std::cout << std::string(indent.substr(0, indent.size() - 2U));
  }
  std::cout << "]";
}

void PrintFileProbes(const std::vector<orchard::apfs::FileProbeInfo>& probes,
                     const std::string_view indent) {
  std::cout << "[";
  if (!probes.empty()) {
    std::cout << "\n";
  }

  for (std::size_t index = 0; index < probes.size(); ++index) {
    std::cout << indent << "{\n";
    std::cout << indent << "  \"path\": \"" << EscapeJson(probes[index].path) << "\",\n";
    std::cout << indent << "  \"inode_id\": " << probes[index].inode_id << ",\n";
    std::cout << indent << "  \"size_bytes\": " << probes[index].size_bytes << ",\n";
    std::cout << indent << "  \"kind\": \"" << EscapeJson(probes[index].kind) << "\",\n";
    std::cout << indent << "  \"link_count\": " << probes[index].link_count << ",\n";
    std::cout << indent << "  \"compression\": \"" << EscapeJson(probes[index].compression)
              << "\",\n";
    std::cout << indent << "  \"sparse\": " << (probes[index].sparse ? "true" : "false") << ",\n";
    std::cout << indent << "  \"symlink_target\": ";
    if (probes[index].symlink_target.has_value()) {
      std::cout << "\"" << EscapeJson(probes[index].symlink_target.value_or(std::string{})) << "\"";
    } else {
      std::cout << "null";
    }
    std::cout << ",\n";
    std::cout << indent << "  \"aliases\": ";
    PrintQuotedStringArray(probes[index].aliases, std::string(indent) + "    ");
    std::cout << ",\n";
    std::cout << indent << "  \"preview_utf8\": \"" << EscapeJson(probes[index].preview_utf8)
              << "\",\n";
    std::cout << indent << "  \"preview_hex\": \"" << EscapeJson(probes[index].preview_hex)
              << "\"\n";
    std::cout << indent << "}";
    if (index + 1U != probes.size()) {
      std::cout << ",";
    }
    std::cout << "\n";
  }

  if (!probes.empty()) {
    std::cout << std::string(indent.substr(0, indent.size() - 2U));
  }
  std::cout << "]";
}

void PrintVolumeObject(const orchard::apfs::VolumeInfo& volume, const std::string_view indent) {
  std::cout << "{\n";
  std::cout << indent << "\"object_id\": " << volume.object_id << ",\n";
  std::cout << indent << "\"xid\": " << volume.xid << ",\n";
  std::cout << indent << "\"filesystem_index\": " << volume.filesystem_index << ",\n";
  std::cout << indent << "\"name\": \"" << EscapeJson(volume.name) << "\",\n";
  std::cout << indent << "\"uuid\": \"" << EscapeJson(volume.uuid) << "\",\n";
  std::cout << indent << "\"role\": \"" << ToHexString(volume.role) << "\",\n";
  std::cout << indent << "\"role_names\": ";
  PrintQuotedStringArray(volume.role_names, std::string(indent) + "  ");
  std::cout << ",\n";
  std::cout << indent << "\"root_tree_type\": \"" << ToHexString(volume.root_tree_type) << "\",\n";
  std::cout << indent << "\"omap_oid\": " << volume.omap_oid << ",\n";
  std::cout << indent << "\"root_tree_oid\": " << volume.root_tree_oid << ",\n";
  std::cout << indent << "\"root_directory_object_id\": " << volume.root_directory_object_id
            << ",\n";
  std::cout << indent << "\"case_insensitive\": " << (volume.case_insensitive ? "true" : "false")
            << ",\n";
  std::cout << indent << "\"snapshots_present\": " << (volume.snapshots_present ? "true" : "false")
            << ",\n";
  std::cout << indent << "\"encryption_rolled\": " << (volume.encryption_rolled ? "true" : "false")
            << ",\n";
  std::cout << indent
            << "\"incomplete_restore\": " << (volume.incomplete_restore ? "true" : "false")
            << ",\n";
  std::cout << indent << "\"normalization_insensitive\": "
            << (volume.normalization_insensitive ? "true" : "false") << ",\n";
  std::cout << indent << "\"sealed\": " << (volume.sealed ? "true" : "false") << ",\n";
  std::cout << indent << "\"features\": ";
  PrintFeatureFlags(volume.features, std::string(indent) + "  ");
  std::cout << ",\n";
  std::cout << indent << "\"policy\": ";
  PrintPolicyObject(volume.policy, std::string(indent) + "  ");
  std::cout << ",\n";
  std::cout << indent << "\"root_entries\": ";
  PrintDirectoryEntrySamples(volume.root_entries, std::string(indent) + "  ");
  std::cout << ",\n";
  std::cout << indent << "\"root_file_probes\": ";
  PrintFileProbes(volume.root_file_probes, std::string(indent) + "  ");
  std::cout << ",\n";
  std::cout << indent << "\"notes\": ";
  PrintQuotedStringArray(volume.notes, std::string(indent) + "  ");
  std::cout << "\n";
  std::cout << std::string(indent.substr(0, indent.size() - 2U)) << "}";
}

void PrintVolumes(const std::vector<orchard::apfs::VolumeInfo>& volumes,
                  const std::string_view indent) {
  std::cout << "[";
  if (!volumes.empty()) {
    std::cout << "\n";
  }

  for (std::size_t index = 0; index < volumes.size(); ++index) {
    std::cout << indent;
    PrintVolumeObject(volumes[index], std::string(indent) + "  ");
    if (index + 1U != volumes.size()) {
      std::cout << ",";
    }
    std::cout << "\n";
  }

  if (!volumes.empty()) {
    std::cout << std::string(indent.substr(0, indent.size() - 2U));
  }
  std::cout << "]";
}

void PrintObjectIdArray(const std::vector<std::uint64_t>& values, const std::string_view indent) {
  std::cout << "[";
  if (!values.empty()) {
    std::cout << "\n";
  }

  for (std::size_t index = 0; index < values.size(); ++index) {
    std::cout << indent << values[index];
    if (index + 1U != values.size()) {
      std::cout << ",";
    }
    std::cout << "\n";
  }

  if (!values.empty()) {
    std::cout << std::string(indent.substr(0, indent.size() - 2U));
  }
  std::cout << "]";
}

void PrintContainerObject(const orchard::apfs::ContainerInfo& container,
                          const std::string_view indent) {
  std::cout << "{\n";
  std::cout << indent << "\"source_layout\": \"" << orchard::apfs::ToString(container.source_layout)
            << "\",\n";
  std::cout << indent << "\"byte_offset\": " << container.byte_offset << ",\n";
  std::cout << indent << "\"byte_length\": " << container.byte_length << ",\n";
  std::cout << indent << "\"block_size\": " << container.block_size << ",\n";
  std::cout << indent << "\"block_count\": " << container.block_count << ",\n";
  std::cout << indent << "\"uuid\": \"" << EscapeJson(container.uuid) << "\",\n";
  std::cout << indent << "\"checkpoint\": {\n";
  std::cout << indent << "  \"selected_block_index\": " << container.selected_checkpoint.block_index
            << ",\n";
  std::cout << indent << "  \"selected_xid\": " << container.selected_checkpoint.xid << ",\n";
  std::cout << indent << "  \"source\": \""
            << orchard::apfs::ToString(container.selected_checkpoint.source) << "\"\n";
  std::cout << indent << "},\n";
  std::cout << indent << "\"features\": ";
  PrintFeatureFlags(container.features, std::string(indent) + "  ");
  std::cout << ",\n";
  std::cout << indent << "\"spaceman_oid\": " << container.spaceman_oid << ",\n";
  std::cout << indent << "\"omap_oid\": " << container.omap_oid << ",\n";
  std::cout << indent << "\"reaper_oid\": " << container.reaper_oid << ",\n";
  std::cout << indent << "\"volumes_resolved_via_omap\": "
            << (container.volumes_resolved_via_omap ? "true" : "false") << ",\n";
  std::cout << indent << "\"volume_object_ids\": ";
  PrintObjectIdArray(container.volume_object_ids, std::string(indent) + "  ");
  std::cout << ",\n";
  std::cout << indent << "\"partition\": ";
  if (container.partition.has_value()) {
    PrintPartitionObject(*container.partition, std::string(indent) + "  ");
  } else {
    std::cout << "null";
  }
  std::cout << ",\n";
  std::cout << indent << "\"volumes\": ";
  PrintVolumes(container.volumes, std::string(indent) + "  ");
  std::cout << ",\n";
  std::cout << indent << "\"notes\": ";
  PrintQuotedStringArray(container.notes, std::string(indent) + "  ");
  std::cout << "\n";
  std::cout << std::string(indent.substr(0, indent.size() - 2U)) << "}";
}

void PrintContainers(const std::vector<orchard::apfs::ContainerInfo>& containers,
                     const std::string_view indent) {
  std::cout << "[";
  if (!containers.empty()) {
    std::cout << "\n";
  }

  for (std::size_t index = 0; index < containers.size(); ++index) {
    std::cout << indent;
    PrintContainerObject(containers[index], std::string(indent) + "  ");
    if (index + 1U != containers.size()) {
      std::cout << ",";
    }
    std::cout << "\n";
  }

  if (!containers.empty()) {
    std::cout << std::string(indent.substr(0, indent.size() - 2U));
  }
  std::cout << "]";
}

struct PathListingEntry {
  std::string name;
  std::string name_hex;
  std::uint64_t inode_id = 0;
  std::string kind;
};

struct PathListingResult {
  std::string requested_path;
  std::string normalized_path;
  std::vector<PathListingEntry> entries;
  std::optional<orchard::blockio::Error> error;
};

std::string ToHexFromStringBytes(const std::string_view text) {
  std::vector<std::uint8_t> bytes(text.begin(), text.end());
  return ToHexPreview(bytes);
}

const orchard::apfs::ContainerInfo* SelectContainer(const orchard::apfs::DiscoveryReport& report) {
  if (report.containers.empty()) {
    return nullptr;
  }
  return &report.containers.front();
}

const orchard::apfs::VolumeInfo* SelectVolume(const orchard::apfs::ContainerInfo& container,
                                              const orchard::apfs::InspectionOptions& options) {
  if (!options.volume_object_id.has_value()) {
    return container.volumes.empty() ? nullptr : &container.volumes.front();
  }

  const auto it = std::find_if(container.volumes.begin(), container.volumes.end(),
                               [&options](const orchard::apfs::VolumeInfo& volume) {
                                 return volume.object_id == options.volume_object_id.value_or(0U);
                               });
  return it == container.volumes.end() ? nullptr : &(*it);
}

std::optional<PathListingResult>
LoadPathListing(const orchard::blockio::InspectionTargetInfo& target_info,
                const CommandLineOptions& options) {
  if (!options.list_path.has_value()) {
    return std::nullopt;
  }

  PathListingResult listing;
  listing.requested_path = *options.list_path;

  auto reader_result = orchard::blockio::OpenReader(target_info);
  if (!reader_result.ok()) {
    listing.error = reader_result.error();
    return listing;
  }

  auto reader = std::move(reader_result).value();
  auto discovery_result = orchard::apfs::Discover(*reader);
  if (!discovery_result.ok()) {
    listing.error = discovery_result.error();
    return listing;
  }

  const auto* container = SelectContainer(discovery_result.value());
  if (container == nullptr) {
    listing.error = orchard::blockio::Error{
        .code = orchard::blockio::ErrorCode::kNotFound,
        .message = "No APFS container was found for path listing.",
    };
    return listing;
  }

  const auto* volume = SelectVolume(*container, options.inspection);
  if (volume == nullptr) {
    listing.error = orchard::blockio::Error{
        .code = orchard::blockio::ErrorCode::kNotFound,
        .message = "Requested APFS volume was not found for path listing.",
    };
    return listing;
  }

  orchard::apfs::PhysicalObjectReader object_reader(*reader, container->byte_offset,
                                                    container->block_size);
  auto container_omap_result =
      orchard::apfs::OmapResolver::Load(object_reader, container->omap_oid);
  if (!container_omap_result.ok()) {
    listing.error = container_omap_result.error();
    return listing;
  }

  auto volume_result = orchard::apfs::VolumeContext::Load(*reader, *container, *volume,
                                                          container_omap_result.value());
  if (!volume_result.ok()) {
    listing.error = volume_result.error();
    return listing;
  }

  auto path_result = orchard::apfs::LookupPath(volume_result.value(), *options.list_path);
  if (!path_result.ok()) {
    listing.error = path_result.error();
    return listing;
  }
  listing.normalized_path = path_result.value().normalized_path;

  auto entries_result = orchard::apfs::ListDirectory(
      volume_result.value(), path_result.value().inode.key.header.object_id);
  if (!entries_result.ok()) {
    listing.error = entries_result.error();
    return listing;
  }

  listing.entries.reserve(entries_result.value().size());
  for (const auto& entry : entries_result.value()) {
    listing.entries.push_back(PathListingEntry{
        .name = entry.key.name,
        .name_hex = ToHexFromStringBytes(entry.key.name),
        .inode_id = entry.file_id,
        .kind = std::string(orchard::apfs::ToString(entry.kind)),
    });
  }

  return listing;
}

void PrintPathListingEntryArray(const std::vector<PathListingEntry>& entries,
                                const std::string_view indent) {
  std::cout << "[";
  if (!entries.empty()) {
    std::cout << "\n";
  }

  for (std::size_t index = 0; index < entries.size(); ++index) {
    std::cout << indent << "{\n";
    std::cout << indent << "  \"name\": \"" << EscapeJson(entries[index].name) << "\",\n";
    std::cout << indent << "  \"name_hex\": \"" << EscapeJson(entries[index].name_hex) << "\",\n";
    std::cout << indent << "  \"inode_id\": " << entries[index].inode_id << ",\n";
    std::cout << indent << "  \"kind\": \"" << EscapeJson(entries[index].kind) << "\"\n";
    std::cout << indent << "}";
    if (index + 1U != entries.size()) {
      std::cout << ",";
    }
    std::cout << "\n";
  }

  if (!entries.empty()) {
    std::cout << std::string(indent.substr(0, indent.size() - 2U));
  }
  std::cout << "]";
}

void PrintJson(const orchard::blockio::InspectionTargetInfo& target_info,
               const orchard::apfs::InspectionResult& inspection_result,
               const std::optional<PathListingResult>& path_listing) {
  std::cout << "{\n";
  std::cout << "  \"tool\": \"orchard-inspect\",\n";
  std::cout << "  \"version\": \"" << EscapeJson(ORCHARD_VERSION) << "\",\n";
  std::cout << "  \"target\": {\n";
  std::cout << "    \"path\": \"" << EscapeJson(target_info.path.string()) << "\",\n";
  std::cout << "    \"kind\": \"" << orchard::blockio::ToString(target_info.kind) << "\",\n";
  std::cout << "    \"exists\": " << (target_info.exists ? "true" : "false") << ",\n";
  std::cout << "    \"probe_candidate\": " << (target_info.probe_candidate ? "true" : "false")
            << ",\n";
  std::cout << "    \"size_bytes\": ";
  if (target_info.size_bytes.has_value()) {
    std::cout << *target_info.size_bytes;
  } else {
    std::cout << "null";
  }
  std::cout << "\n";
  std::cout << "  },\n";
  std::cout << "  \"apfs\": {\n";
  std::cout << "    \"inspection_status\": \"" << orchard::apfs::ToString(inspection_result.status)
            << "\",\n";
  std::cout << "    \"reader_backend\": ";
  if (inspection_result.reader_backend.empty()) {
    std::cout << "null";
  } else {
    std::cout << "\"" << EscapeJson(inspection_result.reader_backend) << "\"";
  }
  std::cout << ",\n";
  std::cout << "    \"reader_size_bytes\": ";
  if (inspection_result.reader_size_bytes.has_value()) {
    std::cout << *inspection_result.reader_size_bytes;
  } else {
    std::cout << "null";
  }
  std::cout << ",\n";
  std::cout << "    \"error\": ";
  PrintErrorObject(inspection_result.error, "      ");
  std::cout << ",\n";
  std::cout << "    \"layout\": \"" << orchard::apfs::ToString(inspection_result.report.layout)
            << "\",\n";
  std::cout << "    \"gpt_block_size\": ";
  if (inspection_result.report.gpt_block_size.has_value()) {
    std::cout << *inspection_result.report.gpt_block_size;
  } else {
    std::cout << "null";
  }
  std::cout << ",\n";
  std::cout << "    \"partitions\": ";
  PrintPartitions(inspection_result.report.partitions, "      ");
  std::cout << ",\n";
  std::cout << "    \"containers\": ";
  PrintContainers(inspection_result.report.containers, "      ");
  std::cout << ",\n";
  std::cout << "    \"notes\": ";
  PrintQuotedStringArray(inspection_result.notes, "      ");
  std::cout << "\n";
  std::cout << "  },\n";
  std::cout << "  \"path_listing\": ";
  if (!path_listing.has_value()) {
    std::cout << "null\n";
  } else {
    std::cout << "{\n";
    std::cout << "    \"requested_path\": \"" << EscapeJson(path_listing->requested_path)
              << "\",\n";
    std::cout << "    \"normalized_path\": \"" << EscapeJson(path_listing->normalized_path)
              << "\",\n";
    std::cout << "    \"error\": ";
    PrintErrorObject(path_listing->error, "      ");
    std::cout << ",\n";
    std::cout << "    \"entries\": ";
    PrintPathListingEntryArray(path_listing->entries, "      ");
    std::cout << "\n";
    std::cout << "  }\n";
  }
  std::cout << "}\n";
}

} // namespace

int main(int argc, char** argv) {
  if (argc <= 1) {
    PrintUsage(argc > 0 ? argv[0] : "orchard-inspect");
    return 1;
  }

  const auto options = ParseCommandLine(argc, argv);
  if (!options.has_value()) {
    PrintUsage(argv[0]);
    return 1;
  }
  if (options->target.empty()) {
    PrintUsage(argv[0]);
    return 0;
  }

  const auto target_path = std::filesystem::path(options->target);
  const auto target_info = orchard::blockio::InspectTargetPath(target_path);
  const auto inspection_result = orchard::apfs::InspectTarget(target_info, options->inspection);
  const auto path_listing = LoadPathListing(target_info, *options);
  PrintJson(target_info, inspection_result, path_listing);
  return 0;
}
