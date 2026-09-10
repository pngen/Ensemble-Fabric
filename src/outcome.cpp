#include "ensemble_fabric/outcome.hpp"

namespace ensemble_fabric {

std::string_view to_string(EnsembleError error) noexcept {
  switch (error) {
    case EnsembleError::ok: return "ok";
    case EnsembleError::invalid_argument: return "invalid_argument";
    case EnsembleError::resource_limit_exceeded: return "resource_limit_exceeded";
    case EnsembleError::unsupported_version: return "unsupported_version";
    case EnsembleError::not_found: return "not_found";
    case EnsembleError::unknown_ensemble: return "unknown_ensemble";
    case EnsembleError::unknown_execution: return "unknown_execution";
    case EnsembleError::unknown_participant: return "unknown_participant";
    case EnsembleError::unknown_candidate: return "unknown_candidate";
    case EnsembleError::unknown_evaluation: return "unknown_evaluation";
    case EnsembleError::stale_ensemble_generation: return "stale_ensemble_generation";
    case EnsembleError::stale_execution_generation: return "stale_execution_generation";
    case EnsembleError::stale_participant_generation: return "stale_participant_generation";
    case EnsembleError::stale_worker_boot: return "stale_worker_boot";
    case EnsembleError::stale_coordinator_epoch: return "stale_coordinator_epoch";
    case EnsembleError::stale_candidate_generation: return "stale_candidate_generation";
    case EnsembleError::stale_evaluation_generation: return "stale_evaluation_generation";
    case EnsembleError::participant_not_authoritative: return "participant_not_authoritative";
    case EnsembleError::participant_unavailable: return "participant_unavailable";
    case EnsembleError::participant_incompatible: return "participant_incompatible";
    case EnsembleError::participant_role_mismatch: return "participant_role_mismatch";
    case EnsembleError::participant_duplicate: return "participant_duplicate";
    case EnsembleError::participant_revalidation_required: return "participant_revalidation_required";
    case EnsembleError::participant_failed: return "participant_failed";
    case EnsembleError::required_participant_failed: return "required_participant_failed";
    case EnsembleError::malformed_candidate: return "malformed_candidate";
    case EnsembleError::malformed_evaluation: return "malformed_evaluation";
    case EnsembleError::malformed_frame: return "malformed_frame";
    case EnsembleError::malformed_persistence: return "malformed_persistence";
    case EnsembleError::checksum_mismatch: return "checksum_mismatch";
    case EnsembleError::trailing_data: return "trailing_data";
    case EnsembleError::protocol_error: return "protocol_error";
    case EnsembleError::invalid_enum_value: return "invalid_enum_value";
    case EnsembleError::invalid_identity_combination: return "invalid_identity_combination";
    case EnsembleError::payload_too_large: return "payload_too_large";
    case EnsembleError::invalid_state_transition: return "invalid_state_transition";
    case EnsembleError::duplicate_completion: return "duplicate_completion";
    case EnsembleError::duplicate_evaluation: return "duplicate_evaluation";
    case EnsembleError::conflicting_evaluation: return "conflicting_evaluation";
    case EnsembleError::duplicate_dispatch: return "duplicate_dispatch";
    case EnsembleError::execution_not_open: return "execution_not_open";
    case EnsembleError::evaluation_target_mismatch: return "evaluation_target_mismatch";
    case EnsembleError::quorum_impossible: return "quorum_impossible";
    case EnsembleError::quorum_not_reached: return "quorum_not_reached";
    case EnsembleError::insufficient_evidence: return "insufficient_evidence";
    case EnsembleError::all_candidates_invalid: return "all_candidates_invalid";
    case EnsembleError::consensus_not_reached: return "consensus_not_reached";
    case EnsembleError::disagreement_unresolved: return "disagreement_unresolved";
    case EnsembleError::tie_unresolved: return "tie_unresolved";
    case EnsembleError::fallback_not_authorized: return "fallback_not_authorized";
    case EnsembleError::fallback_exhausted: return "fallback_exhausted";
    case EnsembleError::result_already_committed: return "result_already_committed";
    case EnsembleError::conflicting_result_commit: return "conflicting_result_commit";
    case EnsembleError::no_eligible_candidate: return "no_eligible_candidate";
    case EnsembleError::ensemble_cancelled: return "ensemble_cancelled";
    case EnsembleError::ensemble_superseded: return "ensemble_superseded";
    case EnsembleError::execution_cancelled: return "execution_cancelled";
    case EnsembleError::revalidation_required: return "revalidation_required";
    case EnsembleError::persistence_io_error: return "persistence_io_error";
    case EnsembleError::persistence_corruption: return "persistence_corruption";
    case EnsembleError::transport_error: return "transport_error";
    case EnsembleError::internal_error: return "internal_error";
  }
  return "unrecognized_error";
}

bool is_authority_failure(EnsembleError error) noexcept {
  switch (error) {
    case EnsembleError::stale_ensemble_generation:
    case EnsembleError::stale_execution_generation:
    case EnsembleError::stale_participant_generation:
    case EnsembleError::stale_worker_boot:
    case EnsembleError::stale_coordinator_epoch:
    case EnsembleError::stale_candidate_generation:
    case EnsembleError::stale_evaluation_generation:
    case EnsembleError::participant_not_authoritative:
    case EnsembleError::participant_revalidation_required:
    case EnsembleError::ensemble_superseded:
    case EnsembleError::execution_cancelled:
    case EnsembleError::revalidation_required:
      return true;
    default:
      return false;
  }
}

bool is_retryable_failure(EnsembleError error) noexcept {
  switch (error) {
    case EnsembleError::participant_unavailable:
    case EnsembleError::participant_failed:
    case EnsembleError::transport_error:
    case EnsembleError::malformed_candidate:
      return true;
    default:
      return false;
  }
}

bool is_terminal_failure(EnsembleError error) noexcept {
  switch (error) {
    case EnsembleError::result_already_committed:
    case EnsembleError::ensemble_cancelled:
    case EnsembleError::ensemble_superseded:
    case EnsembleError::all_candidates_invalid:
    case EnsembleError::quorum_impossible:
    case EnsembleError::persistence_corruption:
    case EnsembleError::persistence_io_error:
    case EnsembleError::invalid_argument:
      return true;
    default:
      return false;
  }
}

}  // namespace ensemble_fabric
