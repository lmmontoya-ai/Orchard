#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "orchard/blockio/inspection_target.h"
#include "orchard/blockio/result.h"

namespace orchard::blockio {

class Reader {
public:
  virtual ~Reader() = default;

  [[nodiscard]] virtual Result<std::uint64_t> size_bytes() const = 0;
  [[nodiscard]] virtual Result<std::size_t> ReadAt(std::uint64_t offset,
                                                   std::span<std::uint8_t> buffer) const = 0;
  [[nodiscard]] virtual std::string_view backend_name() const noexcept = 0;
  [[nodiscard]] virtual TargetKind target_kind() const noexcept = 0;
  [[nodiscard]] virtual const std::filesystem::path& path() const noexcept = 0;
};

using ReaderHandle = std::unique_ptr<Reader>;

struct ReadRequest {
  std::uint64_t offset = 0;
  std::size_t size = 0;
};

Result<ReaderHandle> OpenReader(const std::filesystem::path& path);
Result<ReaderHandle> OpenReader(const InspectionTargetInfo& target_info);
Result<std::vector<std::uint8_t>> ReadExact(const Reader& reader, ReadRequest request);
ReaderHandle MakeMemoryReader(std::vector<std::uint8_t> bytes, std::filesystem::path label = {});

} // namespace orchard::blockio
