#include "ensemble_fabric/limits.hpp"

#include <limits>

namespace ensemble_fabric {

bool checked_add(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t limit,
                 std::uint64_t& out) noexcept {
  if (lhs > limit || rhs > limit) {
    return false;
  }
  const std::uint64_t sum = lhs + rhs;
  if (sum < lhs || sum > limit) {
    return false;
  }
  out = sum;
  return true;
}

bool checked_mul(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t limit,
                 std::uint64_t& out) noexcept {
  if (lhs == 0ull || rhs == 0ull) {
    out = 0ull;
    return true;
  }
  if (lhs > limit || rhs > limit) {
    return false;
  }
  const std::uint64_t product = lhs * rhs;
  if (product / lhs != rhs || product > limit) {
    return false;
  }
  out = product;
  return true;
}

Status Limits::validate() const {
  if (max_participants_per_ensemble == 0u || max_stages_per_ensemble == 0u ||
      max_parallel_branches == 0u || max_role_quota_entries == 0u) {
    return fail(EnsembleError::invalid_argument,
                "limits: participant, stage, branch, and role-quota bounds must be positive");
  }
  if (max_in_flight_executions == 0u) {
    return fail(EnsembleError::invalid_argument, "limits: max_in_flight_executions must be positive");
  }
  if (max_candidates_per_execution == 0u || max_evaluations_per_candidate == 0u ||
      max_evaluations_per_execution == 0u) {
    return fail(EnsembleError::invalid_argument,
                "limits: candidate and evaluation bounds must be positive");
  }
  if (max_candidate_payload_bytes == 0u || max_evaluation_payload_bytes == 0u) {
    return fail(EnsembleError::invalid_argument, "limits: payload bounds must be positive");
  }
  if (max_frame_bytes < 128u) {
    return fail(EnsembleError::invalid_argument, "limits: max_frame_bytes must hold a header");
  }
  if (max_candidate_payload_bytes > max_frame_bytes) {
    return fail(EnsembleError::invalid_argument,
                "limits: candidate payloads must fit inside the maximum frame size");
  }
  if (max_metadata_bytes == 0u || max_label_bytes == 0u) {
    return fail(EnsembleError::invalid_argument, "limits: metadata bounds must be positive");
  }
  if (max_records_per_snapshot == 0u || max_retained_results == 0u) {
    return fail(EnsembleError::invalid_argument, "limits: persistence bounds must be positive");
  }
  if (max_persistence_bytes < 256u) {
    return fail(EnsembleError::invalid_argument, "limits: max_persistence_bytes is too small");
  }
  if (max_evaluations_per_candidate >
      std::numeric_limits<std::uint32_t>::max() / 2u) {
    return fail(EnsembleError::invalid_argument, "limits: evaluation bound is unreasonable");
  }
  return ok_status();
}

}  // namespace ensemble_fabric
