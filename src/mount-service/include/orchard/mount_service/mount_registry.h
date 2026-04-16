#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "orchard/blockio/result.h"
#include "orchard/fs_winfsp/types.h"
#include "orchard/mount_service/types.h"

namespace orchard::mount_service {

class ManagedMountSession {
public:
  virtual ~ManagedMountSession() = default;

  [[nodiscard]] virtual std::wstring_view mount_point() const noexcept = 0;
  [[nodiscard]] virtual std::wstring_view volume_label() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t volume_object_id() const noexcept = 0;
  [[nodiscard]] virtual std::string_view volume_name() const noexcept = 0;
  [[nodiscard]] virtual MountedSessionPerformanceRecord performance() const noexcept = 0;
  virtual void Stop() noexcept = 0;
};

using ManagedMountSessionHandle = std::unique_ptr<ManagedMountSession>;

class MountSessionFactory {
public:
  virtual ~MountSessionFactory() = default;

  [[nodiscard]] virtual blockio::Result<ManagedMountSessionHandle>
  Start(const orchard::fs_winfsp::MountConfig& config) = 0;
};

std::unique_ptr<MountSessionFactory> CreateDefaultMountSessionFactory();

class MountRegistry {
public:
  explicit MountRegistry(
      std::unique_ptr<MountSessionFactory> factory = CreateDefaultMountSessionFactory());

  [[nodiscard]] blockio::Result<MountedSessionRecord> MountVolume(const MountRequest& request);
  [[nodiscard]] blockio::Result<MountedSessionRecord> GetMount(std::wstring_view mount_id) const;
  [[nodiscard]] std::vector<MountedSessionRecord> ListMounts() const;
  [[nodiscard]] blockio::Result<std::monostate> UnmountVolume(const UnmountRequest& request);

  void Shutdown() noexcept;

private:
  struct ActiveMount {
    MountedSessionRecord record;
    ManagedMountSessionHandle session;
  };

  [[nodiscard]] std::wstring AllocateMountId();
  [[nodiscard]] static MountedSessionRecord SnapshotActiveMount(const ActiveMount& active_mount);

  std::unique_ptr<MountSessionFactory> factory_;
  mutable std::mutex mutex_;
  std::unordered_map<std::wstring, ActiveMount> mounts_by_id_;
  std::unordered_map<std::wstring, std::wstring> mount_id_by_mount_point_;
  std::uint64_t next_mount_ordinal_ = 1;
  bool shutdown_ = false;
};

} // namespace orchard::mount_service
