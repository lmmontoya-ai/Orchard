#include "orchard/mount_service/mount_registry.h"

#include <algorithm>
#include <cwctype>
#include <utility>

#include "orchard/fs_winfsp/mount.h"

namespace orchard::mount_service {
namespace {

class WinFspManagedMountSession final : public ManagedMountSession {
public:
  explicit WinFspManagedMountSession(orchard::fs_winfsp::MountSessionHandle session)
      : session_(std::move(session)), mount_point_(session_->mount_point()),
        volume_label_(session_->mounted_volume().volume_label()),
        volume_object_id_(session_->mounted_volume().volume_info().object_id),
        volume_name_(session_->mounted_volume().volume_info().name) {}

  [[nodiscard]] std::wstring_view mount_point() const noexcept override {
    return mount_point_;
  }
  [[nodiscard]] std::wstring_view volume_label() const noexcept override {
    return volume_label_;
  }
  [[nodiscard]] std::uint64_t volume_object_id() const noexcept override {
    return volume_object_id_;
  }
  [[nodiscard]] std::string_view volume_name() const noexcept override {
    return volume_name_;
  }
  [[nodiscard]] MountedSessionPerformanceRecord performance() const noexcept override {
    return MountedSessionPerformanceRecord{
        .apfs = session_->mounted_volume().volume_context().performance_stats(),
        .mounted_volume = session_->mounted_volume().performance_stats(),
        .callbacks = session_->performance_stats(),
    };
  }

