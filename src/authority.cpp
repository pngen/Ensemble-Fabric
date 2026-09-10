#include "ensemble_fabric/authority.hpp"

#include "detail/text.hpp"

namespace ensemble_fabric {
namespace {

[[nodiscard]] Status require_complete(const ParticipantAuthority& authority) {
  if (!authority.ensemble.valid()) {
    return fail(EnsembleError::invalid_identity_combination, "authority: ensemble identity is absent");
  }
  if (!authority.ensemble_generation.valid()) {
    return fail(EnsembleError::invalid_identity_combination,
                "authority: ensemble generation is absent");
  }
  if (!authority.participant.valid()) {
    return fail(EnsembleError::invalid_identity_combination,
                "authority: participant identity is absent");
  }
  if (!authority.participant_generation.valid()) {
    return fail(EnsembleError::invalid_identity_combination,
                "authority: participant generation is absent");
  }
  if (!authority.worker.valid()) {
    return fail(EnsembleError::invalid_identity_combination, "authority: worker identity is absent");
  }
  if (!authority.worker_boot.valid()) {
    return fail(EnsembleError::invalid_identity_combination,
                "authority: worker boot identity is absent");
  }
  if (!authority.coordinator_epoch.valid()) {
    return fail(EnsembleError::invalid_identity_combination,
                "authority: coordinator epoch is absent");
  }
  return ok_status();
}

}  // namespace

Status validate_authority(const ParticipantAuthority& authority) {
  return require_complete(authority);
}

Status validate_authority(const CandidateAuthority& authority) {
  const Status base = require_complete(authority.participant);
  if (!base.ok()) {
    return base;
  }
  if (!authority.execution.valid()) {
    return fail(EnsembleError::invalid_identity_combination, "authority: execution identity is absent");
  }
  if (!authority.attempt_generation.valid()) {
    return fail(EnsembleError::invalid_identity_combination, "authority: attempt generation is absent");
  }
  if (!authority.candidate.valid()) {
    return fail(EnsembleError::invalid_identity_combination, "authority: candidate identity is absent");
  }
  if (!authority.candidate_generation.valid()) {
    return fail(EnsembleError::invalid_identity_combination,
                "authority: candidate generation is absent");
  }
  return ok_status();
}

Status validate_authority(const EvaluationAuthority& authority) {
  const Status base = require_complete(authority.evaluator);
  if (!base.ok()) {
    return base;
  }
  if (!authority.execution.valid()) {
    return fail(EnsembleError::invalid_identity_combination, "authority: execution identity is absent");
  }
  if (!authority.attempt_generation.valid()) {
    return fail(EnsembleError::invalid_identity_combination, "authority: attempt generation is absent");
  }
  if (!authority.evaluation.valid()) {
    return fail(EnsembleError::invalid_identity_combination,
                "authority: evaluation identity is absent");
  }
  if (!authority.evaluation_generation.valid()) {
    return fail(EnsembleError::invalid_identity_combination,
                "authority: evaluation generation is absent");
  }
  if (!authority.candidate.valid()) {
    return fail(EnsembleError::invalid_identity_combination, "authority: candidate identity is absent");
  }
  if (!authority.candidate_generation.valid()) {
    return fail(EnsembleError::invalid_identity_combination,
                "authority: candidate generation is absent");
  }
  return ok_status();
}

bool same_actor(const ParticipantAuthority& lhs, const ParticipantAuthority& rhs) noexcept {
  return lhs.ensemble == rhs.ensemble && lhs.participant == rhs.participant &&
         lhs.participant_generation == rhs.participant_generation && lhs.worker == rhs.worker &&
         lhs.worker_boot == rhs.worker_boot &&
         lhs.coordinator_epoch == rhs.coordinator_epoch;
}

std::string ParticipantAuthority::describe() const {
  std::string out;
  out.append("ensemble=").append(ensemble.to_string());
  out.append(" gen=").append(ensemble_generation.to_string());
  out.append(" participant=").append(participant.to_string());
  out.append(" pgen=").append(participant_generation.to_string());
  out.append(" worker=").append(worker.to_string());
  out.append(" boot=").append(worker_boot.to_string());
  out.append(" epoch=").append(coordinator_epoch.to_string());
  return out;
}

std::string CandidateAuthority::describe() const {
  std::string out = participant.describe();
  out.append(" execution=").append(execution.to_string());
  out.append(" attempt=").append(attempt_generation.to_string());
  out.append(" candidate=").append(candidate.to_string());
  out.append(" cgen=").append(candidate_generation.to_string());
  return out;
}

std::string EvaluationAuthority::describe() const {
  std::string out = evaluator.describe();
  out.append(" execution=").append(execution.to_string());
  out.append(" attempt=").append(attempt_generation.to_string());
  out.append(" evaluation=").append(evaluation.to_string());
  out.append(" egen=").append(evaluation_generation.to_string());
  out.append(" candidate=").append(candidate.to_string());
  out.append(" cgen=").append(candidate_generation.to_string());
  return out;
}

}  // namespace ensemble_fabric
