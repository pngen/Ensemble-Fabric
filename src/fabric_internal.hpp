#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "ensemble_fabric/fabric.hpp"

namespace ensemble_fabric {

/// Per-participant mutable runtime record.  Holding a *logical* participant and
/// a *physical* worker incarnation separately is what makes stale-worker
/// fencing possible.
struct ParticipantRecord {
  ParticipantId id;
  ParticipantGeneration generation;
  ParticipantSpec declaration;
  ParticipantStatus status{ParticipantStatus::revalidation_required};
  WorkerId worker;
  WorkerBootId worker_boot;
  CoordinatorEpoch coordinator_epoch;
  CompatibilityProfile profile;
  bool registered{false};
  std::string failure_detail;
  std::uint32_t replacement_count{0};
  std::uint32_t candidates_completed{0};
  std::uint32_t candidates_failed{0};
  std::uint32_t candidates_abstained{0};
  std::uint32_t evaluations_submitted{0};
  std::uint32_t retries_consumed{0};
  bool fallback_activated{false};

  [[nodiscard]] bool authoritative(CoordinatorEpoch current_epoch) const noexcept {
    return registered && status == ParticipantStatus::current && coordinator_epoch == current_epoch;
  }
  [[nodiscard]] bool capable() const noexcept {
    return status == ParticipantStatus::current || status == ParticipantStatus::revalidation_required;
  }
};

/// One logical candidate slot with its retry attempts.
struct CandidateRecord {
  CandidateId id;
  CandidateGeneration generation;
  ParticipantId participant;
  ParticipantGeneration participant_generation;
  ParticipantRole role{ParticipantRole::candidate};
  StageId stage;
  CandidateState state{CandidateState::declared};
  std::uint32_t attempt{1};
  std::uint32_t attempts_used{0};
  std::string domain;
  std::vector<std::byte> payload;
  std::uint64_t payload_digest{0};
  Sequence received_sequence;
  WorkerId worker;
  WorkerBootId worker_boot;
  CoordinatorEpoch coordinator_epoch;
  Sequence dispatch_sequence;
  EnsembleError failure{EnsembleError::ok};
  std::string failure_detail;
  std::uint32_t evaluation_count{0};
  std::uint32_t hard_failures{0};
  std::uint32_t hard_unknowns{0};
  std::uint32_t hard_predicates{0};
  std::uint32_t budget_units_used{0};
  std::uint32_t latency{0};
  bool fallback{false};
  bool current{true};
  std::string elimination_reason;

  [[nodiscard]] bool in_flight() const noexcept {
    return state == CandidateState::declared || state == CandidateState::dispatched;
  }
  [[nodiscard]] bool resolved() const noexcept { return !in_flight(); }
};

struct EvaluationRecord {
  EvaluationId id;
  EvaluationGeneration generation;
  ParticipantId evaluator;
  ParticipantGeneration evaluator_generation;
  ParticipantRole role{ParticipantRole::judge};
  CandidateId candidate;
  CandidateGeneration candidate_generation;
  CriterionRef criterion;
  CriterionKind kind{CriterionKind::ranking_signal};
  VerificationState verification{VerificationState::unknown};
  CategoricalJudgment judgment{CategoricalJudgment::unknown};
  bool score_defined{false};
  double score{0.0};
  double weight{1.0};
  std::vector<std::byte> evidence;
  std::string rationale;
  Sequence received_sequence;
  CoordinatorEpoch coordinator_epoch;
  WorkerId worker;
  WorkerBootId worker_boot;
  std::uint64_t fingerprint{0};
  bool current{true};
};

struct PendingEvaluation {
  EvaluationId id;
  EvaluationGeneration generation;
  ParticipantId evaluator;
  ParticipantGeneration evaluator_generation;
  ParticipantRole role{ParticipantRole::judge};
  CandidateId candidate;
  CandidateGeneration candidate_generation;
  CriterionRef criterion;
  CriterionKind kind{CriterionKind::ranking_signal};
  bool emitted{false};
  bool answered{false};
  bool abandoned{false};
};

struct ExecutionRecord {
  EnsembleExecutionId id;
  EnsembleAttemptGeneration attempt_generation{EnsembleAttemptGeneration(1)};
  ExecutionStatus status{ExecutionStatus::open};
  CoordinatorEpoch coordinator_epoch;
  std::vector<CandidateRecord> candidates;
  std::vector<EvaluationRecord> evaluations;
  std::vector<PendingEvaluation> pending;
  /// Stages synthesized by explicit fallback activation.
  std::vector<StageSpec> extra_stages;
  std::size_t stage_index{0};
  bool declared_current_stage{false};
  bool planning_complete{false};
  bool fallback_activated{false};
  std::uint32_t fallback_depth{0};
  Sequence cancelled_sequence;
  std::string cancellation_reason;
  bool cancellation_requested{false};
  bool cancellation_emitted{false};
  bool has_result{false};
  EnsembleResult result;
  std::vector<CommitRecord> commits;
  std::uint64_t commit_attempts{0};
  std::uint64_t report_generation{0};
  QuorumReport quorum;
  ConsensusReport consensus;
  ArbitrationReport arbitration;
  std::vector<ExplanationEntry> explanation;
  Sequence committed_sequence;
};

struct EnsembleRecord {
  EnsembleSpec spec;
  std::vector<ParticipantRecord> participants;
  bool has_execution{false};
  ExecutionRecord execution;
  bool superseded{false};
  std::vector<EnsembleResult> history;
};

/// Shared implementation of the runtime.  Defined in a private header so that
/// the public API stays free of implementation details while several
/// translation units implement disjoint parts of the same object.
struct EnsembleFabric::Impl {
  FabricConfig config;
  mutable std::shared_mutex mutex;

