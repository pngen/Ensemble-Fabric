#include "ensemble_fabric/persistence.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ensemble_fabric {
namespace {

// Reflected Castagnoli polynomial (CRC32C), as used by iSCSI and by the wire
// protocol and persistence container of this runtime.
constexpr std::uint32_t kPolynomial = 0x82F63B78u;

struct Crc32cTable {
  std::array<std::uint32_t, 256> entries{};

  constexpr Crc32cTable() noexcept {
    for (std::uint32_t index = 0; index < 256u; ++index) {
      std::uint32_t value = index;
      for (std::uint32_t bit = 0; bit < 8u; ++bit) {
        value = (value & 1u) != 0u ? (value >> 1u) ^ kPolynomial : (value >> 1u);
      }
      entries[index] = value;
    }
  }
};

constexpr Crc32cTable kTable{};

}  // namespace

std::uint32_t crc32c_extend(std::uint32_t seed, std::span<const std::byte> data) noexcept {
  std::uint32_t crc = ~seed;
  for (const std::byte raw : data) {
    const std::uint8_t byte = static_cast<std::uint8_t>(raw);
    crc = kTable.entries[(crc ^ byte) & 0xFFu] ^ (crc >> 8u);
  }
  return ~crc;
}

std::uint32_t crc32c(std::span<const std::byte> data) noexcept {
  return crc32c_extend(0u, data);
}

}  // namespace ensemble_fabric
