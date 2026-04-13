#include "orchard/fs_winfsp/filesystem.h"

#include "orchard/apfs/format.h"

namespace orchard::fs_winfsp {

blockio::Result<MountSessionHandle> CreateFileSystemSession(const MountConfig& config,
                                                            MountedVolumeHandle mounted_volume) {
  static_cast<void>(config);
  static_cast<void>(mounted_volume);
  return orchard::apfs::MakeApfsError(
      blockio::ErrorCode::kNotImplemented,
      "WinFsp SDK support is not available in this Orchard build. Configure with "
      "ORCHARD_ENABLE_WINFSP=ON and a valid WINFSP_ROOT_DIR developer SDK root.");
}

bool IsWinFspBackendAvailable() noexcept {
  return false;
}

std::string_view WinFspBackendStatus() noexcept {
  return "sdk_unavailable";
}

} // namespace orchard::fs_winfsp
