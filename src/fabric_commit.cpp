#include <algorithm>
#include <utility>

#include "detail/text.hpp"
#include "fabric_internal.hpp"

namespace ensemble_fabric {

CandidateSnapshot snapshot_candidate(const CandidateRecord& candidate) {
  CandidateSnapshot snapshot;
  snapshot.id = candidate.id;
  snapshot.generation = candidate.generation;
  snapshot.execution = EnsembleExecutionId();
  snapshot.attempt_generation = EnsembleAttemptGeneration();
  snapshot.participant = candidate.participant;
  snapshot.participant_generation = candidate.participant_generation;
  snapshot.role = candidate.role;
  snapshot.stage = candidate.stage;
  snapshot.state = candidate.state;
  snapshot.attempt = candidate.attempt;
  snapshot.attempts_used = candidate.attempts_used;
  snapshot.domain = candidate.domain;
  snapshot.payload = candidate.payload;
  snapshot.payload_digest = candidate.payload_digest;
  snapshot.payload_bytes = static_cast<std::uint32_t>(candidate.payload.size());
  snapshot.received_sequence = candidate.received_sequence;
  snapshot.coordinator_epoch = candidate.coordinator_epoch;
  snapshot.worker = candidate.worker;
  snapshot.worker_boot = candidate.worker_boot;
  snapshot.failure = candidate.failure;
  snapshot.failure_detail = candidate.failure_detail;
  snapshot.evaluation_count = candidate.evaluation_count;
  snapshot.hard_failures = candidate.hard_failures;
  snapshot.fallback = candidate.fallback;
  snapshot.current = candidate.current;
  snapshot.elimination_reason = candidate.elimination_reason;
  return snapshot;
}

EvaluationSnapshot snapshot_evaluation(const EvaluationRecord& evaluation) {
  EvaluationSnapshot snapshot;
  snapshot.id = evaluation.id;
  snapshot.generation = evaluation.generation;
  snapshot.evaluator = evaluation.evaluator;
  snapshot.evaluator_generation = evaluation.evaluator_generation;
  snapshot.evaluator_role = evaluation.role;
  snapshot.candidate = evaluation.candidate;
  snapshot.candidate_generation = evaluation.candidate_generation;
  snapshot.criterion = evaluation.criterion;
  snapshot.kind = evaluation.kind;
  snapshot.verification = evaluation.verification;
  snapshot.judgment = evaluation.judgment;
  snapshot.score_defined = evaluation.score_defined;
  snapshot.score = evaluation.score;
  snapshot.weight = evaluation.weight;
  snapshot.evidence = evaluation.evidence;
  snapshot.rationale = evaluation.rationale;
  snapshot.received_sequence = evaluation.received_sequence;
  snapshot.coordinator_epoch = evaluation.coordinator_epoch;
  snapshot.worker = evaluation.worker;
  snapshot.worker_boot = evaluation.worker_boot;
  snapshot.current = evaluation.current;
  snapshot.superseded = !evaluation.current;
  return snapshot;
}

namespace {

[[nodiscard]] EnsembleError consensus_error(ConsensusState state) noexcept {
  switch (state) {
    case ConsensusState::quorum_not_reached: return EnsembleError::quorum_not_reached;
    case ConsensusState::quorum_impossible: return EnsembleError::quorum_impossible;
    case ConsensusState::disagreement: return EnsembleError::disagreement_unresolved;
    case ConsensusState::tie: return EnsembleError::tie_unresolved;
    case ConsensusState::insufficient_evidence: return EnsembleError::insufficient_evidence;
    case ConsensusState::required_participant_failed: return EnsembleError::required_participant_failed;
    case ConsensusState::all_candidates_invalid: return EnsembleError::all_candidates_invalid;
    case ConsensusState::no_eligible_candidate: return EnsembleError::no_eligible_candidate;
    case ConsensusState::fallback_required: return EnsembleError::fallback_not_authorized;
    case ConsensusState::cancelled: return EnsembleError::ensemble_cancelled;
    case ConsensusState::superseded: return EnsembleError::ensemble_superseded;
    case ConsensusState::revalidation_required: return EnsembleError::revalidation_required;
    default: return EnsembleError::consensus_not_reached;
  }
}

/// Maps the consensus outcome and the presence of an authorized selection onto
/// the terminal decision.  Only states that are terminal on their own short
/// circuit; everything else commits if and only if a selection exists.
[[nodiscard]] EnsembleDecision decision_for(ConsensusState state, bool has_selection) noexcept {
  switch (state) {
    case ConsensusState::cancelled: return EnsembleDecision::cancelled;
    case ConsensusState::superseded: return EnsembleDecision::superseded;
    case ConsensusState::revalidation_required: return EnsembleDecision::revalidation_required;
    case ConsensusState::fallback_required: return EnsembleDecision::fallback_required;
    case ConsensusState::quorum_not_reached:
    case ConsensusState::quorum_impossible:
    case ConsensusState::required_participant_failed:
      return EnsembleDecision::failed;
    default:
      break;
  }
  return has_selection ? EnsembleDecision::committed : EnsembleDecision::no_eligible_candidate;
}

[[nodiscard]] ExecutionStatus status_for(EnsembleDecision decision) noexcept {
  switch (decision) {
    case EnsembleDecision::committed: return ExecutionStatus::committed;
    case EnsembleDecision::cancelled: return ExecutionStatus::cancelled;
    case EnsembleDecision::superseded: return ExecutionStatus::superseded;
    case EnsembleDecision::revalidation_required: return ExecutionStatus::revalidation_required;
    case EnsembleDecision::no_eligible_candidate: return ExecutionStatus::no_eligible_candidate;
    case EnsembleDecision::failed:
    case EnsembleDecision::fallback_required:
    case EnsembleDecision::pending:
      return ExecutionStatus::failed;
  }
  return ExecutionStatus::failed;
}

}  // namespace

void compute_reports(EnsembleRecord& ensemble, ExecutionRecord& execution) {
  execution.report_generation += 1u;
  const Sequence report_sequence(execution.report_generation);

  std::vector<QuorumMember> membership;
  membership.reserve(ensemble.participants.size());
  for (const ParticipantRecord& participant : ensemble.participants) {
    QuorumMember member;
    member.participant = participant.id;
    member.generation = participant.generation;
    member.roles = participant.declaration.roles;
    member.required = participant.declaration.required;
    member.authorized = participant.authoritative(execution.coordinator_epoch);
    member.available = participant.status != ParticipantStatus::unavailable &&
                       participant.status != ParticipantStatus::fenced &&
                       participant.status != ParticipantStatus::retired;
    member.failed = participant.status == ParticipantStatus::failed;
    member.fallback = participant.fallback_activated;
    member.role_available = participant.status != ParticipantStatus::retired;
    membership.push_back(member);
  }

  std::vector<EligibleVote> votes;
  for (const EvaluationRecord& evaluation : execution.evaluations) {
    if (!evaluation.current) {
      continue;
    }
    EligibleVote vote;
    vote.participant = evaluation.evaluator;
    vote.generation = evaluation.evaluator_generation;
    vote.role = evaluation.role;
    vote.candidate = evaluation.candidate;
    vote.candidate_generation = evaluation.candidate_generation;
    vote.weight = evaluation.weight;
    vote.abstained = evaluation.judgment == CategoricalJudgment::abstain ||
                     evaluation.verification == VerificationState::abstain;
    votes.push_back(vote);
  }

  std::uint32_t valid_candidates = 0;
  std::uint32_t invalid_candidates = 0;
  for (const CandidateRecord& candidate : execution.candidates) {
    if (is_content_state(candidate.state)) {
      valid_candidates += 1u;
    } else if (candidate.state == CandidateState::invalid ||
               candidate.state == CandidateState::failed) {
      invalid_candidates += 1u;
    }
  }

  execution.quorum = compute_quorum(ensemble.spec.quorum, membership, votes, valid_candidates,
                                    ensemble.spec.generation, execution.coordinator_epoch,
                                    report_sequence);
  execution.quorum.evaluations = static_cast<std::uint32_t>(votes.size());

  std::vector<CandidateTally> tallies;
  tallies.reserve(execution.candidates.size());
  for (const CandidateRecord& candidate : execution.candidates) {
    CandidateTally tally;
    tally.candidate = candidate.id;
    tally.generation = candidate.generation;
    tally.participant = candidate.participant;
    tally.eligible = is_content_state(candidate.state);
    for (const EvaluationRecord& evaluation : execution.evaluations) {
      if (!evaluation.current || evaluation.candidate != candidate.id) {
        continue;
      }
      if (evaluation.candidate_generation != candidate.generation) {
        continue;
      }
      switch (evaluation.judgment) {
        case CategoricalJudgment::accept:
          tally.agreeing += 1u;
          tally.weighted_agree += evaluation.weight;
          tally.agreeing_members.push_back(evaluation.evaluator);
          break;
        case CategoricalJudgment::reject:
          tally.disagreeing += 1u;
          tally.disagreeing_members.push_back(evaluation.evaluator);
          break;
        case CategoricalJudgment::abstain:
          tally.abstaining += 1u;
          tally.abstaining_members.push_back(evaluation.evaluator);
          break;
        case CategoricalJudgment::unknown:
          break;
      }
    }
    tallies.push_back(std::move(tally));
  }

  bool required_participant_failed = false;
  for (const ParticipantRecord& participant : ensemble.participants) {
    if (participant.declaration.required && participant.status == ParticipantStatus::failed) {
      required_participant_failed = true;
    }
  }

  bool fallback_required = false;
  if (ensemble.spec.fallback.enabled &&
      execution.fallback_depth >= ensemble.spec.fallback.max_depth) {
    // A fallback that has already produced content satisfies the demand: the
    // ensemble is then decided on that evidence rather than reported as still
    // waiting for a fallback it can no longer request.
    bool fallback_content = false;
    for (const CandidateRecord& candidate : execution.candidates) {
      if (candidate.fallback && is_content_state(candidate.state)) {
        fallback_content = true;
      }
    }
    if (!fallback_content) {
      for (const FallbackTrigger trigger : ensemble.spec.fallback.triggers) {
        if (fallback_trigger_fires(trigger, ensemble, execution)) {
          fallback_required = true;
          break;
        }
      }
    }
  }

  ConsensusInputs inputs;
  inputs.quorum = execution.quorum;
  inputs.tallies = std::move(tallies);
  inputs.candidates_declared = static_cast<std::uint32_t>(execution.candidates.size());
  inputs.invalid_candidates = invalid_candidates;
  inputs.required_participant_failed = required_participant_failed;
  inputs.require_all_required = ensemble.spec.completion.require_all_required_participants;
  inputs.cancelled = execution.status == ExecutionStatus::cancelled;
  inputs.superseded = execution.status == ExecutionStatus::superseded || ensemble.superseded;
  inputs.revalidation_required = execution.status == ExecutionStatus::revalidation_required;
  inputs.fallback_required = fallback_required;
  inputs.requires_votes = !evaluating_participants(ensemble.spec).empty();
  inputs.ensemble_generation = ensemble.spec.generation;
  inputs.coordinator_epoch = execution.coordinator_epoch;
  inputs.sequence = report_sequence;
  execution.consensus = compute_consensus(inputs);

  ArbitrationInputs arbitration_inputs;
  arbitration_inputs.policy = ensemble.spec.arbitration;
  arbitration_inputs.aggregation = ensemble.spec.aggregation;
  arbitration_inputs.evidence = ensemble.spec.evidence;
  arbitration_inputs.quorum_satisfied = execution.quorum.state == QuorumState::reached;
  arbitration_inputs.ensemble_generation = ensemble.spec.generation;
  arbitration_inputs.coordinator_epoch = execution.coordinator_epoch;
  arbitration_inputs.sequence = report_sequence;
  for (const CandidateRecord& candidate : execution.candidates) {
    ArbitrationCandidateInput input;
    input.candidate = candidate.id;
    input.generation = candidate.generation;
    input.participant = candidate.participant;
    input.participant_generation = candidate.participant_generation;
    input.role = candidate.role;
    input.state = candidate.state;
    input.generation_current = candidate.current && candidate.generation.value() != 0u;
    const ParticipantRecord* participant = EnsembleFabric::Impl::find_participant(ensemble, candidate.participant);
    input.participant_authoritative =
        participant != nullptr && participant->authoritative(execution.coordinator_epoch);
    input.compatible = participant != nullptr &&
                       check_compatibility(participant->declaration.compatibility,
                                           participant->profile)
                           .ok();
    input.required_participant = participant != nullptr && participant->declaration.required;
    input.fallback = candidate.fallback;
    input.role_selectable =
        participant != nullptr && participant->declaration.selectable();
    input.participant_priority =
        participant != nullptr ? participant->declaration.priority : 0u;
    input.execution_cost = candidate.budget_units_used;
    input.latency = candidate.latency;
    input.payload_bytes = static_cast<std::uint32_t>(candidate.payload.size());
    if (participant != nullptr && !participant->declaration.domain.empty() &&
        participant->declaration.domain == ensemble.spec.task_class) {
      input.specialist_matches = 1u;
    }
    input.evaluation = summarize_candidate(execution, candidate);
    arbitration_inputs.candidates.push_back(std::move(input));
  }
  execution.arbitration = arbitrate(arbitration_inputs);

  for (CandidateRanking& ranking : execution.arbitration.ranking) {
    for (CandidateRecord& candidate : execution.candidates) {
      if (candidate.id == ranking.candidate) {
        candidate.elimination_reason =
            ranking.eligible ? std::string() : std::string(to_string(ranking.elimination));
      }
    }
  }
}

namespace {

void build_explanation(const EnsembleRecord& ensemble, const ExecutionRecord& execution,
                       EnsembleResult& result, std::uint32_t max_entries) {
  auto add = [&result, max_entries](std::string subject, std::string detail) {
    if (result.explanation.size() >= max_entries) {
      return;
    }
    ExplanationEntry entry;
    entry.subject = std::move(subject);
    entry.detail = std::move(detail);
    result.explanation.push_back(std::move(entry));
  };
  add("ensemble", "id=" + ensemble.spec.id.to_string() + " generation=" +
                      ensemble.spec.generation.to_string() + " label='" + ensemble.spec.label + "'");
  add("epoch", "coordinator_epoch=" + execution.coordinator_epoch.to_string());
  add("quorum", execution.quorum.render());
  add("consensus", execution.consensus.render());
  add("arbitration", execution.arbitration.render());
  for (const std::string& note : execution.quorum.notes) {
    add("quorum_note", note);
  }
  for (const CandidateRanking& ranking : execution.arbitration.ranking) {
    if (ranking.eligible) {
      add("candidate " + ranking.candidate.to_string(),
          "eligible rank=" + std::to_string(ranking.rank) + " participant=" +
              ranking.participant.to_string() + " role=" +
              std::string(ensemble_fabric::to_string(ranking.role)));
      for (const RankingFactorValue& factor : ranking.factors) {
        add("  factor", std::string(ensemble_fabric::to_string(factor.factor)) + "=" +
                            detail::format_double(factor.value) + " (" + factor.detail + ")");
      }
    } else {
      add("candidate " + ranking.candidate.to_string(),
          "eliminated: " + std::string(ensemble_fabric::to_string(ranking.elimination)) + " - " +
              ranking.elimination_detail);
    }
  }
  for (const std::string& note : execution.arbitration.notes) {
    add("arbitration_note", note);
  }
  if (execution.fallback_activated) {
    add("fallback", "activated at depth " + std::to_string(execution.fallback_depth));
  }
  if (execution.cancellation_emitted) {
    add("cancellation", "reason: " + execution.cancellation_reason);
  }
}

}  // namespace

Outcome<EnsembleResult> EnsembleFabric::prepare_result(EnsembleId id,
                                                       EnsembleGeneration generation) {
  std::unique_lock lock(impl_->mutex);
  EnsembleRecord* ensemble = impl_->find(id);
  if (ensemble == nullptr) {
    return fail<EnsembleResult>(EnsembleError::unknown_ensemble, "unknown ensemble " + id.to_string());
  }
  if (ensemble->spec.generation != generation) {
    return fail<EnsembleResult>(EnsembleError::stale_ensemble_generation,
                                "result preparation targets a historical ensemble generation");
  }
  if (!ensemble->has_execution) {
    return fail<EnsembleResult>(EnsembleError::unknown_execution,
                                "the ensemble has no execution");
  }
  ExecutionRecord& execution = ensemble->execution;

  if (execution.has_result) {
    // A committed execution generation has exactly one authoritative logical
    // result, and re-preparing it must reproduce that result unchanged rather
    // than re-deriving a decision from post-commit bookkeeping.
    return execution.result;
  }

  compute_reports(*ensemble, execution);

  EnsembleResult result;
  result.id = ResultId(execution.id.value());
  result.generation = ResultGeneration(1);
  result.ensemble = ensemble->spec.id;
  result.ensemble_generation = ensemble->spec.generation;
  result.execution = execution.id;
  result.attempt_generation = execution.attempt_generation;
  result.coordinator_epoch = execution.coordinator_epoch;
  result.quorum = execution.quorum;
  result.consensus = execution.consensus;
  result.arbitration = execution.arbitration;
  result.fallback_used = execution.fallback_activated;
  result.fallback_depth = execution.fallback_depth;

  // A tie in the votes is reported truthfully.  Whether it may still produce a
  // result is an explicit arbitration-policy decision: a deterministic
  // tie-break authorizes one, and REPORT_TIE does not.
  const bool tie_resolved_by_policy =
      execution.consensus.state == ConsensusState::tie &&
      ensemble->spec.arbitration.tie_break != TieBreakPolicy::report_tie &&
      !execution.arbitration.tie_unresolved;
  const bool has_selection = execution.arbitration.selected.valid() &&
                             !execution.arbitration.no_eligible_candidate &&
                             !execution.arbitration.tie_unresolved &&
                             (execution.consensus.resolved() || tie_resolved_by_policy);
  result.decision = decision_for(execution.consensus.state, has_selection);

  for (const ParticipantRecord& participant : ensemble->participants) {
    ParticipantOutcome outcome;
    outcome.id = participant.id;
    outcome.generation = participant.generation;
    outcome.roles = participant.declaration.roles;
    outcome.required = participant.declaration.required;
    outcome.final_status = participant.status;
    outcome.candidates = 0;
    outcome.evaluations = participant.evaluations_submitted;
    outcome.retries = participant.retries_consumed;
    outcome.fallback = participant.fallback_activated;
    outcome.authoritative = participant.authoritative(execution.coordinator_epoch);
    for (const CandidateRecord& candidate : execution.candidates) {
      if (candidate.participant != participant.id) {
        continue;
      }
      outcome.candidates += 1u;
      if (is_content_state(candidate.state)) {
        outcome.candidates_valid += 1u;
      }
      if (candidate.state == CandidateState::failed) {
        outcome.candidates_failed += 1u;
      }
      if (candidate.state == CandidateState::abstained) {
        outcome.candidates_abstained += 1u;
      }
    }
    result.participants.push_back(std::move(outcome));
  }

  const std::uint32_t max_entries = impl_->config.limits.max_explanation_entries;
  std::uint32_t kept = 0;
  for (const CandidateRecord& candidate : execution.candidates) {
    if (kept >= max_entries) {
      break;
    }
    CandidateSnapshot snapshot = snapshot_candidate(candidate);
    snapshot.ensemble = ensemble->spec.id;
    snapshot.ensemble_generation = ensemble->spec.generation;
    snapshot.execution = execution.id;
    snapshot.attempt_generation = execution.attempt_generation;
    result.candidates.push_back(std::move(snapshot));
    kept += 1u;
  }
  kept = 0;
  for (const EvaluationRecord& evaluation : execution.evaluations) {
    if (kept >= max_entries) {
      break;
    }
    EvaluationSnapshot snapshot = snapshot_evaluation(evaluation);
    snapshot.ensemble = ensemble->spec.id;
    snapshot.ensemble_generation = ensemble->spec.generation;
    snapshot.execution = execution.id;
    snapshot.attempt_generation = execution.attempt_generation;
    result.evaluations.push_back(std::move(snapshot));
    kept += 1u;
  }

  if (result.decision == EnsembleDecision::committed && has_selection) {
    const CandidateRecord* selected = find_candidate(execution, execution.arbitration.selected);
    if (selected == nullptr || !is_content_state(selected->state)) {
      return fail<EnsembleResult>(EnsembleError::internal_error,
                                  "arbitration selected a candidate that holds no content");
    }
    result.has_selected = true;
    result.selected_candidate = selected->id;
    result.selected_candidate_generation = selected->generation;
    result.selected_participant = selected->participant;
    result.selected_participant_generation = selected->participant_generation;
    result.selected_role = selected->role;
    result.payload = selected->payload;
    result.payload_digest = selected->payload_digest;
    result.payload_bytes = static_cast<std::uint32_t>(selected->payload.size());
  }

  build_explanation(*ensemble, execution, result, max_entries);
  result.commit_fingerprint = commit_fingerprint(result);
  return result;
}

Outcome<EnsembleResult> EnsembleFabric::commit_result(const EnsembleResult& candidate) {
  std::unique_lock lock(impl_->mutex);
  EnsembleRecord* ensemble = impl_->find(candidate.ensemble);
  if (ensemble == nullptr) {
    return fail<EnsembleResult>(EnsembleError::unknown_ensemble, "unknown ensemble");
  }
  const Status validated = validate_result(candidate, ensemble->spec, impl_->epoch);
  if (!validated.ok()) {
    if (validated.code() == EnsembleError::stale_ensemble_generation ||
        validated.code() == EnsembleError::stale_coordinator_epoch) {
      impl_->counters.commits_rejected_stale += 1u;
    }
    return fail<EnsembleResult>(validated.code(), validated.error().message);
  }
  if (!ensemble->has_execution || ensemble->execution.id != candidate.execution) {
    return fail<EnsembleResult>(EnsembleError::stale_execution_generation,
                                "the result does not target the current execution generation");
  }
  ExecutionRecord& execution = ensemble->execution;
  execution.commit_attempts += 1u;

  if (execution.has_result) {
    if (execution.result.commit_fingerprint == candidate.commit_fingerprint) {
      // An identical repeated commit is idempotent: the authoritative logical
      // result does not change.
      return execution.result;
    }
    impl_->counters.commits_rejected_conflicting += 1u;
    return fail<EnsembleResult>(
        EnsembleError::conflicting_result_commit,
        "this execution generation already committed a different authoritative result");
  }
  if (execution.cancellation_requested || execution.status == ExecutionStatus::cancelled) {
    return fail<EnsembleResult>(EnsembleError::ensemble_cancelled,
                                "the execution was cancelled; it may no longer publish a result");
  }
  if (execution.status == ExecutionStatus::superseded || ensemble->superseded) {
    return fail<EnsembleResult>(EnsembleError::ensemble_superseded,
                                "the execution generation is superseded");
  }
  if (execution.status == ExecutionStatus::revalidation_required) {
    return fail<EnsembleResult>(EnsembleError::revalidation_required,
                                "the execution requires revalidation after coordinator recovery");
  }

  const auto result_id = impl_->result_ids.allocate();
  if (!result_id.has_value()) {
    return fail<EnsembleResult>(EnsembleError::resource_limit_exceeded,
                                "result identity space is exhausted");
  }

  EnsembleResult stored = candidate;
  stored.id = *result_id;
  stored.generation = ResultGeneration(1);
  stored.commit_sequence = impl_->next_sequence();
  stored.commit_fingerprint = commit_fingerprint(stored);

  CommitRecord record;
  record.result = stored.id;
  record.generation = stored.generation;
  record.sequence = stored.commit_sequence;
  record.coordinator_epoch = stored.coordinator_epoch;
  record.execution = stored.execution;
  record.attempt_generation = stored.attempt_generation;
  record.decision = stored.decision;
  record.commit_fingerprint = stored.commit_fingerprint;

  execution.result = stored;
  execution.has_result = true;
  execution.committed_sequence = stored.commit_sequence;
  execution.status = status_for(stored.decision);
  execution.commits.push_back(record);
  const std::size_t retained = impl_->config.limits.max_retained_commits;
  if (execution.commits.size() > retained) {
    execution.commits.erase(execution.commits.begin(),
                            execution.commits.begin() + static_cast<std::ptrdiff_t>(
                                                          execution.commits.size() - retained));
  }
  if (stored.decision == EnsembleDecision::committed) {
    impl_->counters.commits_accepted += 1u;
    for (CandidateRecord& record_candidate : execution.candidates) {
      if (record_candidate.id == stored.selected_candidate) {
        record_candidate.state = CandidateState::selected;
      } else if (is_content_state(record_candidate.state)) {
        record_candidate.state = CandidateState::rejected;
      }
    }
    // The committed result must describe the same states the runtime now holds.
    std::uint32_t kept = 0;
    stored.candidates.clear();
    for (const CandidateRecord& record_candidate : execution.candidates) {
      if (kept >= impl_->config.limits.max_explanation_entries) {
        break;
      }
      CandidateSnapshot view = snapshot_candidate(record_candidate);
      view.ensemble = ensemble->spec.id;
      view.ensemble_generation = ensemble->spec.generation;
      view.execution = execution.id;
      view.attempt_generation = execution.attempt_generation;
      stored.candidates.push_back(std::move(view));
      kept += 1u;
    }
    execution.result = stored;
  }
  return stored;
}

Outcome<EnsembleResult> EnsembleFabric::finalize(EnsembleId id, EnsembleGeneration generation) {
  EnsembleExecutionId execution_id;
  {
    std::shared_lock lock(impl_->mutex);
    const EnsembleRecord* ensemble = impl_->find(id);
    if (ensemble == nullptr) {
      return fail<EnsembleResult>(EnsembleError::unknown_ensemble,
                                  "unknown ensemble " + id.to_string());
    }
    if (ensemble->spec.generation != generation) {
      return fail<EnsembleResult>(EnsembleError::stale_ensemble_generation,
                                  "finalize targets a historical ensemble generation");
    }
    if (!ensemble->has_execution) {
      return fail<EnsembleResult>(EnsembleError::unknown_execution, "the ensemble has no execution");
    }
    if (!execution_settled(*ensemble)) {
      return fail<EnsembleResult>(
          EnsembleError::invalid_state_transition,
          "the execution has not settled: candidates or evaluations are still in flight");
    }
    // Terminal execution states are reported before any epoch comparison, so
    // that recovery and cancellation produce their own specific outcome.
    switch (ensemble->execution.status) {
      case ExecutionStatus::revalidation_required:
        return fail<EnsembleResult>(
            EnsembleError::revalidation_required,
            "the execution requires revalidation after coordinator recovery");
      case ExecutionStatus::cancelled:
        return fail<EnsembleResult>(EnsembleError::ensemble_cancelled,
                                    "the execution was cancelled");
      case ExecutionStatus::superseded:
        return fail<EnsembleResult>(EnsembleError::ensemble_superseded,
                                    "the execution generation is superseded");
      default:
        break;
    }
    if (ensemble->superseded) {
      return fail<EnsembleResult>(EnsembleError::ensemble_superseded,
                                  "the ensemble generation is superseded");
    }
    execution_id = ensemble->execution.id;
  }

  Outcome<EnsembleResult> prepared = prepare_result(id, generation);
  if (!prepared.ok()) {
    return prepared;
  }
  EnsembleResult result = std::move(prepared).value();
  if (result.execution != execution_id) {
    return fail<EnsembleResult>(EnsembleError::internal_error,
                                "the execution generation changed during finalization");
  }
  Outcome<EnsembleResult> committed = commit_result(result);
  if (!committed.ok()) {
    return committed;
  }
  if (committed.value().decision != EnsembleDecision::committed) {
    return fail<EnsembleResult>(consensus_error(committed.value().consensus.state),
                                "the ensemble reached a terminal non-success decision: " +
                                    std::string(to_string(committed.value().consensus.state)));
  }
  return committed;
}

Status EnsembleFabric::cancel_ensemble(EnsembleId id, EnsembleGeneration generation,
                                       std::string reason) {
  std::unique_lock lock(impl_->mutex);
  EnsembleRecord* ensemble = impl_->find(id);
  if (ensemble == nullptr) {
    return fail(EnsembleError::unknown_ensemble, "unknown ensemble " + id.to_string());
  }
  if (ensemble->spec.generation != generation) {
    return fail(EnsembleError::stale_ensemble_generation,
                "cancellation targets a historical ensemble generation");
  }
  if (!ensemble->has_execution) {
    return fail(EnsembleError::unknown_execution, "the ensemble has no execution to cancel");
  }
  ExecutionRecord& execution = ensemble->execution;
  if (execution.has_result) {
    return fail(EnsembleError::result_already_committed,
                "the execution already committed an authoritative result; cancellation does not "
                "revoke a committed result");
  }
  if (execution.status == ExecutionStatus::cancelled) {
    return ok_status();
  }
  execution.cancellation_requested = true;
  execution.cancellation_reason = detail::sanitize_text(reason, impl_->config.limits.max_metadata_bytes);
  execution.cancelled_sequence = impl_->next_sequence();
  execution.status = ExecutionStatus::cancelled;
  execution.committed_sequence = Sequence();
  return ok_status();
}

Outcome<EnsembleResult> EnsembleFabric::current_result(EnsembleId id) const {
  std::shared_lock lock(impl_->mutex);
  const EnsembleRecord* ensemble = impl_->find(id);
  if (ensemble == nullptr) {
    return fail<EnsembleResult>(EnsembleError::unknown_ensemble, "unknown ensemble " + id.to_string());
  }
  if (!ensemble->has_execution || !ensemble->execution.has_result) {
    return fail<EnsembleResult>(EnsembleError::not_found,
                                "the ensemble has not committed a result in this execution");
  }
  return ensemble->execution.result;
}

}  // namespace ensemble_fabric
