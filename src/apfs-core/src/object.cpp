#include "orchard/apfs/object.h"

#include <limits>

namespace orchard::apfs {

blockio::Result<ObjectHeader> ParseObjectHeader(const std::span<const std::uint8_t> block) {
  if (!HasRange(block, 0U, kApfsObjectHeaderSize)) {
    return MakeApfsError(blockio::ErrorCode::kShortRead,
                         "APFS object block is too small for the common object header.");
  }

  ObjectHeader header;
  header.checksum = ReadLe64(block, 0x00U);
  header.oid = ReadLe64(block, 0x08U);
  header.xid = ReadLe64(block, 0x10U);
  header.raw_type = ReadLe32(block, 0x18U);
  header.raw_subtype = ReadLe32(block, 0x1CU);
  header.type = header.raw_type & kObjectTypeMask;
  header.subtype = header.raw_subtype & kObjectTypeMask;
  return header;
}

PhysicalObjectReader::PhysicalObjectReader(const blockio::Reader& reader,
                                           const std::uint64_t container_byte_offset,
                                           const std::uint32_t block_size)
    : reader_(&reader), container_byte_offset_(container_byte_offset), block_size_(block_size) {}

blockio::Result<ObjectBlock> PhysicalObjectReader::ReadPhysicalObject(
    const std::uint64_t block_index) const {
  if (reader_ == nullptr || block_size_ == 0U) {
    return MakeApfsError(blockio::ErrorCode::kInvalidArgument,
                         "Physical object reader is not configured with a valid APFS block size.");
  }

  constexpr auto kMaxUint64 = std::numeric_limits<std::uint64_t>::max();
  if (block_index > (kMaxUint64 - container_byte_offset_) / block_size_) {
    return MakeApfsError(blockio::ErrorCode::kOutOfRange,
                         "Physical object read would overflow the container byte range.");
  }

  const auto byte_offset = container_byte_offset_ + (block_index * block_size_);
  auto block_bytes_result = blockio::ReadExact(
      *reader_, blockio::ReadRequest{.offset = byte_offset, .size = block_size_});
  if (!block_bytes_result.ok()) {
    return block_bytes_result.error();
  }

  auto header_result = ParseObjectHeader(block_bytes_result.value());
  if (!header_result.ok()) {
    return header_result.error();
  }

  ObjectBlock object;
  object.block_index = block_index;
  object.header = header_result.value();
  object.bytes = std::move(block_bytes_result.value());
  return object;
}

} // namespace orchard::apfs
