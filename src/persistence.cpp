#include "ensemble_fabric/persistence.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>

#include "ensemble_fabric/version.hpp"

#include "detail/text.hpp"

namespace ensemble_fabric {
namespace {

inline constexpr std::size_t header_bytes = 40;

void store_u16(std::byte* out, std::uint16_t value) {
  out[0] = static_cast<std::byte>(value & 0xFFu);
  out[1] = static_cast<std::byte>((value >> 8u) & 0xFFu);
}

void store_u32(std::byte* out, std::uint32_t value) {
  for (std::size_t index = 0; index < 4u; ++index) {
    out[index] = static_cast<std::byte>((value >> (index * 8u)) & 0xFFu);
  }
}

void store_u64(std::byte* out, std::uint64_t value) {
  for (std::size_t index = 0; index < 8u; ++index) {
    out[index] = static_cast<std::byte>((value >> (index * 8u)) & 0xFFu);
  }
}

[[nodiscard]] std::uint16_t load_u16(const std::byte* in) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[0]) |
                                    (static_cast<std::uint16_t>(in[1]) << 8u));
}

[[nodiscard]] std::uint32_t load_u32(const std::byte* in) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4u; ++index) {
    value |= static_cast<std::uint32_t>(in[index]) << (index * 8u);
  }
  return value;
}

[[nodiscard]] std::uint64_t load_u64(const std::byte* in) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8u; ++index) {
    value |= static_cast<std::uint64_t>(in[index]) << (index * 8u);
  }
  return value;
}

}  // namespace

Encoder::Encoder(const Limits& limits, std::size_t reserve_bytes) : limits_(limits) {
  buffer_.reserve(reserve_bytes);
}

void Encoder::require_capacity(std::size_t additional) {
  const std::size_t cap = static_cast<std::size_t>(limits_.max_persistence_bytes);
  if (additional > cap || buffer_.size() > cap - additional) {
    // The bounded encoder refuses to grow past the configured limit rather than
    // relying on the allocator to fail.
    buffer_.resize(cap);
  }
}

void Encoder::raw(std::span<const std::byte> value) {
  require_capacity(value.size());
  buffer_.insert(buffer_.end(), value.begin(), value.end());
}

void Encoder::u8(std::uint8_t value) { buffer_.push_back(static_cast<std::byte>(value)); }

void Encoder::u16(std::uint16_t value) {
  std::byte temp[2] = {};
  store_u16(temp, value);
  raw(std::span<const std::byte>(temp, 2));
}

void Encoder::u32(std::uint32_t value) {
  std::byte temp[4] = {};
  store_u32(temp, value);
  raw(std::span<const std::byte>(temp, 4));
}

void Encoder::u64(std::uint64_t value) {
  std::byte temp[8] = {};
  store_u64(temp, value);
  raw(std::span<const std::byte>(temp, 8));
}

void Encoder::i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

void Encoder::boolean(bool value) { u8(value ? 1u : 0u); }

void Encoder::f64(double value) {
  std::uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value), "double must be 64-bit");
  std::memcpy(&bits, &value, sizeof(bits));
  u64(bits);
}

void Encoder::bytes(std::span<const std::byte> value) {
  u32(static_cast<std::uint32_t>(value.size()));
  raw(value);
}

void Encoder::string(std::string_view value) {
  u32(static_cast<std::uint32_t>(value.size()));
  raw(std::span<const std::byte>(reinterpret_cast<const std::byte*>(value.data()), value.size()));
}

Decoder::Decoder(std::span<const std::byte> data, const Limits& limits)
    : data_(data), limits_(limits) {}

bool Decoder::have(std::size_t count) const noexcept {
  return count <= data_.size() && offset_ <= data_.size() - count;
}

Outcome<std::uint8_t> Decoder::u8() {
  if (!have(1u)) {
    return fail<std::uint8_t>(EnsembleError::malformed_persistence, "truncated: expected a byte");
  }
  const std::uint8_t value = static_cast<std::uint8_t>(data_[offset_]);
  offset_ += 1u;
  return value;
}

Outcome<std::uint16_t> Decoder::u16() {
  if (!have(2u)) {
    return fail<std::uint16_t>(EnsembleError::malformed_persistence, "truncated: expected u16");
  }
  const std::uint16_t value = load_u16(data_.data() + offset_);
  offset_ += 2u;
  return value;
}

Outcome<std::uint32_t> Decoder::u32() {
  if (!have(4u)) {
    return fail<std::uint32_t>(EnsembleError::malformed_persistence, "truncated: expected u32");
  }
  const std::uint32_t value = load_u32(data_.data() + offset_);
  offset_ += 4u;
  return value;
}

Outcome<std::uint64_t> Decoder::u64() {
  if (!have(8u)) {
    return fail<std::uint64_t>(EnsembleError::malformed_persistence, "truncated: expected u64");
  }
  const std::uint64_t value = load_u64(data_.data() + offset_);
  offset_ += 8u;
  return value;
}

