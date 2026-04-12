#include "orchard/blockio/inspection_target.h"

#include <system_error>

namespace orchard::blockio {
namespace {

constexpr std::string_view kPhysicalDrivePrefix = R"(\\.\PhysicalDrive)";
constexpr std::string_view kVolumePrefix = R"(\\.\Volume{)";

} // namespace

InspectionTargetInfo InspectTargetPath(const std::filesystem::path& path) {
  InspectionTargetInfo info;
  info.path = path;

  const std::string native_path = path.string();
  if (LooksLikeRawDevicePath(native_path)) {
    info.kind = TargetKind::kRawDevice;
    info.probe_candidate = true;
    return info;
  }

  std::error_code error_code;
  info.exists = std::filesystem::exists(path, error_code);

  if (error_code || !info.exists) {
    info.kind = TargetKind::kMissing;
    return info;
  }

  info.is_regular_file = std::filesystem::is_regular_file(path, error_code);
  if (!error_code && info.is_regular_file) {
    info.kind = TargetKind::kRegularFile;
    info.probe_candidate = true;
    info.size_bytes = std::filesystem::file_size(path, error_code);
    if (error_code) {
      info.size_bytes.reset();
    }
    return info;
  }

  error_code.clear();
  info.is_directory = std::filesystem::is_directory(path, error_code);
  if (!error_code && info.is_directory) {
    info.kind = TargetKind::kDirectory;
    return info;
  }

  info.kind = TargetKind::kUnknown;
  return info;
}

bool LooksLikeRawDevicePath(std::string_view native_path) noexcept {
  return native_path.starts_with(kPhysicalDrivePrefix) || native_path.starts_with(kVolumePrefix);
}

std::string_view ToString(TargetKind kind) noexcept {
  switch (kind) {
  case TargetKind::kRegularFile:
    return "regular_file";
  case TargetKind::kRawDevice:
    return "raw_device";
  case TargetKind::kDirectory:
    return "directory";
  case TargetKind::kMissing:
    return "missing";
  case TargetKind::kUnknown:
    return "unknown";
  }

  return "unknown";
}

} // namespace orchard::blockio
