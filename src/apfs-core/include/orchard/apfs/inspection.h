#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "orchard/apfs/discovery.h"
#include "orchard/blockio/error.h"
#include "orchard/blockio/inspection_target.h"

namespace orchard::apfs {

enum class InspectionStatus {
  kMissingTarget,
  kUnsupportedTarget,
  kOpenFailed,
  kReadFailed,
  kParseFailed,
  kNoApfsContainer,
  kSuccess,
};

struct InspectionResult {
  InspectionStatus status = InspectionStatus::kUnsupportedTarget;
  std::string reader_backend;
  std::optional<std::uint64_t> reader_size_bytes;
  std::optional<blockio::Error> error;
  DiscoveryReport report;
  std::vector<std::string> notes;
};

struct InspectionOptions {
  bool enrich_raw_device_volumes = false;
  std::optional<std::uint64_t> volume_object_id;
};

InspectionResult InspectTarget(const blockio::InspectionTargetInfo& target_info);
InspectionResult InspectTarget(const blockio::InspectionTargetInfo& target_info,
                               const InspectionOptions& options);
std::string_view ToString(InspectionStatus status) noexcept;

} // namespace orchard::apfs
