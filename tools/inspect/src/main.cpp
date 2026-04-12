#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "orchard/apfs/discovery.h"
#include "orchard/apfs/inspection.h"
#include "orchard/blockio/error.h"
#include "orchard/blockio/inspection_target.h"

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

void PrintUsage(const std::string_view program_name) {
  std::cout << "Usage: " << program_name << " --target <path>\n";
  std::cout << "   or: " << program_name << " <path>\n";
}

std::string GetTargetArgument(int argc, char** argv) {
  if (argc == 2) {
    return argv[1];
  }

  if (argc == 3 && std::string_view(argv[1]) == "--target") {
    return argv[2];
  }

  return {};
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

void PrintVolumeObject(const orchard::apfs::VolumeInfo& volume, const std::string_view indent) {
  std::cout << "{\n";
  std::cout << indent << "\"object_id\": " << volume.object_id << ",\n";
  std::cout << indent << "\"filesystem_index\": " << volume.filesystem_index << ",\n";
  std::cout << indent << "\"name\": \"" << EscapeJson(volume.name) << "\",\n";
  std::cout << indent << "\"uuid\": \"" << EscapeJson(volume.uuid) << "\",\n";
  std::cout << indent << "\"role\": \"" << ToHexString(volume.role) << "\",\n";
  std::cout << indent << "\"role_names\": ";
  PrintQuotedStringArray(volume.role_names, std::string(indent) + "  ");
  std::cout << ",\n";
  std::cout << indent << "\"case_insensitive\": " << (volume.case_insensitive ? "true" : "false")
            << ",\n";
  std::cout << indent << "\"sealed\": " << (volume.sealed ? "true" : "false") << ",\n";
  std::cout << indent << "\"features\": ";
  PrintFeatureFlags(volume.features, std::string(indent) + "  ");
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

void PrintJson(const orchard::blockio::InspectionTargetInfo& target_info,
               const orchard::apfs::InspectionResult& inspection_result) {
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
  std::cout << "  }\n";
  std::cout << "}\n";
}

} // namespace

int main(int argc, char** argv) {
  if (argc <= 1) {
    PrintUsage(argc > 0 ? argv[0] : "orchard-inspect");
    return 1;
  }

  const std::string argument = GetTargetArgument(argc, argv);
  if (argument.empty() || argument == "--help" || argument == "-h") {
    PrintUsage(argv[0]);
    return argument.empty() ? 1 : 0;
  }

  const auto target_path = std::filesystem::path(argument);
  const auto target_info = orchard::blockio::InspectTargetPath(target_path);
  const auto inspection_result = orchard::apfs::InspectTarget(target_info);
  PrintJson(target_info, inspection_result);
  return 0;
}