Outcome<std::int64_t> Decoder::i64() {
  Outcome<std::uint64_t> value = u64();
  if (!value.ok()) {
    return fail<std::int64_t>(value.code(), value.error().message);
  }
  return static_cast<std::int64_t>(value.value());
}

Outcome<bool> Decoder::boolean() {
  Outcome<std::uint8_t> value = u8();
  if (!value.ok()) {
    return fail<bool>(value.code(), value.error().message);
  }
  if (value.value() > 1u) {
    return fail<bool>(EnsembleError::malformed_persistence, "boolean field holds an invalid value");
  }
  return value.value() == 1u;
}

Outcome<double> Decoder::f64() {
  Outcome<std::uint64_t> bits = u64();
  if (!bits.ok()) {
    return fail<double>(bits.code(), bits.error().message);
  }
  double value = 0.0;
  const std::uint64_t raw = bits.value();
  std::memcpy(&value, &raw, sizeof(value));
  return value;
}

Outcome<std::vector<std::byte>> Decoder::raw(std::size_t count) {
  if (!have(count)) {
    return fail<std::vector<std::byte>>(EnsembleError::malformed_persistence,
                                        "truncated: declared length exceeds the remaining input");
  }
  std::vector<std::byte> out(data_.begin() + static_cast<std::ptrdiff_t>(offset_),
                             data_.begin() + static_cast<std::ptrdiff_t>(offset_ + count));
  offset_ += count;
  return out;
}

Outcome<std::vector<std::byte>> Decoder::bytes(std::uint32_t max_bytes) {
  Outcome<std::uint32_t> length = u32();
  if (!length.ok()) {
    return fail<std::vector<std::byte>>(length.code(), length.error().message);
  }
  if (length.value() > max_bytes) {
    return fail<std::vector<std::byte>>(EnsembleError::payload_too_large,
                                        "declared byte string exceeds the configured bound");
  }
  return raw(length.value());
}

Outcome<std::string> Decoder::string(std::uint32_t max_bytes) {
  Outcome<std::uint32_t> length = u32();
  if (!length.ok()) {
    return fail<std::string>(length.code(), length.error().message);
  }
  if (length.value() > max_bytes) {
    return fail<std::string>(EnsembleError::payload_too_large,
                             "declared string exceeds the configured bound");
  }
  Outcome<std::vector<std::byte>> content = raw(length.value());
  if (!content.ok()) {
    return fail<std::string>(content.code(), content.error().message);
  }
  const std::vector<std::byte>& value = content.value();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

Status Decoder::require_exhausted(std::string_view context) const {
  if (!exhausted()) {
    return fail(EnsembleError::trailing_data,
                std::string(context) + ": " + std::to_string(remaining()) +
                    " trailing byte(s) after the declared content");
  }
  return ok_status();
}

Status write_state_file(const std::filesystem::path& path, std::span<const std::byte> payload,
                        std::uint32_t record_count, const Limits& limits) {
  if (payload.size() > limits.max_persistence_bytes) {
    return fail(EnsembleError::resource_limit_exceeded,
                "state payload exceeds the configured persistence bound");
  }
  std::vector<std::byte> container(header_bytes + payload.size());
  const std::uint32_t payload_crc = crc32c(payload);
  const std::uint64_t digest = sha256_digest64(payload);
  store_u32(container.data() + 0, persistence_magic);
  store_u16(container.data() + 4, static_cast<std::uint16_t>(persistence_format_version));
  store_u16(container.data() + 6, 0u);
  store_u64(container.data() + 8, payload.size());
  store_u32(container.data() + 16, payload_crc);
  store_u32(container.data() + 20, record_count);
  store_u64(container.data() + 24, digest);
  store_u32(container.data() + 32, 0u);
  store_u32(container.data() + 36, 0u);
  // Header integrity covers the first 36 bytes with the checksum field zeroed.
  const std::uint32_t header_crc =
      crc32c(std::span<const std::byte>(container.data(), header_bytes - 4u));
  store_u32(container.data() + 36, header_crc);
  std::copy(payload.begin(), payload.end(), container.begin() + static_cast<std::ptrdiff_t>(header_bytes));

  std::error_code error;
  const std::filesystem::path directory = path.parent_path();
  if (!directory.empty() && !std::filesystem::exists(directory, error)) {
    std::filesystem::create_directories(directory, error);
    if (error) {
      return fail(EnsembleError::persistence_io_error,
                  "cannot create the state directory: " + error.message());
    }
  }
  // A unique temporary file in the same directory keeps replacement atomic: a
  // crash can leave a temporary file behind, never a half-written state file.
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t serial = counter.fetch_add(1u) + 1u;
  std::filesystem::path temporary = path;
  temporary += ".tmp-" + std::to_string(serial);

  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
      return fail(EnsembleError::persistence_io_error,
                  "cannot open the temporary state file for writing");
    }
    stream.write(reinterpret_cast<const char*>(container.data()),
                 static_cast<std::streamsize>(container.size()));
    stream.flush();
    if (!stream) {
      stream.close();
      std::filesystem::remove(temporary, error);
      return fail(EnsembleError::persistence_io_error, "failed to write the state file");
    }
  }
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return fail(EnsembleError::persistence_io_error,
                "atomic replacement of the state file failed: " + error.message());
  }
  return ok_status();
}

