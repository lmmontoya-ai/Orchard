#include "orchard/blockio/reader.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

namespace orchard::blockio {
namespace {

class ScopedHandle {
public:
  ScopedHandle() = default;
  explicit ScopedHandle(HANDLE handle) : handle_(handle) {}

  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;

  ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.Release()) {}

  ScopedHandle& operator=(ScopedHandle&& other) noexcept {
    if (this == &other) {
      return *this;
    }

    Reset(other.Release());
    return *this;
  }

  ~ScopedHandle() {
    Reset(INVALID_HANDLE_VALUE);
  }

  [[nodiscard]] HANDLE get() const noexcept {
    return handle_;
  }
  [[nodiscard]] bool valid() const noexcept {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

  HANDLE Release() noexcept {
    const HANDLE released = handle_;
    handle_ = INVALID_HANDLE_VALUE;
    return released;
  }

  void Reset(HANDLE handle) noexcept {
    if (valid()) {
      ::CloseHandle(handle_);
    }
    handle_ = handle;
  }

private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

[[nodiscard]] Error MakeError(ErrorCode code, std::string message,
                              const std::uint32_t system_code = 0) {
  return Error{
      .code = code,
      .message = std::move(message),
      .system_code = system_code,
  };
}

[[nodiscard]] Error MakeWin32Error(const ErrorCode code, const DWORD system_code,
                                   const std::string_view message) {
  return MakeError(code, std::string(message), system_code);
}

[[nodiscard]] ErrorCode MapOpenErrorCode(const DWORD system_code) noexcept {
  switch (system_code) {
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
  case ERROR_INVALID_DRIVE:
    return ErrorCode::kNotFound;
  case ERROR_ACCESS_DENIED:
  case ERROR_SHARING_VIOLATION:
    return ErrorCode::kAccessDenied;
  default:
    return ErrorCode::kOpenFailed;
  }
}

class HandleReader final : public Reader {
public:
  HandleReader(ScopedHandle handle, std::filesystem::path path, const TargetKind target_kind,
               std::optional<std::uint64_t> known_size, std::optional<Error> size_error)
      : handle_(std::move(handle)), path_(std::move(path)), target_kind_(target_kind),
        known_size_(known_size), size_error_(std::move(size_error)) {}

  [[nodiscard]] Result<std::uint64_t> size_bytes() const override {
    if (known_size_.has_value()) {
      return *known_size_;
    }

    return size_error_.value_or(
        MakeError(ErrorCode::kNotImplemented, "Reader size is not available for this target."));
  }

  [[nodiscard]] Result<std::size_t> ReadAt(const std::uint64_t offset,
                                           std::span<std::uint8_t> buffer) const override {
    if (buffer.empty()) {
      return static_cast<std::size_t>(0);
    }

    if (buffer.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
      return MakeError(ErrorCode::kInvalidArgument,
                       "ReadAt only supports buffers up to DWORD size.");
    }

    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
    overlapped.OffsetHigh = static_cast<DWORD>((offset >> 32U) & 0xFFFFFFFFULL);

    DWORD bytes_read = 0;
    const BOOL read_ok = ::ReadFile(handle_.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                                    &bytes_read, &overlapped);

    if (read_ok == FALSE) {
      return MakeWin32Error(ErrorCode::kReadFailed, ::GetLastError(), "ReadAt failed.");
    }

    return static_cast<std::size_t>(bytes_read);
  }

  [[nodiscard]] std::string_view backend_name() const noexcept override {
    return "windows_handle";
  }

  [[nodiscard]] TargetKind target_kind() const noexcept override {
    return target_kind_;
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept override {
    return path_;
  }

private:
  ScopedHandle handle_;
  std::filesystem::path path_;
  TargetKind target_kind_ = TargetKind::kUnknown;
  std::optional<std::uint64_t> known_size_;
  std::optional<Error> size_error_;
};

class MemoryReader final : public Reader {
public:
  MemoryReader(std::vector<std::uint8_t> bytes, std::filesystem::path label)
      : bytes_(std::move(bytes)), label_(std::move(label)) {}

  [[nodiscard]] Result<std::uint64_t> size_bytes() const override {
    return static_cast<std::uint64_t>(bytes_.size());
  }

  [[nodiscard]] Result<std::size_t> ReadAt(const std::uint64_t offset,
                                           std::span<std::uint8_t> buffer) const override {
    if (offset > bytes_.size()) {
      return static_cast<std::size_t>(0);
    }

    const auto start = static_cast<std::size_t>(offset);
    const auto available = bytes_.size() - start;
    const auto read_size = std::min(buffer.size(), available);
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(start),
                static_cast<std::ptrdiff_t>(read_size), buffer.begin());
    return read_size;
  }

  [[nodiscard]] std::string_view backend_name() const noexcept override {
    return "memory";
  }

  [[nodiscard]] TargetKind target_kind() const noexcept override {
    return TargetKind::kRegularFile;
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept override {
    return label_;
  }

private:
  std::vector<std::uint8_t> bytes_;
  std::filesystem::path label_;
};

[[nodiscard]] Result<std::uint64_t> QuerySize(const HANDLE handle, const TargetKind target_kind) {
  if (target_kind == TargetKind::kRawDevice) {
    GET_LENGTH_INFORMATION disk_length{};
    DWORD bytes_returned = 0;
    if (::DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &disk_length,
                          sizeof(disk_length), &bytes_returned, nullptr) != FALSE) {
      return static_cast<std::uint64_t>(disk_length.Length.QuadPart);
    }

    const DWORD ioctl_error = ::GetLastError();

    LARGE_INTEGER file_size{};
    if (::GetFileSizeEx(handle, &file_size) != FALSE) {
      return static_cast<std::uint64_t>(file_size.QuadPart);
    }

    return MakeWin32Error(ErrorCode::kIoctlFailed, ioctl_error,
                          "Failed to query raw-device size via IOCTL_DISK_GET_LENGTH_INFO.");
  }

  LARGE_INTEGER file_size{};
  if (::GetFileSizeEx(handle, &file_size) == FALSE) {
    return MakeWin32Error(ErrorCode::kOpenFailed, ::GetLastError(), "Failed to query file size.");
  }

  return static_cast<std::uint64_t>(file_size.QuadPart);
}

} // namespace

Result<ReaderHandle> OpenReader(const std::filesystem::path& path) {
  return OpenReader(InspectTargetPath(path));
}

Result<ReaderHandle> OpenReader(const InspectionTargetInfo& target_info) {
  switch (target_info.kind) {
  case TargetKind::kMissing:
    return MakeError(ErrorCode::kNotFound,
                     "Target path does not exist or is not a supported raw-device path.");
  case TargetKind::kDirectory:
    return MakeError(ErrorCode::kUnsupportedTarget, "Directories are not valid block I/O targets.");
  case TargetKind::kUnknown:
    return MakeError(ErrorCode::kUnsupportedTarget,
                     "Target kind is not supported by the block I/O layer.");
  case TargetKind::kRegularFile:
  case TargetKind::kRawDevice:
    break;
  }

  const DWORD desired_access = GENERIC_READ;
  const DWORD share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
  const DWORD creation_disposition = OPEN_EXISTING;
  const DWORD flags_and_attributes = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS;

  ScopedHandle handle(::CreateFileW(target_info.path.c_str(), desired_access, share_mode, nullptr,
                                    creation_disposition, flags_and_attributes, nullptr));

  if (!handle.valid()) {
    const DWORD system_code = ::GetLastError();
    return MakeWin32Error(MapOpenErrorCode(system_code), system_code,
                          "Failed to open block I/O target.");
  }

  auto size_result = QuerySize(handle.get(), target_info.kind);
  std::optional<std::uint64_t> known_size;
  std::optional<Error> size_error;
  if (size_result.ok()) {
    known_size = size_result.value();
  } else {
    size_error = size_result.error();
    if (target_info.kind == TargetKind::kRegularFile) {
      return size_result.error();
    }
  }

  return ReaderHandle(std::make_unique<HandleReader>(std::move(handle), target_info.path,
                                                     target_info.kind, known_size, size_error));
}

Result<std::vector<std::uint8_t>> ReadExact(const Reader& reader, const ReadRequest request) {
  std::vector<std::uint8_t> bytes(request.size);
  if (request.size == 0U) {
    return bytes;
  }

  auto read_result = reader.ReadAt(request.offset, bytes);
  if (!read_result.ok()) {
    return read_result.error();
  }

  if (read_result.value() != request.size) {
    return MakeError(ErrorCode::kShortRead, "ReadAt returned fewer bytes than requested.");
  }

  return bytes;
}

ReaderHandle MakeMemoryReader(std::vector<std::uint8_t> bytes, std::filesystem::path label) {
  return std::make_unique<MemoryReader>(std::move(bytes), std::move(label));
}

} // namespace orchard::blockio
