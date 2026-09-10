#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <variant>
#include <vector>

#include "ensemble_fabric/arbitration.hpp"
#include "ensemble_fabric/authority.hpp"
#include "ensemble_fabric/candidate.hpp"
#include "ensemble_fabric/consensus.hpp"
#include "ensemble_fabric/explanation.hpp"
#include "ensemble_fabric/ids.hpp"
#include "ensemble_fabric/limits.hpp"
#include "ensemble_fabric/outcome.hpp"
#include "ensemble_fabric/participant.hpp"
#include "ensemble_fabric/quorum.hpp"
#include "ensemble_fabric/result.hpp"
#include "ensemble_fabric/spec.hpp"

namespace ensemble_fabric {

/// Configuration of one runtime instance.
struct FabricConfig {
  Limits limits{};
  CoordinatorEpoch initial_epoch{CoordinatorEpoch(1)};
  WorkerBootId coordinator_boot;
  bool require_authoritative_participants{true};
  bool deterministic_ids{true};
};

/// Status of the current execution generation of an ensemble.
enum class ExecutionStatus : std::uint8_t {
  none = 0,
  open = 1,
  committed = 2,
  cancelled = 3,
  superseded = 4,
  revalidation_required = 5,
  failed = 6,
  no_eligible_candidate = 7,
};

[[nodiscard]] std::string_view to_string(ExecutionStatus status) noexcept;

/// Work the runtime needs performed.  The core never performs I/O itself: it
/// returns actions, and the caller (an in-process test driver or the
/// distributed coordinator) transports them.  This keeps network waits out of
/// every internal lock.
struct DispatchAction {
  ExecutionRef execution;
  CandidateId candidate;
  CandidateGeneration candidate_generation;
  ParticipantId participant;
  ParticipantGeneration participant_generation;
  ParticipantRole role{ParticipantRole::candidate};
  StageId stage;
  std::uint32_t attempt{1};
  std::uint32_t budget_units{0};
  std::uint64_t deterministic_seed{0};
  std::string domain;
  std::vector<std::byte> request;
  Sequence sequence;
};

struct EvaluationAction {
  ExecutionRef execution;
  EvaluationId evaluation;
  EvaluationGeneration evaluation_generation;
  ParticipantId judge;
  ParticipantGeneration judge_generation;
  ParticipantRole role{ParticipantRole::judge};
  CandidateId candidate;
  CandidateGeneration candidate_generation;
  CriterionRef criterion;
  CriterionKind criterion_kind{CriterionKind::ranking_signal};
  std::string domain;
  Sequence sequence;
};

struct CancelAction {
  ExecutionRef execution;
  ParticipantId participant;
  ParticipantGeneration participant_generation;
  CandidateId candidate;
  CandidateGeneration candidate_generation;
  std::string reason;
  Sequence sequence;
};

struct FallbackAction {
  ExecutionRef execution;
  ParticipantId participant;
  ParticipantGeneration participant_generation;
  FallbackTrigger trigger{FallbackTrigger::participant_unavailable};
  std::uint32_t depth{1};
  Sequence sequence;
};

using FabricAction = std::variant<DispatchAction, EvaluationAction, CancelAction, FallbackAction>;

/// Compact listing row used by inspection tools.
struct EnsembleSummary {
  EnsembleId id;
  EnsembleGeneration generation;
  std::string label;
  ExecutionStatus execution_status{ExecutionStatus::none};
  CoordinatorEpoch coordinator_epoch;
  std::uint32_t participants{0};
  std::uint32_t candidates{0};
  std::uint32_t evaluations{0};
  bool has_result{false};
};

/// Full inspection view of one ensemble.  Every field is a value copy: callers
/// never hold a reference into mutable runtime state.
struct EnsembleSnapshot {
  EnsembleId id;
  EnsembleGeneration generation;
  std::string label;
  std::string task_class;
  std::uint64_t spec_fingerprint{0};
  CoordinatorEpoch coordinator_epoch;
  EnsembleExecutionId execution;
  EnsembleAttemptGeneration attempt_generation;
  ExecutionStatus execution_status{ExecutionStatus::none};
  bool cancelled{false};
  bool superseded{false};
  bool revalidation_required{false};
  bool fallback_activated{false};
  std::uint32_t fallback_depth{0};
  std::vector<ParticipantSnapshot> participants;
  std::vector<CandidateSnapshot> candidates;
  std::vector<EvaluationSnapshot> evaluations;
  QuorumReport quorum;
  ConsensusReport consensus;
  ArbitrationReport arbitration;
  std::vector<CommitRecord> commits;
  bool has_result{false};
  EnsembleResult result;
  std::vector<ExplanationEntry> explanation;
  EnsembleSpec spec;

