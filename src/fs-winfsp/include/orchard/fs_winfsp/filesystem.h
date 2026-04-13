#pragma once

#include <memory>
#include <string_view>

#include "orchard/blockio/result.h"
#include "orchard/fs_winfsp/mount.h"

namespace orchard::fs_winfsp {

blockio::Result<MountSessionHandle> CreateFileSystemSession(const MountConfig& config,
                                                            MountedVolumeHandle mounted_volume);
bool IsWinFspBackendAvailable() noexcept;
std::string_view WinFspBackendStatus() noexcept;

} // namespace orchard::fs_winfsp
