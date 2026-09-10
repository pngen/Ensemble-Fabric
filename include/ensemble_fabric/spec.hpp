#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ensemble_fabric/ids.hpp"
#include "ensemble_fabric/limits.hpp"
#include "ensemble_fabric/outcome.hpp"
#include "ensemble_fabric/participant.hpp"
#include "ensemble_fabric/role.hpp"

namespace ensemble_fabric {

/// Sentinel meaning "no limit configured".  Written as an explicit constant so
/// that a zero value can keep its natural meaning of "none required".
inline constexpr std::uint32_t unbounded = 0xFFFFFFFFu;

/// How the members of one stage are driven.
enum class TopologyMode : std::uint8_t {
  parallel = 1,
  sequential = 2,
};

[[nodiscard]] std::string_view to_string(TopologyMode mode) noexcept;

/// One ordered stage of an ensemble plan.
struct StageSpec {
  StageId id;
  std::string label;
  TopologyMode mode{TopologyMode::parallel};
  std::vector<ParticipantId> participants;
  std::vector<std::string> required_domains;
  /// True for a stage the runtime synthesizes when an explicit, typed fallback
  /// condition authorizes fallback participation.
  bool fallback_stage{false};
};

/// Minimum number of participants holding a role inside the quorum denominator.
struct RoleQuota {
  ParticipantRole role{ParticipantRole::candidate};
  std::uint32_t min_count{0};
};

/// Which set forms the denominator of the quorum fraction.  This is stated
/// explicitly because "quorum" without a denominator is not a contract.
enum class VoteBasis : std::uint8_t {
  declared_participants = 1,
  authoritative_participants = 2,
  contributing_participants = 3,
  required_participants = 4,
  eligible_participants = 5,
};

[[nodiscard]] std::string_view to_string(VoteBasis basis) noexcept;

struct QuorumPolicy {
  std::uint32_t min_participants{0};
  std::vector<RoleQuota> role_quota;
  std::uint32_t min_evaluations{0};
  std::uint32_t min_valid_candidates{1};
  /// Percentage of the vote basis required to declare consensus.  Zero means
  /// "no consensus threshold configured"; it never means "any plurality wins".
  std::uint32_t consensus_threshold_percent{0};
  VoteBasis vote_basis{VoteBasis::authoritative_participants};
  bool count_abstentions{false};
  bool count_optional_participants{true};
  bool count_failed_in_denominator{false};
  bool count_unavailable_in_denominator{false};
  std::uint32_t max_failures{unbounded};
  std::uint32_t max_abstentions{unbounded};
};

enum class AggregationKind : std::uint8_t {
  best_of_n = 1,
  quorum_vote = 2,
  weighted_vote = 3,
  consensus_threshold = 4,
  judge_arbitration = 5,
  verifier_gated = 6,
};

[[nodiscard]] std::string_view to_string(AggregationKind kind) noexcept;

struct AggregationPolicy {
  AggregationKind kind{AggregationKind::best_of_n};
  bool use_weights{false};
  bool require_verification_pass{false};
  bool require_judge_acceptance{false};
  double min_aggregate_score{0.0};
  double min_judge_acceptance_ratio{0.0};
};

/// Ordered arbitration factors.  Hard eligibility is not a factor: it always
/// runs first and is not configurable.
enum class ArbitrationFactor : std::uint8_t {
  judge_acceptance_ratio = 1,
  verifier_state = 2,
  vote_count = 3,
  weighted_vote = 4,
  aggregate_score = 5,
  min_score = 6,
  confidence = 7,
  participant_priority = 8,
  specialist_relevance = 9,
  execution_cost = 10,
  latency = 11,
  provenance_quality = 12,
  fallback_status = 13,
};

[[nodiscard]] std::string_view to_string(ArbitrationFactor factor) noexcept;

/// What happens when two candidates are indistinguishable on every configured
/// factor.  A silent choice is never allowed.
enum class TieBreakPolicy : std::uint8_t {
  report_tie = 1,
  lowest_candidate_id = 2,
  highest_score_then_id = 3,
  prefer_required_participant = 4,
};

[[nodiscard]] std::string_view to_string(TieBreakPolicy policy) noexcept;

struct ArbitrationPolicy {
  std::vector<ArbitrationFactor> factors;
  TieBreakPolicy tie_break{TieBreakPolicy::lowest_candidate_id};
  bool require_unique_winner{false};
  std::uint32_t max_selected{1};
};

/// Typed conditions that authorize fallback activation.  Fallback is never
/// activated because a primary candidate merely ranked lower.
enum class FallbackTrigger : std::uint8_t {
  participant_unavailable = 1,
  retry_exhausted = 2,
  quorum_impossible = 3,
  required_role_unavailable = 4,
  all_primary_invalid = 5,
  judge_disagreement = 6,
  insufficient_evidence = 7,
  required_participant_failed = 8,
};

[[nodiscard]] std::string_view to_string(FallbackTrigger trigger) noexcept;

struct FallbackPolicy {
  bool enabled{false};
  std::vector<FallbackTrigger> triggers;
  std::vector<ParticipantId> fallback_participants;
  bool eager_speculative{false};
  std::uint32_t max_depth{1};
};

struct CompletionPolicy {
  bool require_consensus{false};
  bool require_all_required_participants{false};
  bool allow_partial_results{true};
};

struct RetryPolicy {
  /// Total attempts allowed for one logical candidate slot, including the first.
  std::uint32_t max_attempts{1};
  bool retry_on_unavailable{true};
  bool retry_on_transport_error{false};
  bool retry_on_malformed_candidate{false};
  bool retry_on_participant_failed{false};
  /// When true a retry may be served by a replacement physical participant; the
  /// logical participant generation advances and prior votes are not inherited.
  bool replace_participant{false};
  /// When true, evaluations of a superseded attempt are invalidated.
  bool rerun_evaluations{true};
};

struct CancellationPolicy {
  bool cancel_on_required_failure{false};
  bool cancel_on_quorum_impossible{false};
  bool preserve_committed_results{true};
};

/// Evidence the ensemble requires before a candidate may be considered complete.
struct EvidenceRequirement {
  std::uint32_t min_candidate_payload_bytes{0};
  std::uint32_t min_evaluations_per_candidate{0};
  bool require_rationale{false};
  /// Hard predicates that must be satisfied.  A candidate that fails one of
  /// these is ineligible no matter how favorable its ranking signals are.
  std::vector<CriterionRef> mandatory_criteria;
  bool unknown_verification_is_failure{true};
  bool abstention_is_failure{false};
};

/// The first-class, generation-bound ensemble specification.  It is immutable
/// once defined: reconfiguration creates a new generation and fences the old one.
struct EnsembleSpec {
  EnsembleId id;
  EnsembleGeneration generation;
  std::string label;
  std::string task_class;
  std::uint32_t task_class_id{0};
  std::vector<ParticipantSpec> participants;
  std::vector<StageSpec> stages;
  QuorumPolicy quorum;
  AggregationPolicy aggregation;
  ArbitrationPolicy arbitration;
  FallbackPolicy fallback;
  CompletionPolicy completion;
  RetryPolicy retry;
  CancellationPolicy cancellation;
  EvidenceRequirement evidence;
  /// Criteria that judges apply when ranking candidates.  An empty list means a
  /// single default ranking criterion (id 1, version 1).
  std::vector<CriterionRef> judging_criteria;
  CompatibilityRequirement requirements;
  std::uint32_t overall_budget_units{0};
  std::uint64_t deterministic_seed{0};
  /// Canonical digest over the policy-relevant content.  Two specs with the same
  /// digest are behaviourally identical.
  std::uint64_t fingerprint{0};
};

/// Returns the participant declaration for an identity, or nullptr.
[[nodiscard]] const ParticipantSpec* find_participant(const EnsembleSpec& spec,
                                                      ParticipantId id) noexcept;

/// Roles actually declared anywhere in the specification.
[[nodiscard]] RoleSet declared_roles(const EnsembleSpec& spec) noexcept;

/// Validates structure, per-limit bounds, and policy coherence.  Called before a
/// specification becomes a generation so that impossible policies are rejected
/// at definition time rather than during arbitration.
[[nodiscard]] Status validate_spec(const EnsembleSpec& spec, const Limits& limits);

/// Canonical, deterministic digest of the specification.
[[nodiscard]] std::uint64_t compute_spec_fingerprint(const EnsembleSpec& spec) noexcept;

/// Deterministic rendering used by inspection tools and tests.
[[nodiscard]] std::string render_spec(const EnsembleSpec& spec);

}  // namespace ensemble_fabric