  void Stop() noexcept override {
    if (session_) {
      session_->Stop();
    }
  }

private:
  orchard::fs_winfsp::MountSessionHandle session_;
  std::wstring mount_point_;
  std::wstring volume_label_;
  std::uint64_t volume_object_id_ = 0;
  std::string volume_name_;
};

class DefaultMountSessionFactory final : public MountSessionFactory {
public:
  [[nodiscard]] blockio::Result<ManagedMountSessionHandle>
  Start(const orchard::fs_winfsp::MountConfig& config) override {
    if (!orchard::fs_winfsp::HasWinFspSupport()) {
      return MakeMountServiceError(
          blockio::ErrorCode::kUnsupportedTarget,
          "The Orchard WinFsp backend is not available for mount-service operations.");
    }

    auto session_result = orchard::fs_winfsp::StartMount(config);
    if (!session_result.ok()) {
      return session_result.error();
    }

    auto session = std::move(session_result.value());
    ManagedMountSessionHandle wrapped(new WinFspManagedMountSession(std::move(session)));
    return wrapped;
  }
};

std::wstring NormalizeMountPointKey(std::wstring_view mount_point) {
  std::wstring normalized(mount_point);
  for (auto& ch : normalized) {
    ch = static_cast<wchar_t>(std::towupper(ch));
  }
  return normalized;
}

} // namespace

std::unique_ptr<MountSessionFactory> CreateDefaultMountSessionFactory() {
  return std::make_unique<DefaultMountSessionFactory>();
}

MountRegistry::MountRegistry(std::unique_ptr<MountSessionFactory> factory)
    : factory_(std::move(factory)) {
  if (!factory_) {
    factory_ = CreateDefaultMountSessionFactory();
  }
}

std::wstring MountRegistry::AllocateMountId() {
  return L"mount-" + std::to_wstring(next_mount_ordinal_++);
}

MountedSessionRecord MountRegistry::SnapshotActiveMount(const ActiveMount& active_mount) {
  auto snapshot = active_mount.record;
  if (active_mount.session) {
    snapshot.performance = active_mount.session->performance();
  }
  return snapshot;
}

blockio::Result<MountedSessionRecord> MountRegistry::MountVolume(const MountRequest& request) {
  if (request.config.target_path.empty()) {
    return MakeMountServiceError(blockio::ErrorCode::kInvalidArgument,
                                 "Mount requests require a target path.");
  }
  if (request.config.mount_point.empty()) {
    return MakeMountServiceError(blockio::ErrorCode::kInvalidArgument,
                                 "Mount requests require a mount point.");
  }

  std::wstring mount_id;
  {
    std::scoped_lock lock(mutex_);
    if (shutdown_) {
      return MakeMountServiceError(blockio::ErrorCode::kAccessDenied,
                                   "The Orchard mount registry is shutting down.");
    }

    mount_id = request.mount_id.value_or(AllocateMountId());
    if (mounts_by_id_.contains(mount_id)) {
      return MakeMountServiceError(blockio::ErrorCode::kInvalidArgument,
                                   "The requested mount ID is already active.");
    }

    const auto mount_point_key = NormalizeMountPointKey(request.config.mount_point);
    if (mount_id_by_mount_point_.contains(mount_point_key)) {
      return MakeMountServiceError(blockio::ErrorCode::kInvalidArgument,
                                   "The requested mount point is already active.");
    }
  }

  auto session_result = factory_->Start(request.config);
  if (!session_result.ok()) {
    return session_result.error();
  }

  MountedSessionRecord record{
      .mount_id = mount_id,
      .target_path = request.config.target_path,
      .mount_point = std::wstring(session_result.value()->mount_point()),
      .volume_object_id = session_result.value()->volume_object_id(),
      .volume_name = std::string(session_result.value()->volume_name()),
      .volume_label = std::wstring(session_result.value()->volume_label()),
      .read_only = request.config.require_read_only_mount,
      .performance = {},
  };

  {
    std::scoped_lock lock(mutex_);
    if (shutdown_) {
      session_result.value()->Stop();
      return MakeMountServiceError(blockio::ErrorCode::kAccessDenied,
                                   "The Orchard mount registry is shutting down.");
    }

    const auto mount_point_key = NormalizeMountPointKey(record.mount_point);
    if (mounts_by_id_.contains(record.mount_id) ||
        mount_id_by_mount_point_.contains(mount_point_key)) {
      session_result.value()->Stop();
      return MakeMountServiceError(blockio::ErrorCode::kInvalidArgument,
                                   "The requested mount identity became active while mounting.");
    }

    mounts_by_id_.emplace(record.mount_id, ActiveMount{
                                               .record = record,
                                               .session = std::move(session_result.value()),
                                           });
    mount_id_by_mount_point_.emplace(mount_point_key, record.mount_id);
  }

  return record;
}

blockio::Result<MountedSessionRecord>
MountRegistry::GetMount(const std::wstring_view mount_id) const {
  std::scoped_lock lock(mutex_);
  const auto found = mounts_by_id_.find(std::wstring(mount_id));
  if (found == mounts_by_id_.end()) {
    return MakeMountServiceError(blockio::ErrorCode::kNotFound,
                                 "The requested Orchard mount is not active.");
  }

  return SnapshotActiveMount(found->second);
}

std::vector<MountedSessionRecord> MountRegistry::ListMounts() const {
  std::vector<MountedSessionRecord> mounts;

  std::scoped_lock lock(mutex_);
  mounts.reserve(mounts_by_id_.size());
  for (const auto& [mount_id, active_mount] : mounts_by_id_) {
    (void)mount_id;
    mounts.push_back(SnapshotActiveMount(active_mount));
  }

  std::sort(mounts.begin(), mounts.end(),
            [](const MountedSessionRecord& left, const MountedSessionRecord& right) {
              return left.mount_id < right.mount_id;
            });
  return mounts;
}

blockio::Result<std::monostate> MountRegistry::UnmountVolume(const UnmountRequest& request) {
  ManagedMountSessionHandle session;

  {
    std::scoped_lock lock(mutex_);
    const auto found = mounts_by_id_.find(request.mount_id);
    if (found == mounts_by_id_.end()) {
      return MakeMountServiceError(blockio::ErrorCode::kNotFound,
                                   "The requested Orchard mount is not active.");
    }

    mount_id_by_mount_point_.erase(NormalizeMountPointKey(found->second.record.mount_point));
    session = std::move(found->second.session);
    mounts_by_id_.erase(found);
  }

  if (session) {
    session->Stop();
  }

  return std::monostate{};
}

void MountRegistry::Shutdown() noexcept {
  std::vector<ManagedMountSessionHandle> sessions;

  {
    std::scoped_lock lock(mutex_);
    shutdown_ = true;
    sessions.reserve(mounts_by_id_.size());
    for (auto& [mount_id, active_mount] : mounts_by_id_) {
      (void)mount_id;
      sessions.push_back(std::move(active_mount.session));
    }

    mounts_by_id_.clear();
    mount_id_by_mount_point_.clear();
  }

  for (auto& session : sessions) {
    if (session) {
      session->Stop();
    }
  }
}

} // namespace orchard::mount_service