Outcome<std::vector<std::byte>> read_state_file(const std::filesystem::path& path,
                                                PersistenceHeader& header, const Limits& limits) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) {
    return fail<std::vector<std::byte>>(EnsembleError::persistence_io_error,
                                        "cannot stat the state file: " + error.message());
  }
  if (size < header_bytes) {
    return fail<std::vector<std::byte>>(EnsembleError::persistence_corruption,
                                        "state file is smaller than its header");
  }
  if (size > static_cast<std::uintmax_t>(header_bytes) + limits.max_persistence_bytes) {
    return fail<std::vector<std::byte>>(EnsembleError::resource_limit_exceeded,
                                        "state file exceeds the configured persistence bound");
  }
  std::vector<std::byte> container(static_cast<std::size_t>(size));
  {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
      return fail<std::vector<std::byte>>(EnsembleError::persistence_io_error,
                                          "cannot open the state file for reading");
    }
    stream.read(reinterpret_cast<char*>(container.data()),
                static_cast<std::streamsize>(container.size()));
    if (stream.gcount() != static_cast<std::streamsize>(container.size())) {
      return fail<std::vector<std::byte>>(EnsembleError::persistence_corruption,
                                          "state file is truncated");
    }
  }

  header.magic = load_u32(container.data() + 0);
  header.format_version = load_u16(container.data() + 4);
  header.flags = load_u16(container.data() + 6);
  header.payload_bytes = load_u64(container.data() + 8);
  header.payload_crc32c = load_u32(container.data() + 16);
  header.record_count = load_u32(container.data() + 20);
  header.content_digest = load_u64(container.data() + 24);
  const std::uint32_t stored_header_crc = load_u32(container.data() + 36);

  if (header.magic != persistence_magic) {
    return fail<std::vector<std::byte>>(EnsembleError::malformed_persistence,
                                        "state file magic does not match");
  }
  if (header.format_version != persistence_format_version) {
    return fail<std::vector<std::byte>>(
        EnsembleError::unsupported_version,
        "state file format version " + std::to_string(header.format_version) +
            " is not supported by this build (expected " +
            std::to_string(persistence_format_version) + ")");
  }
  std::vector<std::byte> header_copy(container.begin(),
                                     container.begin() + static_cast<std::ptrdiff_t>(header_bytes));
  store_u32(header_copy.data() + 36, 0u);
  const std::uint32_t computed_header_crc =
      crc32c(std::span<const std::byte>(header_copy.data(), header_bytes - 4u));
  if (computed_header_crc != stored_header_crc) {
    return fail<std::vector<std::byte>>(EnsembleError::checksum_mismatch,
                                        "state file header checksum does not match");
  }
  if (header.payload_bytes > limits.max_persistence_bytes) {
    return fail<std::vector<std::byte>>(EnsembleError::resource_limit_exceeded,
                                        "declared state payload exceeds the configured bound");
  }
  const std::uintmax_t expected = static_cast<std::uintmax_t>(header_bytes) + header.payload_bytes;
  if (static_cast<std::uintmax_t>(size) < expected) {
    return fail<std::vector<std::byte>>(EnsembleError::persistence_corruption,
                                        "state file is truncated relative to its declared length");
  }
  if (static_cast<std::uintmax_t>(size) > expected) {
    return fail<std::vector<std::byte>>(EnsembleError::trailing_data,
                                        "state file carries trailing bytes after its payload");
  }
  if (header.record_count > limits.max_records_per_snapshot) {
    return fail<std::vector<std::byte>>(EnsembleError::resource_limit_exceeded,
                                        "declared record count exceeds the configured bound");
  }
  std::vector<std::byte> payload(
      container.begin() + static_cast<std::ptrdiff_t>(header_bytes), container.end());
  if (crc32c(std::span<const std::byte>(payload.data(), payload.size())) != header.payload_crc32c) {
    return fail<std::vector<std::byte>>(EnsembleError::checksum_mismatch,
                                        "state file payload checksum does not match");
  }
  if (sha256_digest64(std::span<const std::byte>(payload.data(), payload.size())) !=
      header.content_digest) {
    return fail<std::vector<std::byte>>(EnsembleError::checksum_mismatch,
                                        "state file content digest does not match");
  }
  return payload;
}

Status remove_tree_if_present(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    return ok_status();
  }
  std::filesystem::remove_all(path, error);
  if (error) {
    return fail(EnsembleError::persistence_io_error,
                "cannot remove '" + path.string() + "': " + error.message());
  }
  return ok_status();
}

}  // namespace ensemble_fabric