  [[nodiscard]] std::string render() const;
};

/// The ensemble runtime.
///
/// Thread safety: every public method is safe to call concurrently.  Internal
/// state is guarded by a single shared mutex; no callback, network operation, or
/// file write ever runs while that mutex is held.  Actions returned by
/// advance() are produced from a state snapshot and transported by the caller
/// after the lock is released.  Snapshots returned to callers own their data.
class EnsembleFabric {
 public:
  explicit EnsembleFabric(FabricConfig config = FabricConfig{});
  ~EnsembleFabric();

  EnsembleFabric(const EnsembleFabric&) = delete;
  EnsembleFabric& operator=(const EnsembleFabric&) = delete;

  [[nodiscard]] CoordinatorEpoch epoch() const;
  [[nodiscard]] WorkerBootId coordinator_boot() const;
  [[nodiscard]] Limits limits() const;
  [[nodiscard]] const FabricConfig& config() const noexcept;

  // -- Definition -----------------------------------------------------------
  /// Defines a new ensemble or redefines an existing one, advancing its
  /// generation.  Definition validates structure, bounds, and policy coherence.
  [[nodiscard]] Outcome<EnsembleGeneration> define_ensemble(EnsembleSpec spec);

  /// Marks the current generation superseded.  A superseded generation can never
  /// commit a result afterwards.
  [[nodiscard]] Status supersede_ensemble(EnsembleId id, EnsembleGeneration generation);

  [[nodiscard]] Outcome<EnsembleSpec> spec(EnsembleId id) const;

  // -- Execution ------------------------------------------------------------
  /// Opens a fresh execution generation for the ensemble's current generation.
  [[nodiscard]] Outcome<EnsembleExecutionId> open_execution(EnsembleId id,
                                                            EnsembleGeneration generation);

  /// Cancels the current execution.  Idempotent for an already-cancelled
  /// execution; rejected once a result has been committed unless the policy
  /// permits post-commit cancellation (in which case the result stays
  /// authoritative and is marked historical).
  [[nodiscard]] Status cancel_ensemble(EnsembleId id, EnsembleGeneration generation,
                                       std::string reason);

  // -- Participants ---------------------------------------------------------
  [[nodiscard]] Outcome<ParticipantGeneration> register_participant(
      const ParticipantRegistration& registration);

  [[nodiscard]] Status report_participant_failure(const ParticipantAuthority& authority,
                                                  ParticipantFailureKind kind,
                                                  std::string detail);

  /// Marks every participant as requiring revalidation.  Used on coordinator
  /// restart: recovered dynamic evidence never becomes silently current.
  [[nodiscard]] Status invalidate_dynamic_authority(std::string reason);

  // -- Execution progression ------------------------------------------------
  /// Computes the next set of actions for the current execution.  Idempotent:
  /// calling it twice without changing state returns an empty action list.
  [[nodiscard]] Outcome<std::vector<FabricAction>> advance(EnsembleId id,
                                                           EnsembleGeneration generation);

