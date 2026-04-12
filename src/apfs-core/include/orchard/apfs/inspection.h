#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "orchard/blockio/inspection_target.h"

namespace orchard::apfs {

enum class ProbeStatus {
  kMissingTarget,
  kUnsupportedTarget,
  kNotImplemented,
  kOpenFailed,
  kStubScanned,
};

struct StubInspectionResult {
  ProbeStatus status = ProbeStatus::kUnsupportedTarget;
  bool apfs_container_magic_present = false;
  std::string suggested_mount_mode = "reject";
  std::vector<std::string> notes;
};

StubInspectionResult RunStubInspection(const blockio::InspectionTargetInfo& target_info);
std::string_view ToString(ProbeStatus status) noexcept;

}  // namespace orchard::apfs

