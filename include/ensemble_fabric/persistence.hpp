#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ensemble_fabric/ids.hpp"
#include "ensemble_fabric/limits.hpp"
#include "ensemble_fabric/outcome.hpp"

namespace ensemble_fabric {

/// Bounded binary encoder.  Every length is checked against the configured
/// limits before allocation so that untrusted declared sizes can never drive an
/// allocation.
class Encoder {
 public:
  explicit Encoder(const Limits& limits = Limits::defaults(), std::size_t reserve_bytes = 256);

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void boolean(bool value);
  void f64(double value);
  void bytes(std::span<const std::byte> value);
  void string(std::string_view value);
  void id64(std::uint64_t value) { u64(value); }

  /// Appends raw bytes without a length prefix.  Used for fixed-size fields.
  void raw(std::span<const std::byte> value);

  [[nodiscard]] const std::vector<std::byte>& buffer() const noexcept { return buffer_; }
  [[nodiscard]] std::vector<std::byte> take() noexcept { return std::move(buffer_); }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }

 private:
  void require_capacity(std::size_t additional);

  Limits limits_;
  std::vector<std::byte> buffer_;
};

/// Bounded binary decoder for untrusted input.  Every failure is typed; a
/// truncated or oversized payload never yields a partial object.
class Decoder {
 public:
  Decoder(std::span<const std::byte> data, const Limits& limits = Limits::defaults());

  [[nodiscard]] Outcome<std::uint8_t> u8();
  [[nodiscard]] Outcome<std::uint16_t> u16();
  [[nodiscard]] Outcome<std::uint32_t> u32();
  [[nodiscard]] Outcome<std::uint64_t> u64();
  [[nodiscard]] Outcome<std::int64_t> i64();
  [[nodiscard]] Outcome<bool> boolean();
  [[nodiscard]] Outcome<double> f64();
  [[nodiscard]] Outcome<std::vector<std::byte>> bytes(std::uint32_t max_bytes);
  [[nodiscard]] Outcome<std::string> string(std::uint32_t max_bytes);
  [[nodiscard]] Outcome<std::vector<std::byte>> raw(std::size_t count);

  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] bool exhausted() const noexcept { return offset_ == data_.size(); }

  /// Rejects trailing data according to the caller's policy.  Used everywhere a
  /// payload must be consumed exactly.
  [[nodiscard]] Status require_exhausted(std::string_view context) const;

 private:
  [[nodiscard]] bool have(std::size_t count) const noexcept;

  std::span<const std::byte> data_;
  std::size_t offset_{0};
  Limits limits_;
};

/// On-disk container header.  The container is versioned, integrity-checked, and
/// length-bounded before any record is decoded.
struct PersistenceHeader {
  std::uint32_t magic{0};
  std::uint16_t format_version{0};
  std::uint16_t flags{0};
  std::uint64_t payload_bytes{0};
  std::uint32_t payload_crc32c{0};
  std::uint32_t record_count{0};
  std::uint64_t content_digest{0};
};

inline constexpr std::uint32_t persistence_magic = 0x46424E45u;  // 'ENBF' little-endian

/// Writes a state file atomically: a unique temporary file in the same
/// directory is written, flushed, and renamed over the destination.  A crash can
/// therefore never leave a half-written authoritative state.
[[nodiscard]] Status write_state_file(const std::filesystem::path& path,
                                      std::span<const std::byte> payload,
                                      std::uint32_t record_count,
                                      const Limits& limits);

/// Reads and integrity-checks a state file.  Rejects bad magic, unsupported
/// format versions, checksum mismatches, truncation, oversized declarations, and
/// trailing bytes.
[[nodiscard]] Outcome<std::vector<std::byte>> read_state_file(
    const std::filesystem::path& path, PersistenceHeader& header, const Limits& limits);

/// CRC32C (Castagnoli) used by both persistence and the wire protocol.
[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> data) noexcept;
[[nodiscard]] std::uint32_t crc32c_extend(std::uint32_t seed,
                                          std::span<const std::byte> data) noexcept;

/// SHA-256 of a byte range, used for content digests.
[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::span<const std::byte> data) noexcept;
[[nodiscard]] std::uint64_t sha256_digest64(std::span<const std::byte> data) noexcept;

/// Removes a directory tree, ignoring absence.  Used by recovery tests and by
/// the tools to clean their own scratch state.
[[nodiscard]] Status remove_tree_if_present(const std::filesystem::path& path);

}  // namespace ensemble_fabric
