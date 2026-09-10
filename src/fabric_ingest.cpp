#include <algorithm>
#include <utility>

#include "detail/text.hpp"
#include "fabric_internal.hpp"

namespace ensemble_fabric {
namespace {

/// Typed rejection for an output that arrives for a candidate which can no
/// longer accept content.  Every distinct lifecycle cause keeps its own code.
[[nodiscard]] Status rejected_output_state(CandidateState state, CandidateId id) {
  switch (state) {
    case CandidateState::cancelled:
      return fail(EnsembleError::execution_cancelled,
                  "candidate " + id.to_string() + " was cancelled; its output is fenced");
    case CandidateState::superseded:
      return fail(EnsembleError::ensemble_superseded,
                  "candidate " + id.to_string() + " was superseded; its output is fenced");
    case CandidateState::failed:
      return fail(EnsembleError::invalid_state_transition,
                  "candidate " + id.to_string() + " already failed");
    case CandidateState::abstained:
      return fail(EnsembleError::invalid_state_transition,
                  "candidate " + id.to_string() + " already abstained");
    case CandidateState::invalid:
      return fail(EnsembleError::duplicate_completion,
                  "candidate " + id.to_string() + " was already classified invalid");
    case CandidateState::valid:
    case CandidateState::eligible:
    case CandidateState::selected:
    case CandidateState::rejected:
      return fail(EnsembleError::duplicate_completion,
                  "candidate " + id.to_string() + " already produced content");
    case CandidateState::retired:
      return fail(EnsembleError::invalid_state_transition,
                  "candidate " + id.to_string() + " is retired");
    case CandidateState::output_received:
      return fail(EnsembleError::invalid_state_transition,
                  "candidate " + id.to_string() + " is already being validated");
    case CandidateState::declared:
      return fail(EnsembleError::invalid_state_transition,
                  "candidate " + id.to_string() + " has not been dispatched yet");
    case CandidateState::dispatched:
      break;
  }
  return ok_status();
}

struct Located {
  EnsembleRecord* ensemble{nullptr};
  ExecutionRecord* execution{nullptr};
  CandidateRecord* candidate{nullptr};
  ParticipantRecord* participant{nullptr};
};

/// Resolves and validates a candidate-scoped authority.  Returns the located
/// records only when every authority component matches current state.
[[nodiscard]] Outcome<Located> locate_candidate(EnsembleFabric::Impl& impl,
                                                const CandidateAuthority& authority) {
  const Status structural = validate_authority(authority);
  if (!structural.ok()) {
    return fail<Located>(structural.code(), structural.error().message);
  }
  if (authority.participant.coordinator_epoch != impl.epoch) {
    impl.counters.stale_submissions_rejected += 1u;
    return fail<Located>(EnsembleError::stale_coordinator_epoch,
                         "submission was produced under epoch " +
                             authority.participant.coordinator_epoch.to_string() +
                             " but the current epoch is " + impl.epoch.to_string());
  }
  EnsembleRecord* ensemble = impl.find(authority.participant.ensemble);
  if (ensemble == nullptr) {
    return fail<Located>(EnsembleError::unknown_ensemble, "unknown ensemble");
  }
  if (ensemble->spec.generation != authority.participant.ensemble_generation) {
    impl.counters.stale_submissions_rejected += 1u;
    return fail<Located>(EnsembleError::stale_ensemble_generation,
                         "submission targets ensemble generation " +
                             authority.participant.ensemble_generation.to_string() +
                             " but the current generation is " +
                             ensemble->spec.generation.to_string());
  }
  if (!ensemble->has_execution || ensemble->execution.id != authority.execution) {
    impl.counters.stale_submissions_rejected += 1u;
    return fail<Located>(EnsembleError::stale_execution_generation,
                         "submission targets a historical execution generation");
  }
  if (ensemble->execution.attempt_generation != authority.attempt_generation) {
    impl.counters.stale_submissions_rejected += 1u;
    return fail<Located>(EnsembleError::stale_execution_generation,
                         "submission targets a historical attempt generation");
  }
  ParticipantRecord* participant =
      EnsembleFabric::Impl::find_participant(*ensemble, authority.participant.participant);
  if (participant == nullptr) {
    return fail<Located>(EnsembleError::unknown_participant, "unknown participant");
  }
  if (participant->generation != authority.participant.participant_generation) {
    impl.counters.stale_submissions_rejected += 1u;
    return fail<Located>(EnsembleError::stale_participant_generation,
                         "submission names participant generation " +
                             authority.participant.participant_generation.to_string() +
                             " but the current generation is " +
                             participant->generation.to_string());
  }
  if (participant->worker != authority.participant.worker ||
      participant->worker_boot != authority.participant.worker_boot) {
    impl.counters.stale_submissions_rejected += 1u;
    return fail<Located>(EnsembleError::stale_worker_boot,
                         "submission names a worker incarnation that is no longer authoritative");
  }
  if (!participant->authoritative(impl.epoch)) {
    return fail<Located>(EnsembleError::participant_not_authoritative,
                         "participant " + participant->id.to_string() +
                             " is not authoritative in the current epoch");
  }
  CandidateRecord* candidate = find_candidate(ensemble->execution, authority.candidate);
  if (candidate == nullptr) {
    return fail<Located>(EnsembleError::unknown_candidate, "unknown candidate");
  }
  if (candidate->generation != authority.candidate_generation) {
    impl.counters.stale_submissions_rejected += 1u;
    return fail<Located>(EnsembleError::stale_candidate_generation,
                         "submission names candidate generation " +
                             authority.candidate_generation.to_string() +
                             " but the current generation is " + candidate->generation.to_string());
  }
  if (candidate->participant != participant->id) {
    return fail<Located>(EnsembleError::participant_role_mismatch,
                         "submission impersonates another participant's candidate");
  }
  Located located;
  located.ensemble = ensemble;
  located.execution = &ensemble->execution;
  located.candidate = candidate;
  located.participant = participant;
  return located;
}

}  // namespace

Status EnsembleFabric::submit_candidate(const CandidateSubmission& submission) {
  std::unique_lock lock(impl_->mutex);
  Outcome<Located> located = locate_candidate(*impl_, submission.authority);
  if (!located.ok()) {
    return fail(located.code(), located.error().message);
  }
  EnsembleRecord& ensemble = *located.value().ensemble;
  ExecutionRecord& execution = *located.value().execution;
  CandidateRecord& candidate = *located.value().candidate;
  ParticipantRecord& participant = *located.value().participant;

  if (candidate.state != CandidateState::dispatched) {
    if (!permits_output(candidate.state)) {
      impl_->counters.duplicate_submissions_suppressed += 1u;
    }
    return rejected_output_state(candidate.state, candidate.id);
  }

  const Limits& limits = impl_->config.limits;
  if (submission.payload.size() > limits.max_candidate_payload_bytes) {
    return fail(EnsembleError::resource_limit_exceeded,
                "candidate payload exceeds the configured bound of " +
                    std::to_string(limits.max_candidate_payload_bytes) + " bytes");
  }
  if (submission.payload.size() > limits.max_frame_bytes) {
    return fail(EnsembleError::payload_too_large,
                "candidate payload cannot be transported in a single frame");
  }

  const std::uint64_t digest = fingerprint_payload(submission.payload);
  candidate.state = CandidateState::output_received;
  candidate.payload = submission.payload;
  candidate.payload_digest = digest;
  candidate.received_sequence = impl_->next_sequence();
  candidate.latency = candidate.dispatch_sequence.valid() && candidate.received_sequence.valid() &&
                              candidate.received_sequence.value() >=
                                  candidate.dispatch_sequence.value()
                          ? static_cast<std::uint32_t>(candidate.received_sequence.value() -
                                                       candidate.dispatch_sequence.value())
                          : 0u;
  candidate.budget_units_used = participant.declaration.execution_budget_units;

  const bool too_small = submission.payload.size() < ensemble.spec.evidence.min_candidate_payload_bytes;
  if (submission.payload.empty() || too_small) {
    // The submission was authoritative; the *content* is not a valid candidate.
    candidate.state = CandidateState::invalid;
    candidate.failure = EnsembleError::malformed_candidate;
    candidate.failure_detail =
        submission.payload.empty()
            ? "empty candidate payload"
            : "candidate payload is smaller than the ensemble evidence requirement";
    candidate.payload.clear();
    candidate.payload_digest = 0;
    participant.candidates_failed += 1u;
    return ok_status();
  }

  candidate.state = CandidateState::valid;
  candidate.failure = EnsembleError::ok;
  candidate.failure_detail.clear();
  participant.candidates_completed += 1u;
  (void)execution;
  return ok_status();
}

Status EnsembleFabric::submit_candidate_failure(const CandidateFailureSubmission& submission) {
  std::unique_lock lock(impl_->mutex);
  Outcome<Located> located = locate_candidate(*impl_, submission.authority);
  if (!located.ok()) {
    return fail(located.code(), located.error().message);
  }
  EnsembleRecord& ensemble = *located.value().ensemble;
  ExecutionRecord& execution = *located.value().execution;
  CandidateRecord& candidate = *located.value().candidate;

  if (candidate.state != CandidateState::dispatched) {
    if (!permits_output(candidate.state)) {
      impl_->counters.duplicate_submissions_suppressed += 1u;
    }
    return rejected_output_state(candidate.state, candidate.id);
  }
  if (submission.participant_failure < 1u || submission.participant_failure > 6u) {
    return fail(EnsembleError::invalid_enum_value, "unknown participant failure kind");
  }
  const ParticipantFailureKind kind =
      static_cast<ParticipantFailureKind>(submission.participant_failure);
  const EnsembleError failure = failure_error(kind);
  const std::string detail = "participant reported " + std::string(to_string(kind));

  if (retry_authorized(ensemble.spec, candidate, failure)) {
    rewind_candidate_for_retry(ensemble, execution, candidate, ensemble.spec.id);
    return ok_status();
  }
  fail_candidate(ensemble, execution, candidate, failure, detail);
  return ok_status();
}

Status EnsembleFabric::submit_candidate_abstention(
    const CandidateAbstentionSubmission& submission) {
  std::unique_lock lock(impl_->mutex);
  Outcome<Located> located = locate_candidate(*impl_, submission.authority);
  if (!located.ok()) {
    return fail(located.code(), located.error().message);
  }
  CandidateRecord& candidate = *located.value().candidate;
  ParticipantRecord& participant = *located.value().participant;
  if (candidate.state != CandidateState::dispatched) {
    if (!permits_output(candidate.state)) {
      impl_->counters.duplicate_submissions_suppressed += 1u;
    }
    return rejected_output_state(candidate.state, candidate.id);
  }
  candidate.state = CandidateState::abstained;
  candidate.failure_detail =
      detail::sanitize_text(submission.rationale, impl_->config.limits.max_metadata_bytes);
  candidate.payload.clear();
  candidate.payload_digest = 0;
  candidate.received_sequence = impl_->next_sequence();
  participant.candidates_abstained += 1u;
  return ok_status();
}

Outcome<EvaluationId> EnsembleFabric::submit_evaluation(const EvaluationSubmission& submission) {
  std::unique_lock lock(impl_->mutex);
  const EvaluationAuthority& authority = submission.authority;
  const Status structural = validate_authority(authority);
  if (!structural.ok()) {
    return fail<EvaluationId>(structural.code(), structural.error().message);
  }
  if (authority.evaluator.coordinator_epoch != impl_->epoch) {
    impl_->counters.stale_submissions_rejected += 1u;
    return fail<EvaluationId>(EnsembleError::stale_coordinator_epoch,
                              "evaluation was produced under epoch " +
                                  authority.evaluator.coordinator_epoch.to_string() +
                                  " but the current epoch is " + impl_->epoch.to_string());
  }
  EnsembleRecord* ensemble = impl_->find(authority.evaluator.ensemble);
  if (ensemble == nullptr) {
    return fail<EvaluationId>(EnsembleError::unknown_ensemble, "unknown ensemble");
  }
  if (ensemble->spec.generation != authority.evaluator.ensemble_generation) {
    impl_->counters.stale_submissions_rejected += 1u;
    return fail<EvaluationId>(EnsembleError::stale_ensemble_generation,
                              "evaluation targets a historical ensemble generation");
  }
  if (!ensemble->has_execution || ensemble->execution.id != authority.execution) {
    impl_->counters.stale_submissions_rejected += 1u;
    return fail<EvaluationId>(EnsembleError::stale_execution_generation,
                              "evaluation targets a historical execution generation");
  }
  ExecutionRecord& execution = ensemble->execution;
  if (execution.attempt_generation != authority.attempt_generation) {
    impl_->counters.stale_submissions_rejected += 1u;
    return fail<EvaluationId>(EnsembleError::stale_execution_generation,
                              "evaluation targets a historical attempt generation");
  }
  ParticipantRecord* evaluator =
      EnsembleFabric::Impl::find_participant(*ensemble, authority.evaluator.participant);
  if (evaluator == nullptr) {
    return fail<EvaluationId>(EnsembleError::unknown_participant,
                              "evaluator is not declared by this ensemble");
  }
  if (evaluator->generation != authority.evaluator.participant_generation) {
    impl_->counters.stale_submissions_rejected += 1u;
    return fail<EvaluationId>(EnsembleError::stale_participant_generation,
                              "evaluation names a historical evaluator generation");
  }
  if (evaluator->worker != authority.evaluator.worker ||
      evaluator->worker_boot != authority.evaluator.worker_boot) {
    impl_->counters.stale_submissions_rejected += 1u;
    return fail<EvaluationId>(EnsembleError::stale_worker_boot,
                              "evaluation names a worker incarnation that is no longer "
                              "authoritative");
  }
  if (!evaluator->authoritative(impl_->epoch)) {
    return fail<EvaluationId>(EnsembleError::participant_not_authoritative,
                              "evaluator is not authoritative in the current epoch");
  }
  if (!role_evaluates(evaluator->declaration.roles.contains(ParticipantRole::verifier)
                          ? ParticipantRole::verifier
                          : ParticipantRole::judge) ||
      (!evaluator->declaration.roles.contains(ParticipantRole::judge) &&
       !evaluator->declaration.roles.contains(ParticipantRole::verifier))) {
    return fail<EvaluationId>(EnsembleError::participant_role_mismatch,
                              "participant holds no evaluating role");
  }
  CandidateRecord* candidate = find_candidate(execution, authority.candidate);
  if (candidate == nullptr) {
    return fail<EvaluationId>(EnsembleError::unknown_candidate, "unknown candidate");
  }
  if (candidate->generation != authority.candidate_generation) {
    impl_->counters.stale_submissions_rejected += 1u;
    return fail<EvaluationId>(EnsembleError::stale_candidate_generation,
                              "evaluation targets a superseded candidate generation");
  }
  if (!is_content_state(candidate->state)) {
    return fail<EvaluationId>(EnsembleError::invalid_state_transition,
                              "candidate holds no content to evaluate");
  }
  PendingEvaluation* pending = find_pending(execution, authority.evaluation);
  if (pending == nullptr) {
    return fail<EvaluationId>(EnsembleError::invalid_identity_combination,
                              "no evaluation was planned with this identity");
  }
  if (pending->generation != authority.evaluation_generation) {
    impl_->counters.stale_submissions_rejected += 1u;
    return fail<EvaluationId>(EnsembleError::stale_evaluation_generation,
                              "evaluation names a historical evaluation generation");
  }
  if (pending->answered) {
    impl_->counters.duplicate_votes_suppressed += 1u;
    return fail<EvaluationId>(EnsembleError::duplicate_evaluation,
                              "this evaluation identity was already answered");
  }
  if (pending->abandoned) {
    return fail<EvaluationId>(EnsembleError::revalidation_required,
                              "this evaluation plan was abandoned");
  }
  if (pending->evaluator != authority.evaluator.participant ||
      pending->candidate != authority.candidate ||
      pending->candidate_generation != authority.candidate_generation ||
      !(pending->criterion == submission.criterion) || pending->kind != submission.kind) {
    return fail<EvaluationId>(EnsembleError::evaluation_target_mismatch,
                              "the submission does not match the planned evaluation target");
  }
  if (evaluator->declaration.roles.contains(ParticipantRole::verifier) &&
      submission.kind != CriterionKind::hard_predicate) {
    return fail<EvaluationId>(EnsembleError::malformed_evaluation,
                              "a verifier may only submit mandatory hard-predicate evidence");
  }
  if (!evaluator->declaration.roles.contains(ParticipantRole::verifier) &&
      submission.kind != CriterionKind::ranking_signal) {
    return fail<EvaluationId>(EnsembleError::malformed_evaluation,
                              "a judge may only submit ranking-signal evidence");
  }
  if (submission.evidence.size() > impl_->config.limits.max_evaluation_payload_bytes) {
    return fail<EvaluationId>(EnsembleError::resource_limit_exceeded,
                              "evaluation evidence exceeds the configured bound");
  }
  if (submission.rationale.size() > impl_->config.limits.max_metadata_bytes) {
    return fail<EvaluationId>(EnsembleError::resource_limit_exceeded,
                              "evaluation rationale exceeds the configured bound");
  }
  if (ensemble->spec.evidence.require_rationale && submission.rationale.empty()) {
    return fail<EvaluationId>(EnsembleError::malformed_evaluation,
                              "the ensemble requires a rationale with every evaluation");
  }
  if (submission.score_defined && !std::isfinite(submission.score)) {
    return fail<EvaluationId>(EnsembleError::malformed_evaluation,
                              "evaluation score is not a finite number");
  }
  if (!std::isfinite(submission.weight) || submission.weight <= 0.0) {
    return fail<EvaluationId>(EnsembleError::malformed_evaluation,
                              "evaluation weight must be finite and positive");
  }
  if (submission.kind == CriterionKind::hard_predicate &&
      submission.verification == VerificationState::unknown &&
      submission.judgment == CategoricalJudgment::unknown) {
    // An UNKNOWN hard predicate is legal and meaningful; it is recorded as
    // UNKNOWN and never silently becomes PASS.
  }

  const std::uint64_t fingerprint = fingerprint_evaluation(submission);
  for (const EvaluationRecord& existing : execution.evaluations) {
    if (existing.evaluator != authority.evaluator.participant ||
        existing.evaluator_generation != authority.evaluator.participant_generation ||
        !(existing.criterion == submission.criterion) || existing.candidate != authority.candidate) {
      continue;
    }
    impl_->counters.duplicate_votes_suppressed += 1u;
    if (existing.fingerprint == fingerprint) {
      return fail<EvaluationId>(EnsembleError::duplicate_evaluation,
                                "this evaluator already submitted identical evidence");
    }
    return fail<EvaluationId>(EnsembleError::conflicting_evaluation,
                              "this evaluator already submitted different evidence for the same "
                              "candidate and criterion");
  }

  EvaluationRecord record;
  record.id = pending->id;
  record.generation = pending->generation;
  record.evaluator = authority.evaluator.participant;
  record.evaluator_generation = authority.evaluator.participant_generation;
  record.role = pending->role;
  record.candidate = authority.candidate;
  record.candidate_generation = authority.candidate_generation;
  record.criterion = submission.criterion;
  record.kind = submission.kind;
  record.verification = submission.verification;
  record.judgment = submission.judgment;
  record.score_defined = submission.score_defined;
  record.score = submission.score;
  record.weight = submission.weight;
  record.evidence = submission.evidence;
  record.rationale =
      detail::sanitize_text(submission.rationale, impl_->config.limits.max_metadata_bytes);
  record.received_sequence = impl_->next_sequence();
  record.coordinator_epoch = impl_->epoch;
  record.worker = evaluator->worker;
  record.worker_boot = evaluator->worker_boot;
  record.fingerprint = fingerprint;
  execution.evaluations.push_back(std::move(record));
  pending->answered = true;
  evaluator->evaluations_submitted += 1u;

  CandidateEvaluationSummary summary = summarize_candidate(execution, *candidate);
  candidate->evaluation_count = summary.evaluations;
  candidate->hard_failures = summary.hard_failures;
  candidate->hard_unknowns = summary.hard_unknowns;
  candidate->hard_predicates = summary.hard_predicates;
  return authority.evaluation;
}

}  // namespace ensemble_fabric
