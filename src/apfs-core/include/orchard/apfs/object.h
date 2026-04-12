#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "orchard/apfs/format.h"
#include "orchard/blockio/reader.h"

namespace orchard::apfs {

struct ObjectHeader {
  std::uint64_t checksum = 0;
  std::uint64_t oid = 0;
  std::uint64_t xid = 0;
  std::uint32_t raw_type = 0;
  std::uint32_t raw_subtype = 0;
  std::uint32_t type = 0;
  std::uint32_t subtype = 0;
};

struct ObjectBlock {
  std::uint64_t block_index = 0;
  ObjectHeader header;
  std::vector<std::uint8_t> bytes;

  [[nodiscard]] std::span<const std::uint8_t> view() const noexcept { return bytes; }
};

blockio::Result<ObjectHeader> ParseObjectHeader(std::span<const std::uint8_t> block);

class PhysicalObjectReader {
public:
  PhysicalObjectReader(const blockio::Reader& reader,
                       std::uint64_t container_byte_offset,
                       std::uint32_t block_size);

  [[nodiscard]] blockio::Result<ObjectBlock> ReadPhysicalObject(std::uint64_t block_index) const;
  [[nodiscard]] std::uint64_t container_byte_offset() const noexcept { return container_byte_offset_; }
  [[nodiscard]] std::uint32_t block_size() const noexcept { return block_size_; }

private:
  const blockio::Reader* reader_ = nullptr;
  std::uint64_t container_byte_offset_ = 0;
  std::uint32_t block_size_ = 0;
};

} // namespace orchard::apfs
