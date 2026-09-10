#pragma once

#include <cstdint>
#include <string_view>

namespace ensemble_fabric {

inline constexpr std::uint32_t version_major = 1;
inline constexpr std::uint32_t version_minor = 0;
inline constexpr std::uint32_t version_patch = 0;

/// Semantic version of the runtime, for example "1.0.0".
[[nodiscard]] std::string_view version_string() noexcept;

/// Human readable identification of the build and its guarantees.
[[nodiscard]] std::string_view build_description() noexcept;

/// Format version of the on-disk persistence container understood by this build.
inline constexpr std::uint32_t persistence_format_version = 1;

/// Protocol version of the framed wire protocol understood by this build.
inline constexpr std::uint16_t protocol_version = 1;

/// Identifies the reference/synthetic model backend in inspection output so that
/// synthetic evidence is never confused with real model inference.
inline constexpr std::string_view reference_backend_label = "reference.synthetic";

}  // namespace ensemble_fabric
