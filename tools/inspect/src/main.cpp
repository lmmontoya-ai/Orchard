#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "orchard/apfs/inspection.h"
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

void PrintJson(const orchard::blockio::InspectionTargetInfo& target_info,
               const orchard::apfs::StubInspectionResult& inspection_result) {
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
  std::cout << "    \"probe_status\": \"" << orchard::apfs::ToString(inspection_result.status)
            << "\",\n";
  std::cout << "    \"container_magic\": "
            << (inspection_result.apfs_container_magic_present ? "\"present\"" : "\"absent\"")
            << ",\n";
  std::cout << "    \"suggested_mount_mode\": \""
            << EscapeJson(inspection_result.suggested_mount_mode) << "\",\n";
  std::cout << "    \"notes\": [\n";

  for (std::size_t index = 0; index < inspection_result.notes.size(); ++index) {
    std::cout << "      \"" << EscapeJson(inspection_result.notes[index]) << "\"";
    if (index + 1 != inspection_result.notes.size()) {
      std::cout << ",";
    }
    std::cout << "\n";
  }

  std::cout << "    ]\n";
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
  const auto inspection_result = orchard::apfs::RunStubInspection(target_info);
  PrintJson(target_info, inspection_result);
  return 0;
}
