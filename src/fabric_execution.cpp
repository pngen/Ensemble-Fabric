#include <algorithm>
#include <utility>

#include "detail/text.hpp"
#include "fabric_internal.hpp"

namespace ensemble_fabric {
namespace {

[[nodiscard]] ParticipantRole stage_role(const ParticipantSpec& participant,
                                         const StageSpec& stage) noexcept {
  if (stage.fallback_stage && participant.roles.contains(ParticipantRole::fallback)) {
    return ParticipantRole::fallback;
  }
  if (participant.roles.contains(ParticipantRole::specialist)) {
    return ParticipantRole::specialist;
  }
  if (participant.roles.contains(ParticipantRole::candidate)) {
    return ParticipantRole::candidate;
  }
  return ParticipantRole::fallback;
}

[[nodiscard]] bool stage_complete(const StageSpec& stage, const ExecutionRecord& execution) {
  for (const ParticipantId id : stage.participants) {
    bool found = false;
    for (const CandidateRecord& candidate : execution.candidates) {
      if (candidate.participant == id && candidate.stage == stage.id) {
        found = true;
        if (candidate.in_flight()) {
          return false;
        }
      }
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

}  // namespace

CandidateEvaluationSummary summarize_candidate(const ExecutionRecord& execution,
                                               const CandidateRecord& candidate) {
  CandidateEvaluationSummary summary;
  summary.candidate = candidate.id;
  summary.generation = candidate.generation;
  bool first_score = true;
  std::vector<ParticipantId> distinct;
  for (const EvaluationRecord& evaluation : execution.evaluations) {
    if (evaluation.candidate != candidate.id || !evaluation.current) {
      continue;
    }
    if (evaluation.candidate_generation != candidate.generation) {
      // An evaluation that targets a superseded candidate generation is not
      // evidence about the current one.
      continue;
    }
    summary.evaluations += 1u;
    if (std::find_if(distinct.begin(), distinct.end(), [&evaluation](ParticipantId id) {
          return id == evaluation.evaluator;
        }) == distinct.end()) {
      distinct.push_back(evaluation.evaluator);
    }
    switch (evaluation.judgment) {
      case CategoricalJudgment::accept: summary.accepts += 1u; break;
      case CategoricalJudgment::reject: summary.rejects += 1u; break;
      case CategoricalJudgment::abstain: summary.abstentions += 1u; break;
      case CategoricalJudgment::unknown: summary.unknowns += 1u; break;
    }
    if (evaluation.kind == CriterionKind::hard_predicate) {
      summary.hard_predicates += 1u;
      switch (evaluation.verification) {
        case VerificationState::pass: break;
        case VerificationState::fail: summary.hard_failures += 1u; break;
        case VerificationState::abstain:
        case VerificationState::unknown: summary.hard_unknowns += 1u; break;
      }
    }
    switch (evaluation.verification) {
      case VerificationState::pass: summary.verifier_pass += 1u; break;
      case VerificationState::fail: summary.verifier_fail += 1u; break;
      case VerificationState::abstain: summary.verifier_abstain += 1u; break;
      case VerificationState::unknown: summary.verifier_unknown += 1u; break;
    }
    if (evaluation.score_defined) {
      summary.weighted_score_sum += evaluation.score * evaluation.weight;
      summary.weight_sum += evaluation.weight;
      if (first_score) {
        summary.min_score = evaluation.score;
        summary.max_score = evaluation.score;
        first_score = false;
      } else {
        summary.min_score = std::min(summary.min_score, evaluation.score);
        summary.max_score = std::max(summary.max_score, evaluation.score);
      }
    }
  }
  summary.distinct_evaluators = static_cast<std::uint32_t>(distinct.size());
  return summary;
}

bool execution_settled(const EnsembleRecord& ensemble) {
  if (!ensemble.has_execution) {
    return false;
  }
  const ExecutionRecord& execution = ensemble.execution;
  if (execution.status != ExecutionStatus::open) {
    return true;
  }
  // Work that was never even planned is not finished work: an execution whose
  // stage plan has not been consumed cannot be finalised.
  std::size_t stage_count = effective_stages(ensemble.spec).size();
  stage_count += execution.extra_stages.size();
  if (execution.stage_index < stage_count) {
    return false;
  }
  for (const CandidateRecord& candidate : execution.candidates) {
    if (!candidate.in_flight()) {
      continue;
    }
    const ParticipantRecord* participant =
        EnsembleFabric::Impl::find_participant(ensemble, candidate.participant);
    if (participant != nullptr && participant->capable()) {
      return false;
    }
  }
  for (const PendingEvaluation& pending : execution.pending) {
    if (pending.answered || pending.abandoned) {
      continue;
    }
    const ParticipantRecord* participant = EnsembleFabric::Impl::find_participant(ensemble, pending.evaluator);
    if (participant != nullptr && participant->capable()) {
      return false;
    }
  }
  return true;
}

Outcome<EnsembleExecutionId> EnsembleFabric::open_execution(EnsembleId id,
                                                            EnsembleGeneration generation) {
  std::unique_lock lock(impl_->mutex);
  EnsembleRecord* ensemble = impl_->find(id);
  if (ensemble == nullptr) {
    return fail<EnsembleExecutionId>(EnsembleError::unknown_ensemble,
                                     "unknown ensemble " + id.to_string());
  }
  if (ensemble->spec.generation != generation) {
    return fail<EnsembleExecutionId>(
        EnsembleError::stale_ensemble_generation,
        "execution requested for generation " + generation.to_string() +
            " but the current generation is " + ensemble->spec.generation.to_string());
  }
  if (ensemble->superseded) {
    return fail<EnsembleExecutionId>(EnsembleError::ensemble_superseded,
                                     "the ensemble generation is superseded");
  }
  if (ensemble->has_execution && ensemble->execution.status == ExecutionStatus::open) {
    return fail<EnsembleExecutionId>(
        EnsembleError::invalid_state_transition,
        "an execution is already open for this ensemble generation; cancel or commit it first");
  }
  std::size_t in_flight = 0;
  for (const auto& entry : impl_->ensembles) {
    if (entry.second.has_execution &&
        entry.second.execution.status == ExecutionStatus::open) {
      in_flight += 1u;
    }
  }
  if (in_flight >= impl_->config.limits.max_in_flight_executions) {
    return fail<EnsembleExecutionId>(EnsembleError::resource_limit_exceeded,
                                     "the in-flight execution bound is reached");
  }
  ExecutionRecord execution;
  if (ensemble->has_execution &&
      ensemble->execution.id.value() == static_cast<std::uint64_t>(-1)) {
    return fail<EnsembleExecutionId>(EnsembleError::resource_limit_exceeded,
                                     "execution identity space is exhausted");
  }
  execution.id = EnsembleExecutionId(ensemble->has_execution ? ensemble->execution.id.value() + 1u
                                                             : 1u);
  execution.attempt_generation = EnsembleAttemptGeneration(1);
  execution.status = ExecutionStatus::open;
  execution.coordinator_epoch = impl_->epoch;
  ensemble->execution = std::move(execution);
  ensemble->has_execution = true;
  return ensemble->execution.id;
}

Outcome<bool> EnsembleFabric::is_settled(EnsembleId id, EnsembleGeneration generation) const {
  std::shared_lock lock(impl_->mutex);
  const EnsembleRecord* ensemble = impl_->find(id);
  if (ensemble == nullptr) {
    return fail<bool>(EnsembleError::unknown_ensemble, "unknown ensemble " + id.to_string());
  }
  if (ensemble->spec.generation != generation) {
    return fail<bool>(EnsembleError::stale_ensemble_generation,
                      "settled query targets a historical ensemble generation");
  }
  return execution_settled(*ensemble);
}

namespace {

/// Collects the answer to "may fallback be activated now, and why".
struct FallbackDecision {
  bool activate{false};
  FallbackTrigger trigger{FallbackTrigger::participant_unavailable};
};

}  // namespace

bool fallback_trigger_fires(FallbackTrigger trigger, const EnsembleRecord& ensemble,
                            const ExecutionRecord& execution) {
  switch (trigger) {
    case FallbackTrigger::participant_unavailable: {
      for (const ParticipantRecord& participant : ensemble.participants) {
        if (!participant.declaration.required) {
          continue;
        }
        if (participant.status == ParticipantStatus::unavailable ||
            participant.status == ParticipantStatus::fenced ||
            (participant.status == ParticipantStatus::failed && !participant.registered)) {
          return true;
        }
      }
      return false;
    }
    case FallbackTrigger::required_participant_failed: {
      for (const ParticipantRecord& participant : ensemble.participants) {
        if (participant.declaration.required && participant.status == ParticipantStatus::failed) {
          return true;
        }
      }
      return false;
    }
    case FallbackTrigger::retry_exhausted: {
      for (const CandidateRecord& candidate : execution.candidates) {
        if (candidate.state == CandidateState::failed &&
            candidate.attempts_used >= ensemble.spec.retry.max_attempts) {
          return true;
        }
      }
      return false;
    }
    case FallbackTrigger::all_primary_invalid: {
      bool any_primary = false;
      for (const CandidateRecord& candidate : execution.candidates) {
        if (candidate.fallback) {
          continue;
        }
        any_primary = true;
        if (is_content_state(candidate.state)) {
          return false;
        }
      }
      return any_primary;
    }
    case FallbackTrigger::quorum_impossible: {
      return execution.quorum.state == QuorumState::impossible;
    }
    case FallbackTrigger::required_role_unavailable: {
      for (const RoleQuota& quota : ensemble.spec.quorum.role_quota) {
        std::uint32_t capable = 0;
        for (const ParticipantRecord& participant : ensemble.participants) {
          if (participant.declaration.roles.contains(quota.role) && participant.capable()) {
            capable += 1u;
          }
        }
        if (capable < quota.min_count) {
          return true;
        }
      }
      return false;
    }
    case FallbackTrigger::insufficient_evidence: {
      for (const ParticipantSpec* spec : evaluating_participants(ensemble.spec)) {
        const ParticipantRecord* participant = EnsembleFabric::Impl::find_participant(ensemble, spec->id);
        if (participant != nullptr && participant->capable()) {
          return false;
        }
      }
      return true;
    }
    case FallbackTrigger::judge_disagreement: {
      return execution.consensus.state == ConsensusState::disagreement ||
             execution.consensus.state == ConsensusState::tie ||
             execution.consensus.state == ConsensusState::insufficient_evidence;
    }
  }
  return false;
}

Outcome<std::vector<FabricAction>> EnsembleFabric::advance(EnsembleId id,
                                                           EnsembleGeneration generation) {
  std::unique_lock lock(impl_->mutex);
  std::vector<FabricAction> actions;
  EnsembleRecord* ensemble = impl_->find(id);
  if (ensemble == nullptr) {
    return fail<std::vector<FabricAction>>(EnsembleError::unknown_ensemble,
                                           "unknown ensemble " + id.to_string());
  }
  if (ensemble->spec.generation != generation) {
    return fail<std::vector<FabricAction>>(
        EnsembleError::stale_ensemble_generation,
        "advance requested for a historical ensemble generation");
  }
  if (!ensemble->has_execution) {
    return actions;
  }
  ExecutionRecord& execution = ensemble->execution;
  if (execution.status == ExecutionStatus::cancelled) {
    if (!execution.cancellation_emitted) {
      execution.cancellation_emitted = true;
      impl_->counters.cancellations += 1u;
      for (CandidateRecord& candidate : execution.candidates) {
        if (!candidate.in_flight()) {
          continue;
        }
        CancelAction cancel;
        cancel.execution = ExecutionRef{ensemble->spec.id, ensemble->spec.generation, execution.id,
                                        execution.attempt_generation};
        cancel.participant = candidate.participant;
        cancel.participant_generation = candidate.participant_generation;
        cancel.candidate = candidate.id;
        cancel.candidate_generation = candidate.generation;
        cancel.reason = execution.cancellation_reason;
        cancel.sequence = impl_->next_sequence();
        actions.emplace_back(std::move(cancel));
        candidate.state = CandidateState::cancelled;
        candidate.current = false;
      }
      for (PendingEvaluation& pending : execution.pending) {
        pending.abandoned = true;
      }
    }
    return actions;
  }
  if (execution.status != ExecutionStatus::open) {
    return actions;
  }
  if (ensemble->superseded) {
    execution.status = ExecutionStatus::superseded;
    for (CandidateRecord& candidate : execution.candidates) {
      if (candidate.in_flight()) {
        candidate.state = CandidateState::superseded;
        candidate.current = false;
      }
    }
    return actions;
  }
  std::vector<StageSpec> stages = effective_stages(ensemble->spec);
  for (const StageSpec& extra : execution.extra_stages) {
    stages.push_back(extra);
  }

  const Limits& limits = impl_->config.limits;
  bool progress = true;
  int guard = 0;
  while (progress && guard < 64) {
    guard += 1;
    progress = false;

    // ---- Stage declaration and dispatch ----------------------------------
    while (execution.stage_index < stages.size()) {
      const StageSpec& stage = stages[execution.stage_index];
      if (!execution.declared_current_stage) {
        for (const ParticipantId participant_id : stage.participants) {
          const ParticipantSpec* declaration = find_participant(ensemble->spec, participant_id);
          if (declaration == nullptr) {
            continue;
          }
          bool exists = false;
          for (const CandidateRecord& candidate : execution.candidates) {
            if (candidate.participant == participant_id && candidate.stage == stage.id) {
              exists = true;
              break;
            }
          }
          if (exists) {
            continue;
          }
          if (execution.candidates.size() >= limits.max_candidates_per_execution) {
            return fail<std::vector<FabricAction>>(
                EnsembleError::resource_limit_exceeded,
                "the candidate bound for this execution is reached");
          }
          const auto candidate_id = impl_->candidate_ids.allocate();
          if (!candidate_id.has_value()) {
            return fail<std::vector<FabricAction>>(EnsembleError::resource_limit_exceeded,
                                                   "candidate identity space is exhausted");
          }
          CandidateRecord candidate;
          candidate.id = *candidate_id;
          candidate.generation = CandidateGeneration(1);
          candidate.participant = participant_id;
          const ParticipantRecord* participant =
              EnsembleFabric::Impl::find_participant(*ensemble, participant_id);
          candidate.participant_generation =
              participant != nullptr ? participant->generation : ParticipantGeneration(1);
          candidate.role = stage_role(*declaration, stage);
          candidate.stage = stage.id;
          candidate.state = CandidateState::declared;
          candidate.domain = declaration->domain;
          candidate.fallback = stage.fallback_stage;
          execution.candidates.push_back(std::move(candidate));
        }
        execution.declared_current_stage = true;
        progress = true;
      }

      // Dispatch every dispatchable candidate of the current stage.
      bool dispatched_any = false;
      bool sequential_blocked = false;
      for (CandidateRecord& candidate : execution.candidates) {
        if (candidate.stage != stage.id || candidate.state != CandidateState::declared) {
          continue;
        }
        ParticipantRecord* participant = EnsembleFabric::Impl::find_participant(*ensemble, candidate.participant);
        if (participant == nullptr) {
          continue;
        }
        if (candidate.participant_generation != participant->generation) {
          candidate.participant_generation = participant->generation;
        }
        if (!participant->authoritative(impl_->epoch)) {
          if (!participant->capable()) {
            const bool awaiting_replacement =
                ensemble->spec.retry.replace_participant &&
                candidate.attempts_used < ensemble->spec.retry.max_attempts;
            if (!awaiting_replacement) {
              fail_candidate(*ensemble, execution, candidate,
                             EnsembleError::participant_unavailable,
                             "participant " + participant->id.to_string() +
                                 " is " +
                                 std::string(ensemble_fabric::to_string(participant->status)) +
                                 " and cannot serve this candidate");
              progress = true;
            }
          }
          if (stage.mode == TopologyMode::sequential) {
            sequential_blocked = true;
          }
          continue;
        }
        if (stage.mode == TopologyMode::sequential && !dispatched_any) {
          const bool earlier_pending =
              [&execution, &stage, &candidate]() {
                for (const CandidateRecord& other : execution.candidates) {
                  if (other.stage != stage.id || other.id == candidate.id) {
                    continue;
                  }
                  if (other.id.value() < candidate.id.value() && other.in_flight()) {
                    return true;
                  }
                }
                return false;
              }();
          if (earlier_pending) {
            sequential_blocked = true;
            break;
          }
        }

        DispatchAction action;
        action.execution = ExecutionRef{ensemble->spec.id, ensemble->spec.generation, execution.id,
                                        execution.attempt_generation};
        action.candidate = candidate.id;
        action.candidate_generation = candidate.generation;
        action.participant = candidate.participant;
        action.participant_generation = participant->generation;
        action.role = candidate.role;
        action.stage = candidate.stage;
        action.attempt = candidate.attempt;
        action.budget_units = participant->declaration.execution_budget_units;
        action.deterministic_seed = ensemble->spec.deterministic_seed;
        action.domain = candidate.domain;
        action.sequence = impl_->next_sequence();
        const std::string& task = ensemble->spec.task_class;
        action.request.assign(reinterpret_cast<const std::byte*>(task.data()),
                              reinterpret_cast<const std::byte*>(task.data()) + task.size());
        candidate.state = CandidateState::dispatched;
        candidate.dispatch_sequence = action.sequence;
        candidate.attempts_used += 1u;
        candidate.coordinator_epoch = impl_->epoch;
        candidate.worker = participant->worker;
        candidate.worker_boot = participant->worker_boot;
        actions.emplace_back(std::move(action));
        dispatched_any = true;
        progress = true;
        if (stage.mode == TopologyMode::sequential) {
          break;
        }
      }
      (void)sequential_blocked;

      if (!stage_complete(stage, execution)) {
        break;
      }
      execution.stage_index += 1u;
      execution.declared_current_stage = false;
      progress = true;
    }

    // ---- Evaluation planning --------------------------------------------
    if (execution.stage_index >= stages.size()) {
      // A candidate that only became eligible after the plan was built (for
      // example because a retry succeeded) must still receive its evaluations.
      if (execution.planning_complete) {
        for (const CandidateRecord& candidate : execution.candidates) {
          if (!is_content_state(candidate.state)) {
            continue;
          }
          bool evaluated = false;
          for (const EvaluationRecord& evaluation : execution.evaluations) {
            if (evaluation.candidate == candidate.id) {
              evaluated = true;
            }
          }
          for (const PendingEvaluation& pending : execution.pending) {
            if (pending.candidate == candidate.id) {
              evaluated = true;
            }
          }
          if (!evaluated && !evaluating_participants(ensemble->spec).empty()) {
            execution.planning_complete = false;
            progress = true;
          }
        }
      }
      if (!execution.planning_complete) {
        execution.planning_complete = true;
        const std::vector<CriterionRef> judge_criteria = judging_criteria(ensemble->spec);
        for (const CandidateRecord& candidate : execution.candidates) {
          if (!is_content_state(candidate.state)) {
            continue;
          }
          for (const ParticipantSpec* evaluator : evaluating_participants(ensemble->spec)) {
            if (evaluator->roles.contains(ParticipantRole::verifier)) {
              for (const CriterionRef& criterion : ensemble->spec.evidence.mandatory_criteria) {
                const auto allocated = impl_->evaluation_ids.allocate();
                if (!allocated.has_value()) {
                  return fail<std::vector<FabricAction>>(
                      EnsembleError::resource_limit_exceeded,
                      "evaluation identity space is exhausted");
                }
                PendingEvaluation pending;
                pending.id = *allocated;
                pending.generation = EvaluationGeneration(1);
                pending.evaluator = evaluator->id;
                const ParticipantRecord* record = EnsembleFabric::Impl::find_participant(*ensemble, evaluator->id);
                pending.evaluator_generation =
                    record != nullptr ? record->generation : ParticipantGeneration(1);
                pending.role = ParticipantRole::verifier;
                pending.candidate = candidate.id;
                pending.candidate_generation = candidate.generation;
                pending.criterion = criterion;
                pending.kind = CriterionKind::hard_predicate;
                execution.pending.push_back(std::move(pending));
              }
            }
            if (evaluator->roles.contains(ParticipantRole::judge)) {
              for (const CriterionRef& criterion : judge_criteria) {
                const auto allocated = impl_->evaluation_ids.allocate();
                if (!allocated.has_value()) {
                  return fail<std::vector<FabricAction>>(
                      EnsembleError::resource_limit_exceeded,
                      "evaluation identity space is exhausted");
                }
                PendingEvaluation pending;
                pending.id = *allocated;
                pending.generation = EvaluationGeneration(1);
                pending.evaluator = evaluator->id;
                const ParticipantRecord* record = EnsembleFabric::Impl::find_participant(*ensemble, evaluator->id);
                pending.evaluator_generation =
                    record != nullptr ? record->generation : ParticipantGeneration(1);
                pending.role = ParticipantRole::judge;
                pending.candidate = candidate.id;
                pending.candidate_generation = candidate.generation;
                pending.criterion = criterion;
                pending.kind = CriterionKind::ranking_signal;
                execution.pending.push_back(std::move(pending));
              }
            }
          }
        }
        progress = true;
      }

      for (PendingEvaluation& pending : execution.pending) {
        if (pending.answered || pending.abandoned || pending.emitted) {
          continue;
        }
        ParticipantRecord* evaluator = EnsembleFabric::Impl::find_participant(*ensemble, pending.evaluator);
        if (evaluator == nullptr) {
          pending.abandoned = true;
          continue;
        }
        if (pending.evaluator_generation != evaluator->generation) {
          // The logical evaluator was replaced; its pending plan belongs to a
          // previous incarnation and is not silently re-attributed.
          pending.abandoned = true;
          continue;
        }
        if (!evaluator->authoritative(impl_->epoch)) {
          if (!evaluator->capable()) {
            pending.abandoned = true;
            progress = true;
          }
          continue;
        }
        const CandidateRecord* candidate = find_candidate(execution, pending.candidate);
        if (candidate == nullptr || candidate->generation != pending.candidate_generation ||
            !is_content_state(candidate->state)) {
          pending.abandoned = true;
          progress = true;
          continue;
        }
        EvaluationAction action;
        action.execution = ExecutionRef{ensemble->spec.id, ensemble->spec.generation, execution.id,
                                        execution.attempt_generation};
        action.evaluation = pending.id;
        action.evaluation_generation = pending.generation;
        action.judge = pending.evaluator;
        action.judge_generation = evaluator->generation;
        action.role = pending.role;
        action.candidate = pending.candidate;
        action.candidate_generation = pending.candidate_generation;
        action.criterion = pending.criterion;
        action.criterion_kind = pending.kind;
        action.domain = candidate->domain;
        action.sequence = impl_->next_sequence();
        pending.emitted = true;
        actions.emplace_back(std::move(action));
        progress = true;
      }

      // ---- Fallback activation ------------------------------------------
      compute_reports(*ensemble, execution);
      if (ensemble->spec.fallback.enabled && !execution.fallback_activated &&
          execution.fallback_depth < ensemble->spec.fallback.max_depth) {
        bool waiting = false;
        for (const PendingEvaluation& pending : execution.pending) {
          if (pending.emitted && !pending.answered && !pending.abandoned) {
            waiting = true;
          }
        }
        for (const CandidateRecord& candidate : execution.candidates) {
          if (candidate.in_flight()) {
            waiting = true;
          }
        }
        if (!waiting) {
          FallbackDecision decision;
          for (const FallbackTrigger trigger : ensemble->spec.fallback.triggers) {
            if (fallback_trigger_fires(trigger, *ensemble, execution)) {
              decision.activate = true;
              decision.trigger = trigger;
              break;
            }
          }
          if (decision.activate) {
            StageSpec stage;
            stage.id = StageId(1000u + execution.fallback_depth + 1u);
            stage.label = "fallback";
            stage.mode = TopologyMode::parallel;
            stage.fallback_stage = true;
            for (const ParticipantId fallback_id : ensemble->spec.fallback.fallback_participants) {
              stage.participants.push_back(fallback_id);
            }
            execution.extra_stages.push_back(stage);
            execution.fallback_activated = true;
            execution.fallback_depth += 1u;
            execution.planning_complete = false;
            impl_->counters.fallback_activations += 1u;
            for (const ParticipantId fallback_id : stage.participants) {
              ParticipantRecord* participant = EnsembleFabric::Impl::find_participant(*ensemble, fallback_id);
              if (participant != nullptr) {
                participant->fallback_activated = true;
              }
              FallbackAction action;
              action.execution = ExecutionRef{ensemble->spec.id, ensemble->spec.generation,
                                              execution.id, execution.attempt_generation};
              action.participant = fallback_id;
              action.participant_generation =
                  participant != nullptr ? participant->generation : ParticipantGeneration(1);
              action.trigger = decision.trigger;
              action.depth = execution.fallback_depth;
              action.sequence = impl_->next_sequence();
              actions.emplace_back(std::move(action));
            }
            stages.push_back(execution.extra_stages.back());
            progress = true;
          }
        }
      }
    }
  }

  compute_reports(*ensemble, execution);
  return actions;
}

}  // namespace ensemble_fabric
