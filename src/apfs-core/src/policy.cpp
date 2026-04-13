#include "orchard/apfs/policy.h"

#include <algorithm>

#include "orchard/apfs/format.h"

namespace orchard::apfs {
namespace {

bool HasReason(const std::vector<PolicyReason>& reasons, const PolicyReason reason) {
  return std::find(reasons.begin(), reasons.end(), reason) != reasons.end();
}

void AppendReason(std::vector<PolicyReason>& reasons, const PolicyReason reason) {
  if (!HasReason(reasons, reason)) {
    reasons.push_back(reason);
  }
}

bool IsHelperRole(const std::uint16_t role) {
  return (role & (kVolumeRoleSystem | kVolumeRoleRecovery | kVolumeRoleVm | kVolumeRolePreboot |
                  kVolumeRoleInstaller)) != 0U;
}

void AppendRoleReasons(std::vector<PolicyReason>& reasons, const std::uint16_t role) {
  if ((role & kVolumeRoleSystem) != 0U) {
    AppendReason(reasons, PolicyReason::kRoleSystem);
  }
  if ((role & kVolumeRoleRecovery) != 0U) {
    AppendReason(reasons, PolicyReason::kRoleRecovery);
  }
  if ((role & kVolumeRoleVm) != 0U) {
    AppendReason(reasons, PolicyReason::kRoleVm);
  }
  if ((role & kVolumeRolePreboot) != 0U) {
    AppendReason(reasons, PolicyReason::kRolePreboot);
  }
  if ((role & kVolumeRoleInstaller) != 0U) {
    AppendReason(reasons, PolicyReason::kRoleInstaller);
  }
}

} // namespace

PolicyDecision EvaluatePolicy(const PolicyInput& input) {
  PolicyDecision decision;

  const auto supported_readwrite_incompatible = kVolumeIncompatCaseInsensitive;
  const auto explicitly_handled_incompatible =
      kVolumeIncompatCaseInsensitive | kVolumeIncompatDatalessSnaps | kVolumeIncompatEncRolled |
      kVolumeIncompatNormalizationInsensitive | kVolumeIncompatIncompleteRestore |
      kVolumeIncompatSealed;
  const auto unknown_incompatible =
      input.volume_incompatible_features & ~explicitly_handled_incompatible;

  if (input.container_fusion) {
    AppendReason(decision.reasons, PolicyReason::kContainerFusion);
  }
  if (input.sealed) {
    AppendReason(decision.reasons, PolicyReason::kSealed);
  }
  if (input.encryption_rolled) {
    AppendReason(decision.reasons, PolicyReason::kEncryptionRolled);
  }
  if (input.incomplete_restore) {
    AppendReason(decision.reasons, PolicyReason::kIncompleteRestore);
  }
  if (input.normalization_insensitive) {
    AppendReason(decision.reasons, PolicyReason::kNormalizationInsensitive);
  }
  if (unknown_incompatible != 0U) {
    AppendReason(decision.reasons, PolicyReason::kUnsupportedVolumeFeature);
  }

  if (!decision.reasons.empty()) {
    decision.action = MountDisposition::kReject;
    decision.summary = "Rejected because the volume advertises unsupported write-safety metadata.";
    AppendRoleReasons(decision.reasons, input.role);
    return decision;
  }

  if (IsHelperRole(input.role)) {
    decision.action = MountDisposition::kHide;
    AppendRoleReasons(decision.reasons, input.role);
    decision.summary = "Hidden by default because the volume role is helper or system-oriented.";
    return decision;
  }

  if (!input.case_insensitive) {
    AppendReason(decision.reasons, PolicyReason::kCaseSensitive);
  }
  if (input.snapshots_present) {
    AppendReason(decision.reasons, PolicyReason::kSnapshotsPresent);
  }

  if (!decision.reasons.empty()) {
    decision.action = MountDisposition::kMountReadOnly;
    decision.summary =
        "Readable, but held read-only because v1 does not support all detected features.";
    return decision;
  }

  if ((input.volume_incompatible_features & ~supported_readwrite_incompatible) != 0U) {
    decision.action = MountDisposition::kMountReadOnly;
    AppendReason(decision.reasons, PolicyReason::kUnsupportedVolumeFeature);
    decision.summary = "Readable, but held read-only because the volume advertises unsupported "
                       "incompatible flags.";
    return decision;
  }

  decision.action = MountDisposition::kMountReadWrite;
  decision.summary = "Eligible for the conservative v1 read-write user-data tier.";
  return decision;
}

std::string_view ToString(const MountDisposition disposition) noexcept {
  switch (disposition) {
  case MountDisposition::kHide:
    return "Hide";
  case MountDisposition::kMountReadOnly:
    return "MountReadOnly";
  case MountDisposition::kMountReadWrite:
    return "MountReadWrite";
  case MountDisposition::kReject:
    return "Reject";
  }

  return "Reject";
}

std::string_view ToString(const PolicyReason reason) noexcept {
  switch (reason) {
  case PolicyReason::kRoleSystem:
    return "RoleSystem";
  case PolicyReason::kRoleRecovery:
    return "RoleRecovery";
  case PolicyReason::kRoleVm:
    return "RoleVm";
  case PolicyReason::kRolePreboot:
    return "RolePreboot";
  case PolicyReason::kRoleInstaller:
    return "RoleInstaller";
  case PolicyReason::kSealed:
    return "Sealed";
  case PolicyReason::kSnapshotsPresent:
    return "SnapshotsPresent";
  case PolicyReason::kCaseSensitive:
    return "CaseSensitive";
  case PolicyReason::kContainerFusion:
    return "ContainerFusion";
  case PolicyReason::kEncryptionRolled:
    return "EncryptionRolled";
  case PolicyReason::kIncompleteRestore:
    return "IncompleteRestore";
  case PolicyReason::kNormalizationInsensitive:
    return "NormalizationInsensitive";
  case PolicyReason::kUnsupportedVolumeFeature:
    return "UnsupportedVolumeFeature";
  }

  return "UnsupportedVolumeFeature";
}

} // namespace orchard::apfs
