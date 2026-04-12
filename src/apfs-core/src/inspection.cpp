#include "orchard/apfs/inspection.h"

#include <array>
#include <fstream>

#include "orchard/apfs/probe.h"

namespace orchard::apfs {

StubInspectionResult RunStubInspection(const blockio::InspectionTargetInfo& target_info) {
  StubInspectionResult result;
  result.notes.push_back("Stub inspection only. Full APFS parsing begins in M1.");

  switch (target_info.kind) {
  case blockio::TargetKind::kMissing:
    result.status = ProbeStatus::kMissingTarget;
    result.notes.push_back("Target path does not exist or could not be resolved.");
    return result;

  case blockio::TargetKind::kDirectory:
    result.status = ProbeStatus::kUnsupportedTarget;
    result.notes.push_back("Directories are not probe targets. Use a disk path or image file.");
    return result;

  case blockio::TargetKind::kRawDevice:
    result.status = ProbeStatus::kNotImplemented;
    result.notes.push_back("Raw-device probing is planned but not implemented in the M0 stub.");
    result.suggested_mount_mode = "unknown";
    return result;

  case blockio::TargetKind::kUnknown:
    result.status = ProbeStatus::kUnsupportedTarget;
    result.notes.push_back("Target kind is not supported by the stub inspection path.");
    return result;

  case blockio::TargetKind::kRegularFile:
    break;
  }

  std::ifstream stream(target_info.path, std::ios::binary);
  if (!stream) {
    result.status = ProbeStatus::kOpenFailed;
    result.notes.push_back("Failed to open the target file.");
    return result;
  }

  std::array<std::uint8_t, 4> header{};
  stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));

  result.status = ProbeStatus::kStubScanned;
  result.apfs_container_magic_present = ProbeContainerMagic(header);
  result.suggested_mount_mode = result.apfs_container_magic_present ? "unknown" : "reject";

  if (result.apfs_container_magic_present) {
    result.notes.push_back("Detected APFS container magic at offset 0.");
  } else {
    result.notes.push_back("APFS container magic was not detected at offset 0.");
  }

  return result;
}

std::string_view ToString(ProbeStatus status) noexcept {
  switch (status) {
  case ProbeStatus::kMissingTarget:
    return "missing_target";
  case ProbeStatus::kUnsupportedTarget:
    return "unsupported_target";
  case ProbeStatus::kNotImplemented:
    return "not_implemented";
  case ProbeStatus::kOpenFailed:
    return "open_failed";
  case ProbeStatus::kStubScanned:
    return "stub_scanned";
  }

  return "unsupported_target";
}

} // namespace orchard::apfs
