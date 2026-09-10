#include "ensemble_fabric/participant.hpp"

namespace ensemble_fabric {

std::string_view to_string(ParticipantStatus status) noexcept {
  switch (status) {
    case ParticipantStatus::current: return "CURRENT";
    case ParticipantStatus::unavailable: return "UNAVAILABLE";
    case ParticipantStatus::failed: return "FAILED";
    case ParticipantStatus::fenced: return "FENCED";
    case ParticipantStatus::revalidation_required: return "REVALIDATION_REQUIRED";
    case ParticipantStatus::retired: return "RETIRED";
  }
  return "UNKNOWN_STATUS";
}

std::optional<ParticipantStatus> parse_participant_status(std::string_view text) noexcept {
  if (text == "CURRENT" || text == "current") return ParticipantStatus::current;
  if (text == "UNAVAILABLE" || text == "unavailable") return ParticipantStatus::unavailable;
  if (text == "FAILED" || text == "failed") return ParticipantStatus::failed;
  if (text == "FENCED" || text == "fenced") return ParticipantStatus::fenced;
  if (text == "REVALIDATION_REQUIRED" || text == "revalidation_required") {
    return ParticipantStatus::revalidation_required;
  }
  if (text == "RETIRED" || text == "retired") return ParticipantStatus::retired;
  return std::nullopt;
}

std::string_view to_string(ParticipantFailureKind kind) noexcept {
  switch (kind) {
    case ParticipantFailureKind::unavailable: return "UNAVAILABLE";
    case ParticipantFailureKind::transport_error: return "TRANSPORT_ERROR";
    case ParticipantFailureKind::malformed_output: return "MALFORMED_OUTPUT";
    case ParticipantFailureKind::internal_error: return "INTERNAL_ERROR";
    case ParticipantFailureKind::timeout_reported: return "TIMEOUT_REPORTED";
    case ParticipantFailureKind::explicit_abort: return "EXPLICIT_ABORT";
  }
  return "UNKNOWN_FAILURE";
}

std::optional<ParticipantFailureKind> parse_failure_kind(std::string_view text) noexcept {
  if (text == "UNAVAILABLE" || text == "unavailable") return ParticipantFailureKind::unavailable;
  if (text == "TRANSPORT_ERROR" || text == "transport_error") {
    return ParticipantFailureKind::transport_error;
  }
  if (text == "MALFORMED_OUTPUT" || text == "malformed_output") {
    return ParticipantFailureKind::malformed_output;
  }
  if (text == "INTERNAL_ERROR" || text == "internal_error") {
    return ParticipantFailureKind::internal_error;
  }
  if (text == "TIMEOUT_REPORTED" || text == "timeout_reported") {
    return ParticipantFailureKind::timeout_reported;
  }
  if (text == "EXPLICIT_ABORT" || text == "explicit_abort") {
    return ParticipantFailureKind::explicit_abort;
  }
  return std::nullopt;
}

EnsembleError failure_error(ParticipantFailureKind kind) noexcept {
  switch (kind) {
    case ParticipantFailureKind::unavailable: return EnsembleError::participant_unavailable;
    case ParticipantFailureKind::transport_error: return EnsembleError::transport_error;
    case ParticipantFailureKind::malformed_output: return EnsembleError::malformed_candidate;
    case ParticipantFailureKind::internal_error: return EnsembleError::participant_failed;
    case ParticipantFailureKind::timeout_reported: return EnsembleError::participant_unavailable;
    case ParticipantFailureKind::explicit_abort: return EnsembleError::participant_failed;
  }
  return EnsembleError::internal_error;
}

Status check_compatibility(const CompatibilityRequirement& requirement,
                           const CompatibilityProfile& profile) {
  if (profile.protocol_version < requirement.min_protocol_version) {
    return fail(EnsembleError::participant_incompatible,
                "participant protocol version " + std::to_string(profile.protocol_version) +
                    " is below the required version " +
                    std::to_string(requirement.min_protocol_version));
  }
  if (requirement.required_capabilities != 0u &&
      (profile.capability_mask & requirement.required_capabilities) !=
          requirement.required_capabilities) {
    return fail(EnsembleError::participant_incompatible,
                "participant is missing required capability bits");
  }
  if (requirement.constrains_task_class() && profile.task_class != requirement.required_task_class) {
    return fail(EnsembleError::participant_incompatible,
                "participant task class " + std::to_string(profile.task_class) +
                    " does not match the required task class " +
                    std::to_string(requirement.required_task_class));
  }
  if (requirement.required_output_schema != 0u &&
      profile.output_schema != requirement.required_output_schema) {
    return fail(EnsembleError::participant_incompatible,
                "participant output schema does not match the required schema");
  }
  if (requirement.excluded_model_family != 0u &&
      profile.model_family == requirement.excluded_model_family) {
    return fail(EnsembleError::participant_incompatible,
                "participant model family is excluded by the ensemble requirement");
  }
  if (requirement.required_backend_features != 0u &&
      (profile.backend_features & requirement.required_backend_features) !=
          requirement.required_backend_features) {
    return fail(EnsembleError::participant_incompatible,
                "participant is missing required backend features");
  }
  return ok_status();
}

}  // namespace ensemble_fabric
