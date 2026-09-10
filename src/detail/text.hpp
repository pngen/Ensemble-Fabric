#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ensemble_fabric::detail {

/// True when every byte is a printable ASCII character or a tab/space.  All
/// labels, rationales, and diagnostics that cross a trust boundary are checked
/// with this before they are stored or rendered.
[[nodiscard]] bool is_bounded_ascii(std::string_view text, std::size_t max_bytes) noexcept;

/// Replaces non-printable bytes so that untrusted text can never corrupt a
/// terminal, a persisted record, or a log line.
[[nodiscard]] std::string sanitize_text(std::string_view text, std::size_t max_bytes);

/// Strict unsigned parse: no sign, no whitespace, no trailing characters, and a
/// range check that rejects overflow instead of wrapping.
[[nodiscard]] bool parse_u32(std::string_view text, std::uint32_t& out) noexcept;
[[nodiscard]] bool parse_u64(std::string_view text, std::uint64_t& out) noexcept;

/// Strict double parse: rejects NaN, infinities, trailing characters, and
/// non-finite results.
[[nodiscard]] bool parse_finite_double(std::string_view text, double& out) noexcept;

[[nodiscard]] std::string join(const std::vector<std::string>& parts, std::string_view separator);

/// Deterministic 64-bit FNV-1a hash used for fingerprints that must be stable
/// across compilers and runs (never std::hash, which is not stable).
[[nodiscard]] std::uint64_t fnv1a64(std::string_view text) noexcept;
[[nodiscard]] std::uint64_t fnv1a64_extend(std::uint64_t seed, std::string_view text) noexcept;
[[nodiscard]] std::uint64_t fnv1a64_extend(std::uint64_t seed, std::uint64_t value) noexcept;
[[nodiscard]] std::uint64_t fnv1a64_extend(std::uint64_t seed, double value) noexcept;

/// Formats a double with a fixed, locale-independent representation so that
/// persisted and rendered content is byte-stable.
[[nodiscard]] std::string format_double(double value);

}  // namespace ensemble_fabric::detail
