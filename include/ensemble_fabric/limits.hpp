#pragma once

#include <cstddef>
#include <cstdint>

#include "ensemble_fabric/outcome.hpp"

namespace ensemble_fabric {

/// Every unbounded or untrusted quantity in the runtime has an explicit limit.
/// Limits are data, not compile-time magic, so that a deployment can prove what
/// it accepts and tests can exercise the exact boundary.
struct Limits {
  // Ensemble definition.
  std::uint32_t max_participants_per_ensemble = 64;
  std::uint32_t max_stages_per_ensemble = 16;
  std::uint32_t max_parallel_branches = 64;
  std::uint32_t max_role_quota_entries = 16;

  // Execution.
  std::uint32_t max_queued_executions = 1024;
  std::uint32_t max_in_flight_executions = 64;
  std::uint32_t max_candidates_per_execution = 256;
  std::uint32_t max_evaluations_per_candidate = 32;
  std::uint32_t max_evaluations_per_execution = 4096;
  std::uint32_t max_retries = 8;
  std::uint32_t max_fallback_depth = 4;

  // Payloads.
  std::uint32_t max_candidate_payload_bytes = 1u << 20;
  std::uint32_t max_evaluation_payload_bytes = 64u << 10;
  std::uint32_t max_metadata_bytes = 4096;

  // Wire protocol.
  std::uint32_t max_frame_bytes = 4u << 20;
  std::uint32_t max_connections = 256;

  // Persistence.
  std::uint32_t max_persistence_bytes = 32u << 20;
  std::uint32_t max_records_per_snapshot = 4096;
  std::uint32_t max_retained_results = 64;
  std::uint32_t max_retained_commits = 256;

  // Inspection.
  std::uint32_t max_explanation_entries = 256;
  std::uint32_t max_label_bytes = 128;

  [[nodiscard]] static Limits defaults() noexcept { return Limits{}; }

  /// Rejects nonsensical configurations early instead of failing during
  /// untrusted decode.
  [[nodiscard]] Status validate() const;
};

/// Checks whether a multiplication and an addition stay inside a bound.  Used
/// before every allocation driven by untrusted input.
[[nodiscard]] bool checked_add(std::uint64_t lhs, std::uint64_t rhs,
                               std::uint64_t limit, std::uint64_t& out) noexcept;
[[nodiscard]] bool checked_mul(std::uint64_t lhs, std::uint64_t rhs,
                               std::uint64_t limit, std::uint64_t& out) noexcept;

}  // namespace ensemble_fabric