  /// True when no candidate or evaluation is still in flight and no further
  /// action can be produced for the current execution generation.  A settled
  /// execution is the only one that may be arbitrated and committed.
  [[nodiscard]] Outcome<bool> is_settled(EnsembleId id, EnsembleGeneration generation) const;

  // -- Ingest ---------------------------------------------------------------
  [[nodiscard]] Status submit_candidate(const CandidateSubmission& submission);
  [[nodiscard]] Status submit_candidate_failure(const CandidateFailureSubmission& submission);
  [[nodiscard]] Status submit_candidate_abstention(
      const CandidateAbstentionSubmission& submission);
  [[nodiscard]] Outcome<EvaluationId> submit_evaluation(const EvaluationSubmission& submission);

  // -- Commit ---------------------------------------------------------------
  /// Computes arbitration and commits exactly one authoritative logical result
  /// for the current execution generation.  Idempotent for a repeated identical
  /// commit; conflicting commits and stale-generation commits are rejected.
  [[nodiscard]] Outcome<EnsembleResult> finalize(EnsembleId id, EnsembleGeneration generation);

  /// Computes the result the current evidence supports without committing it.
  /// Two-phase preparation is what makes commit-order races deterministic.
  [[nodiscard]] Outcome<EnsembleResult> prepare_result(EnsembleId id,
                                                       EnsembleGeneration generation);

  /// The commit gate itself.  Exposed so that tests and the distributed
  /// coordinator can exercise concurrent commit attempts directly.
  [[nodiscard]] Outcome<EnsembleResult> commit_result(const EnsembleResult& candidate);

  [[nodiscard]] Outcome<EnsembleResult> current_result(EnsembleId id) const;

  // -- Inspection -----------------------------------------------------------
  [[nodiscard]] Outcome<EnsembleSnapshot> inspect(EnsembleId id) const;
  [[nodiscard]] std::vector<EnsembleSummary> list_ensembles() const;

  // -- Persistence ----------------------------------------------------------
  /// Serializes durable state.  Live process authority, sockets, and worker
  /// readiness are deliberately not durable.
  [[nodiscard]] Outcome<std::vector<std::byte>> serialize_state() const;

  /// Atomically writes durable state to a file.
  [[nodiscard]] Status save_state(const std::string& path) const;

  /// Recovers durable state from a file, advances the coordinator epoch, and
  /// marks recovered dynamic authority as requiring revalidation.  Returns the
  /// new epoch.
  [[nodiscard]] Outcome<CoordinatorEpoch> recover_state(const std::string& path);

  /// Applies recovery semantics to already-loaded in-memory state without a
  /// file.  Used by the restart test to prove epoch advance in isolation.
  [[nodiscard]] Outcome<CoordinatorEpoch> restart(std::string reason);

  /// Test/diagnostic counters.
  struct Counters {
    std::uint64_t commits_accepted{0};
    std::uint64_t commits_rejected_conflicting{0};
    std::uint64_t commits_rejected_stale{0};
    std::uint64_t stale_submissions_rejected{0};
    std::uint64_t duplicate_submissions_suppressed{0};
    std::uint64_t duplicate_votes_suppressed{0};
    std::uint64_t fallback_activations{0};
    std::uint64_t retries_issued{0};
    std::uint64_t cancellations{0};
    std::uint64_t supersessions{0};
    std::uint64_t revalidations_required{0};
  };
  [[nodiscard]] Counters counters() const;

  /// Opaque implementation state.  Declared here, defined only inside the
  /// library, so the public header exposes no implementation detail while
  /// several translation units can implement disjoint parts of the runtime.
  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

/// Validates an ensemble result before it is admitted as authoritative.  A
/// result that names a stale generation, a non-current epoch, or an ineligible
/// candidate is rejected.
[[nodiscard]] Status validate_result(const EnsembleResult& result, const EnsembleSpec& spec,
                                     CoordinatorEpoch current_epoch);

}  // namespace ensemble_fabric
