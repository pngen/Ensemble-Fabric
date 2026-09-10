#include <algorithm>
#include <utility>

#include "detail/text.hpp"
#include "fabric_internal.hpp"

namespace ensemble_fabric {

void supersede_candidate_evidence(ExecutionRecord& execution, CandidateId candidate) {
  for (EvaluationRecord& evaluation : execution.evaluations) {
    if (evaluation.candidate == candidate && evaluation.current) {
      evaluation.current = false;
    }
  }
  execution.pending.erase(
      std::remove_if(execution.pending.begin(), execution.pending.end(),
                     [candidate](const PendingEvaluation& pending) {
                       return pending.candidate == candidate && !pending.answered;
                     }),
      execution.pending.end());
}

void fail_candidate(EnsembleRecord& ensemble, ExecutionRecord& execution, CandidateRecord& candidate,
                    EnsembleError failure, std::string detail) {
  candidate.state = CandidateState::failed;
  candidate.failure = failure;
  candidate.failure_detail = detail;
  candidate.payload.clear();
  candidate.payload_digest = 0;
  candidate.evaluation_count = 0;
  for (ParticipantRecord& participant : ensemble.participants) {
    if (participant.id == candidate.participant) {
      participant.candidates_failed += 1u;
    }
  }
  supersede_candidate_evidence(execution, candidate.id);
}

bool retry_authorized(const EnsembleSpec& spec, const CandidateRecord& candidate,
                      EnsembleError failure) noexcept {
  if (candidate.fallback && !spec.fallback.enabled) {
    return false;
  }
  if (candidate.attempts_used >= spec.retry.max_attempts) {
    return false;
  }
  switch (failure) {
    case EnsembleError::participant_unavailable:
      return spec.retry.retry_on_unavailable;
    case EnsembleError::transport_error:
      return spec.retry.retry_on_transport_error;
    case EnsembleError::malformed_candidate:
      return spec.retry.retry_on_malformed_candidate;
    case EnsembleError::participant_failed:
      return spec.retry.retry_on_participant_failed;
    default:
      return false;
  }
}

void rewind_candidate_for_retry(EnsembleRecord& ensemble, ExecutionRecord& execution,
                                CandidateRecord& candidate, EnsembleId ensemble_id) {
  (void)ensemble_id;
  supersede_candidate_evidence(execution, candidate.id);
  candidate.generation = CandidateGeneration(candidate.generation.value() + 1u);
  candidate.attempt += 1u;
  candidate.state = CandidateState::declared;
  candidate.payload.clear();
  candidate.payload_digest = 0;
  candidate.failure = EnsembleError::ok;
  candidate.failure_detail.clear();
  candidate.evaluation_count = 0;
  candidate.hard_failures = 0;
  candidate.hard_unknowns = 0;
  candidate.hard_predicates = 0;
  candidate.budget_units_used = 0;
  candidate.received_sequence = Sequence();
  candidate.current = true;
  for (ParticipantRecord& participant : ensemble.participants) {
    if (participant.id == candidate.participant) {
      participant.retries_consumed += 1u;
    }
  }
}

void handle_participant_failure_locked(EnsembleRecord& ensemble, ExecutionRecord& execution,
                                       ParticipantRecord& participant,
                                       ParticipantFailureKind kind, const std::string& detail) {
  const EnsembleError failure = failure_error(kind);
  for (CandidateRecord& candidate : execution.candidates) {
    if (candidate.participant != participant.id) {
      continue;
    }
    if (candidate.participant_generation != participant.generation) {
      // Output from a previous incarnation of this logical participant is
      // already fenced; it is not re-attributed to the replacement.
      continue;
    }
    if (!candidate.in_flight()) {
      continue;
    }
    if (retry_authorized(ensemble.spec, candidate, failure)) {
      candidate.worker = participant.worker;
      candidate.worker_boot = participant.worker_boot;
      rewind_candidate_for_retry(ensemble, execution, candidate, ensemble.spec.id);
    } else {
      fail_candidate(ensemble, execution, candidate, failure,
                     "participant failure (" + std::string(ensemble_fabric::to_string(kind)) +
                         "): " + detail);
    }
  }
}

Outcome<ParticipantGeneration> EnsembleFabric::register_participant(
    const ParticipantRegistration& registration) {
  std::unique_lock lock(impl_->mutex);

  const Status authority = [&]() -> Status {
    if (!registration.ensemble.valid() || !registration.ensemble_generation.valid() ||
        !registration.participant.valid() || !registration.worker.valid() ||
        !registration.worker_boot.valid() || !registration.coordinator_epoch.valid()) {
      return fail(EnsembleError::invalid_identity_combination,
                  "registration is missing part of its authority identity");
    }
    return ok_status();
  }();
  if (!authority.ok()) {
    return fail<ParticipantGeneration>(authority.code(), authority.error().message);
  }

  if (registration.coordinator_epoch != impl_->epoch) {
    impl_->counters.stale_submissions_rejected += 1u;
    return fail<ParticipantGeneration>(EnsembleError::stale_coordinator_epoch,
                "registration was produced under epoch " +
                    registration.coordinator_epoch.to_string() + " but the current epoch is " +
                    impl_->epoch.to_string());
  }

  EnsembleRecord* ensemble = impl_->find(registration.ensemble);
  if (ensemble == nullptr) {
    return fail<ParticipantGeneration>(EnsembleError::unknown_ensemble,
                "unknown ensemble " + registration.ensemble.to_string());
  }
  if (ensemble->spec.generation != registration.ensemble_generation) {
    impl_->counters.stale_submissions_rejected += 1u;
    return fail<ParticipantGeneration>(EnsembleError::stale_ensemble_generation,
                "registration targets ensemble generation " +
                    registration.ensemble_generation.to_string() + " but the current generation is " +
                    ensemble->spec.generation.to_string());
  }
  ParticipantRecord* participant = EnsembleFabric::Impl::find_participant(*ensemble, registration.participant);
  if (participant == nullptr) {
    return fail<ParticipantGeneration>(EnsembleError::unknown_participant,
                "participant " + registration.participant.to_string() +
                    " is not declared by this ensemble");
  }
  if (participant->registered && participant->status == ParticipantStatus::current) {
    impl_->counters.duplicate_submissions_suppressed += 1u;
    return fail<ParticipantGeneration>(EnsembleError::participant_duplicate,
                "participant " + registration.participant.to_string() +
                    " is already registered by worker " + participant->worker.to_string() +
                    " boot " + participant->worker_boot.to_string());
  }
  if (!participant->declaration.roles.contains_all(registration.claimed_roles)) {
    return fail<ParticipantGeneration>(EnsembleError::participant_role_mismatch,
                "worker claims roles that the ensemble specification does not grant: claimed=" +
                    registration.claimed_roles.to_string() + " granted=" +
                    participant->declaration.roles.to_string());
  }
  const Status compatible =
      check_compatibility(participant->declaration.compatibility, registration.profile);
  if (!compatible.ok()) {
    return fail<ParticipantGeneration>(compatible.code(), compatible.error().message);
  }

  const bool replacement = participant->worker_boot.valid();
  if (replacement) {
    participant->generation = ParticipantGeneration(participant->generation.value() + 1u);
    participant->replacement_count += 1u;
  }
  participant->worker = registration.worker;
  participant->worker_boot = registration.worker_boot;
  participant->coordinator_epoch = registration.coordinator_epoch;
  participant->profile = registration.profile;
  participant->status = ParticipantStatus::current;
  participant->registered = true;
  participant->failure_detail.clear();
  return participant->generation;
}

Status EnsembleFabric::report_participant_failure(const ParticipantAuthority& authority,
                                                  ParticipantFailureKind kind,
                                                  std::string detail) {
  std::unique_lock lock(impl_->mutex);
  const Status valid = validate_authority(authority);
  if (!valid.ok()) {
    return valid;
  }
  if (authority.coordinator_epoch != impl_->epoch) {
    impl_->counters.stale_submissions_rejected += 1u;
    return fail(EnsembleError::stale_coordinator_epoch,
                "participant failure was reported under epoch " +
                    authority.coordinator_epoch.to_string() + " but the current epoch is " +
                    impl_->epoch.to_string());
  }
  EnsembleRecord* ensemble = impl_->find(authority.ensemble);
  if (ensemble == nullptr) {
    return fail(EnsembleError::unknown_ensemble, "unknown ensemble");
  }
  if (ensemble->spec.generation != authority.ensemble_generation) {
    impl_->counters.stale_submissions_rejected += 1u;
    return fail(EnsembleError::stale_ensemble_generation,
                "participant failure targets a historical ensemble generation");
  }
  ParticipantRecord* participant = EnsembleFabric::Impl::find_participant(*ensemble, authority.participant);
  if (participant == nullptr) {
    return fail(EnsembleError::unknown_participant, "unknown participant");
  }
  if (participant->generation != authority.participant_generation ||
      participant->worker_boot != authority.worker_boot ||
      participant->worker != authority.worker) {
    impl_->counters.stale_submissions_rejected += 1u;
    return fail(EnsembleError::stale_worker_boot,
                "participant failure names a worker incarnation that is no longer authoritative");
  }

  participant->status = kind == ParticipantFailureKind::unavailable
                            ? ParticipantStatus::unavailable
                            : ParticipantStatus::failed;
  participant->registered = false;
  participant->failure_detail = detail;

  Status result = ok_status();
  if (ensemble->has_execution && ensemble->execution.status == ExecutionStatus::open) {
    handle_participant_failure_locked(*ensemble, ensemble->execution, *participant, kind, detail);
    if (ensemble->spec.cancellation.cancel_on_required_failure && participant->declaration.required) {
      ensemble->execution.status = ExecutionStatus::failed;
      result = fail(EnsembleError::required_participant_failed,
                    "required participant " + participant->id.to_string() +
                        " failed and the cancellation policy cancels the execution");
    }
  }
  return result;
}

Status EnsembleFabric::invalidate_dynamic_authority(std::string reason) {
  std::unique_lock lock(impl_->mutex);
  for (auto& entry : impl_->ensembles) {
    EnsembleRecord& ensemble = entry.second;
    for (ParticipantRecord& participant : ensemble.participants) {
      if (!participant.registered && participant.status == ParticipantStatus::revalidation_required) {
        continue;
      }
      participant.status = ParticipantStatus::revalidation_required;
      participant.registered = false;
      participant.failure_detail = reason;
    }
    if (ensemble.has_execution && ensemble.execution.status == ExecutionStatus::open) {
      ensemble.execution.status = ExecutionStatus::revalidation_required;
      // In-flight work carried authority from the previous coordinator epoch.
      for (CandidateRecord& candidate : ensemble.execution.candidates) {
        if (candidate.in_flight()) {
          candidate.state = CandidateState::superseded;
          candidate.current = false;
          candidate.failure = EnsembleError::revalidation_required;
          candidate.failure_detail = reason;
        }
      }
      for (EvaluationRecord& evaluation : ensemble.execution.evaluations) {
        evaluation.current = false;
      }
      for (PendingEvaluation& pending : ensemble.execution.pending) {
        pending.abandoned = true;
      }
      impl_->counters.revalidations_required += 1u;
    }
  }
  return ok_status();
}

}  // namespace ensemble_fabric