  CoordinatorEpoch epoch{CoordinatorEpoch(1)};
  WorkerBootId coordinator_boot;

  IdAllocator<EnsembleIdTag> ensemble_ids;
  IdAllocator<CandidateIdTag> candidate_ids;
  IdAllocator<EvaluationIdTag> evaluation_ids;
  IdAllocator<ResultIdTag> result_ids;
  IdAllocator<SequenceTag> sequences;

  EnsembleFabric::Counters counters;

  /// Ordered by identity so that listing and persistence are deterministic.
  std::map<std::uint64_t, EnsembleRecord> ensembles;

  [[nodiscard]] Sequence next_sequence() noexcept {
    const auto value = sequences.allocate();
    return value.has_value() ? *value : Sequence(1);
  }

  [[nodiscard]] EnsembleRecord* find(EnsembleId id) {
    const auto it = ensembles.find(id.value());
    return it == ensembles.end() ? nullptr : &it->second;
  }
  [[nodiscard]] const EnsembleRecord* find(EnsembleId id) const {
    const auto it = ensembles.find(id.value());
    return it == ensembles.end() ? nullptr : &it->second;
  }
  [[nodiscard]] static ParticipantRecord* find_participant(EnsembleRecord& ensemble,
                                                           ParticipantId id) {
    for (ParticipantRecord& record : ensemble.participants) {
      if (record.id == id) {
        return &record;
      }
    }
    return nullptr;
  }
  [[nodiscard]] static const ParticipantRecord* find_participant(const EnsembleRecord& ensemble,
                                                                 ParticipantId id) {
    for (const ParticipantRecord& record : ensemble.participants) {
      if (record.id == id) {
        return &record;
      }
    }
    return nullptr;
  }
};

[[nodiscard]] inline CandidateRecord* find_candidate(ExecutionRecord& execution, CandidateId id) {
  for (CandidateRecord& candidate : execution.candidates) {
    if (candidate.id == id) {
      return &candidate;
    }
  }
  return nullptr;
}

[[nodiscard]] inline const CandidateRecord* find_candidate(const ExecutionRecord& execution,
                                                           CandidateId id) {
  for (const CandidateRecord& candidate : execution.candidates) {
    if (candidate.id == id) {
      return &candidate;
    }
  }
  return nullptr;
}

[[nodiscard]] inline PendingEvaluation* find_pending(ExecutionRecord& execution, EvaluationId id) {
  for (PendingEvaluation& pending : execution.pending) {
    if (pending.id == id) {
      return &pending;
    }
  }
  return nullptr;
}

/// The stages an ensemble actually executes.  A specification without explicit
/// stages dispatches every candidate-producing participant in one implicit
/// parallel stage.
[[nodiscard]] std::vector<StageSpec> effective_stages(const EnsembleSpec& spec);

/// Participants that evaluate other participants' work.
[[nodiscard]] std::vector<const ParticipantSpec*> evaluating_participants(const EnsembleSpec& spec);

/// Criteria a judge applies for a given ensemble.
[[nodiscard]] std::vector<CriterionRef> judging_criteria(const EnsembleSpec& spec);

/// Value-copy snapshot builders shared by the result, inspection, and
/// persistence paths.
[[nodiscard]] CandidateSnapshot snapshot_candidate(const CandidateRecord& candidate);
[[nodiscard]] EvaluationSnapshot snapshot_evaluation(const EvaluationRecord& evaluation);

/// Recomputes quorum, consensus, and arbitration for the execution from
/// current evidence.  Pure with respect to everything except the execution's
/// own report fields, so it can be called from both advance() and finalize().
void compute_reports(EnsembleRecord& ensemble, ExecutionRecord& execution);

/// Builds the current evaluation summary of a candidate from its current
/// evaluations.  Only current, non-superseded evidence is counted.
[[nodiscard]] CandidateEvaluationSummary summarize_candidate(
    const ExecutionRecord& execution, const CandidateRecord& candidate);

/// True when every piece of work for the execution has resolved.
[[nodiscard]] bool execution_settled(const EnsembleRecord& ensemble);

/// Marks a candidate failed with a typed reason and updates participant
/// accounting.  Never mutates a candidate that already reached a content state.
void fail_candidate(EnsembleRecord& ensemble, ExecutionRecord& execution, CandidateRecord& candidate,
                    EnsembleError failure, std::string detail);

/// True when an explicit, typed fallback activation condition holds.
[[nodiscard]] bool fallback_trigger_fires(FallbackTrigger trigger, const EnsembleRecord& ensemble,
                                          const ExecutionRecord& execution);

/// True when the retry policy authorizes another attempt for this failure class.
[[nodiscard]] bool retry_authorized(const EnsembleSpec& spec, const CandidateRecord& candidate,
                                    EnsembleError failure) noexcept;

/// Rewinds a candidate to DECLARED for another attempt.  The candidate
/// generation advances so that late output from the previous attempt can never
/// be mistaken for current evidence, and prior evaluations are superseded.
void rewind_candidate_for_retry(EnsembleRecord& ensemble, ExecutionRecord& execution,
                                CandidateRecord& candidate, EnsembleId ensemble_id);

/// Supersedes every current evaluation of a candidate and drops its unanswered
/// evaluation plan.
void supersede_candidate_evidence(ExecutionRecord& execution, CandidateId candidate);

/// Releases in-flight candidates of one participant after a participant-level
/// failure, applying the retry policy exactly.
void handle_participant_failure_locked(EnsembleRecord& ensemble, ExecutionRecord& execution,
                                       ParticipantRecord& participant,
                                       ParticipantFailureKind kind, const std::string& detail);

}  // namespace ensemble_fabric
