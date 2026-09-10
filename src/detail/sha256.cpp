#include "ensemble_fabric/persistence.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace ensemble_fabric {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

[[nodiscard]] constexpr std::uint32_t rotate_right(std::uint32_t value,
                                                   std::uint32_t count) noexcept {
  return (value >> count) | (value << (32u - count));
}

class Sha256 {
 public:
  Sha256() noexcept
      : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu,
               0x1f83d9abu, 0x5be0cd19u} {}

  void update(std::span<const std::byte> data) noexcept {
    for (const std::byte raw : data) {
      buffer_[buffer_length_] = static_cast<std::uint8_t>(raw);
      ++buffer_length_;
      total_bytes_ += 1u;
      if (buffer_length_ == 64u) {
        compress(buffer_.data());
        buffer_length_ = 0u;
      }
    }
  }

  [[nodiscard]] std::array<std::uint8_t, 32> finish() noexcept {
    const std::uint64_t bit_length = total_bytes_ * 8u;
    const std::uint8_t pad = 0x80u;
    buffer_[buffer_length_] = pad;
    ++buffer_length_;
    if (buffer_length_ > 56u) {
      while (buffer_length_ < 64u) {
        buffer_[buffer_length_] = 0u;
        ++buffer_length_;
      }
      compress(buffer_.data());
      buffer_length_ = 0u;
    }
    while (buffer_length_ < 56u) {
      buffer_[buffer_length_] = 0u;
      ++buffer_length_;
    }
    for (std::uint32_t index = 0; index < 8u; ++index) {
      const std::uint32_t shift = 56u - (index * 8u);
      buffer_[56u + index] = static_cast<std::uint8_t>((bit_length >> shift) & 0xFFu);
    }
    compress(buffer_.data());

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t word = 0; word < 8u; ++word) {
      for (std::size_t byte = 0; byte < 4u; ++byte) {
        const std::uint32_t shift = 24u - static_cast<std::uint32_t>(byte) * 8u;
        digest[word * 4u + byte] = static_cast<std::uint8_t>((state_[word] >> shift) & 0xFFu);
      }
    }
    return digest;
  }

 private:
  void compress(const std::uint8_t* block) noexcept {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t index = 0; index < 16u; ++index) {
      schedule[index] = (static_cast<std::uint32_t>(block[index * 4u]) << 24u) |
                        (static_cast<std::uint32_t>(block[index * 4u + 1u]) << 16u) |
                        (static_cast<std::uint32_t>(block[index * 4u + 2u]) << 8u) |
                        static_cast<std::uint32_t>(block[index * 4u + 3u]);
    }
    for (std::size_t index = 16u; index < 64u; ++index) {
      const std::uint32_t s0 = rotate_right(schedule[index - 15u], 7u) ^
                               rotate_right(schedule[index - 15u], 18u) ^
                               (schedule[index - 15u] >> 3u);
      const std::uint32_t s1 = rotate_right(schedule[index - 2u], 17u) ^
                               rotate_right(schedule[index - 2u], 19u) ^
                               (schedule[index - 2u] >> 10u);
      schedule[index] = schedule[index - 16u] + s0 + schedule[index - 7u] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t index = 0; index < 64u; ++index) {
      const std::uint32_t sigma1 = rotate_right(e, 6u) ^ rotate_right(e, 11u) ^ rotate_right(e, 25u);
      const std::uint32_t choose = (e & f) ^ ((~e) & g);
      const std::uint32_t temp1 = h + sigma1 + choose + kRoundConstants[index] + schedule[index];
      const std::uint32_t sigma0 = rotate_right(a, 2u) ^ rotate_right(a, 13u) ^ rotate_right(a, 22u);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temp2 = sigma0 + majority;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffer_length_{0};
  std::uint64_t total_bytes_{0};
};

}  // namespace

std::array<std::uint8_t, 32> sha256(std::span<const std::byte> data) noexcept {
  Sha256 hasher;
  hasher.update(data);
  return hasher.finish();
}

std::uint64_t sha256_digest64(std::span<const std::byte> data) noexcept {
  const std::array<std::uint8_t, 32> digest = sha256(data);
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8u; ++index) {
    value = (value << 8u) | static_cast<std::uint64_t>(digest[index]);
  }
  return value;
}

}  // namespace ensemble_fabric
