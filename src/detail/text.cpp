#include "detail/text.hpp"

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

namespace ensemble_fabric::detail {

bool is_bounded_ascii(std::string_view text, std::size_t max_bytes) noexcept {
  if (text.size() > max_bytes) {
    return false;
  }
  for (const char ch : text) {
    const unsigned char byte = static_cast<unsigned char>(ch);
    if (byte == 0x09u || byte == 0x0Au) {
      continue;
    }
    if (byte < 0x20u || byte > 0x7Eu) {
      return false;
    }
  }
  return true;
}

std::string sanitize_text(std::string_view text, std::size_t max_bytes) {
  std::string out;
  out.reserve(text.size() < max_bytes ? text.size() : max_bytes);
  for (const char ch : text) {
    if (out.size() >= max_bytes) {
      break;
    }
    const unsigned char byte = static_cast<unsigned char>(ch);
    if (byte == 0x09u) {
      out.push_back(' ');
    } else if (byte >= 0x20u && byte <= 0x7Eu) {
      out.push_back(static_cast<char>(byte));
    } else {
      out.push_back('?');
    }
  }
  return out;
}

bool parse_u32(std::string_view text, std::uint32_t& out) noexcept {
  std::uint64_t value = 0;
  if (!parse_u64(text, value) || value > 0xFFFFFFFFull) {
    return false;
  }
  out = static_cast<std::uint32_t>(value);
  return true;
}

bool parse_u64(std::string_view text, std::uint64_t& out) noexcept {
  if (text.empty() || text.size() > 20u) {
    return false;
  }
  std::uint64_t accumulator = 0;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
    if (accumulator > (std::numeric_limits<std::uint64_t>::max() - digit) / 10ull) {
      return false;
    }
    accumulator = accumulator * 10ull + digit;
  }
  out = accumulator;
  return true;
}

bool parse_finite_double(std::string_view text, double& out) noexcept {
  if (text.empty() || text.size() > 64u) {
    return false;
  }
  std::string copy(text);
  errno = 0;
  char* end = nullptr;
  const double value = std::strtod(copy.c_str(), &end);
  if (end == nullptr || end == copy.c_str() || *end != '\0') {
    return false;
  }
  if (errno == ERANGE && (value == 0.0 || std::fabs(value) == std::numeric_limits<double>::infinity())) {
    return false;
  }
  if (!std::isfinite(value)) {
    return false;
  }
  out = value;
  return true;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
  std::string out;
  for (std::size_t index = 0; index < parts.size(); ++index) {
    if (index != 0u) {
      out.append(separator);
    }
    out.append(parts[index]);
  }
  return out;
}

std::uint64_t fnv1a64(std::string_view text) noexcept {
  return fnv1a64_extend(1469598103934665603ull, text);
}

std::uint64_t fnv1a64_extend(std::uint64_t seed, std::string_view text) noexcept {
  std::uint64_t hash = seed;
  for (const char ch : text) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
    hash *= 1099511628211ull;
  }
  return hash;
}

std::uint64_t fnv1a64_extend(std::uint64_t seed, std::uint64_t value) noexcept {
  std::uint64_t hash = seed;
  for (std::uint32_t byte = 0; byte < 8u; ++byte) {
    hash ^= (value >> (byte * 8u)) & 0xFFull;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::uint64_t fnv1a64_extend(std::uint64_t seed, double value) noexcept {
  std::uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value), "double must be 64-bit");
  std::memcpy(&bits, &value, sizeof(bits));
  return fnv1a64_extend(seed, bits);
}

std::string format_double(double value) {
  // Locale-independent, round-trippable-enough fixed precision without relying
  // on std::to_chars floating point support (which is still incomplete on some
  // standard libraries).
  char buffer[64] = {};
  const int written = std::snprintf(buffer, sizeof(buffer), "%.6f", value);
  if (written <= 0) {
    return "0.000000";
  }
  return std::string(buffer, static_cast<std::size_t>(written));
}

}  // namespace ensemble_fabric::detail
