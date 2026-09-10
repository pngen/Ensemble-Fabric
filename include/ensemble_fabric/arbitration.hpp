#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ensemble_fabric/candidate.hpp"
#include "ensemble_fabric/evaluation.hpp"
#include "ensemble_fabric/ids.hpp"
#include "ensemble_fabric/participant.hpp"
#include "ensemble_fabric/spec.hpp"

namespace ensemble_fabric {

/// Why a candidate left the arbitration.  Every elimination is explained with a
/// typed reason and a human-readable detail string.
enum class EliminationReason : std::uint8_t {
  none = 0,
  not_a_content_state = 1,
  invalid_candidate = 2,
  failed_attempt = 3,
  abstained = 4,
  stale_generation = 5,
  superseded = 6,
  cancelled = 7,
  participant_not_authoritative = 8,
  incompatible_participant = 9,
  verifier_failed = 10,
  hard_predicate_failed = 11,
  missing_required_evidence = 12,
  below_minimum_score = 13,
  insufficient_judge_acceptance = 14,
  not_selectable_role = 15,
  lost_ranking = 16,
  tie_lost = 17,
  fallback_not_selected = 18,
};

[[nodiscard]] std::string_view to_string(EliminationReason reason) noexcept;

/// One measured arbitration factor for one candidate.
struct RankingFactorValue {
  ArbitrationFactor factor{ArbitrationFactor::aggregate_score};
  double value{0.0};
  std::string detail;
};

/// The complete ranking record of one candidate.
struct CandidateRanking {
  CandidateId candidate;
  CandidateGeneration generation;
  ParticipantId participant;
  ParticipantGeneration participant_generation;
  ParticipantRole role{ParticipantRole::candidate};
  bool eligible{false};
  EliminationReason elimination{EliminationReason::none};
  std::string elimination_detail;
  std::vector<RankingFactorValue> factors;
  std::int64_t rank{-1};
  bool selected{false};
};

/// Deterministic arbitration outcome.
struct ArbitrationReport {
  EnsembleGeneration ensemble_generation;
  CoordinatorEpoch coordinator_epoch;
  Sequence computed_sequence;
  TieBreakPolicy tie_break{TieBreakPolicy::lowest_candidate_id};
  std::vector<ArbitrationFactor> factor_order;
  std::vector<CandidateRanking> ranking;
  CandidateId selected;
  CandidateGeneration selected_generation;
  ParticipantId selected_participant;
  ParticipantRole selected_role{ParticipantRole::candidate};
  bool tie_unresolved{false};
  bool no_eligible_candidate{false};
  std::uint32_t considered{0};
  std::uint32_t eliminated{0};
  std::vector<std::string> notes;

  [[nodiscard]] std::string render() const;
};

/// Renders the ordered factor list into a stable string (used by the
/// deterministic explanation and by the specification digest).
[[nodiscard]] std::string render_factors(const std::vector<ArbitrationFactor>& factors);

/// One candidate as arbitration sees it.  The fabric fills in the current
/// facts; arbitration itself is a pure function of these inputs.
struct ArbitrationCandidateInput {
  CandidateId candidate;
  CandidateGeneration generation;
  ParticipantId participant;
  ParticipantGeneration participant_generation;
  ParticipantRole role{ParticipantRole::candidate};
  CandidateState state{CandidateState::declared};
  bool generation_current{true};
  bool participant_authoritative{true};
  bool compatible{true};
  bool required_participant{true};
  bool fallback{false};
  bool role_selectable{true};
  std::uint32_t participant_priority{0};
  std::uint32_t execution_cost{0};
  std::uint32_t latency{0};
  std::uint32_t specialist_matches{0};
  std::uint32_t payload_bytes{0};
  CandidateEvaluationSummary evaluation;
};

struct ArbitrationInputs {
  std::vector<ArbitrationCandidateInput> candidates;
  ArbitrationPolicy policy;
  AggregationPolicy aggregation;
  EvidenceRequirement evidence;
  bool quorum_satisfied{false};
  EnsembleGeneration ensemble_generation;
  CoordinatorEpoch coordinator_epoch;
  Sequence sequence;
};

/// Deterministic arbitration: hard eligibility first, then the configured
/// ranking factors in order, then the configured tie-break policy.  Identical
/// authoritative inputs always produce an identical report.
[[nodiscard]] ArbitrationReport arbitrate(const ArbitrationInputs& inputs);

}  // namespace ensemble_fabric

