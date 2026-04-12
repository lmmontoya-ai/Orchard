#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace orchard::blockio {

enum class TargetKind {
  kRegularFile,
  kRawDevice,
  kDirectory,
  kMissing,
  kUnknown,
};

struct InspectionTargetInfo {
  std::filesystem::path path;
  TargetKind kind = TargetKind::kUnknown;
  bool exists = false;
  bool probe_candidate = false;
  bool is_regular_file = false;
  bool is_directory = false;
  std::optional<std::uintmax_t> size_bytes;
};

InspectionTargetInfo InspectTargetPath(const std::filesystem::path& path);
bool LooksLikeRawDevicePath(std::string_view native_path) noexcept;
std::string_view ToString(TargetKind kind) noexcept;

} // namespace orchard::blockio
