#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ensemble_fabric/ids.hpp"
#include "ensemble_fabric/quorum.hpp"

namespace ensemble_fabric {

/// Typed ensemble outcome.  Disagreement, ties, and missing evidence are
/// first-class results and are never coerced into success.
enum class ConsensusState : std::uint8_t {
  not_evaluated = 0,
  consensus = 1,
  consensus_with_abstentions = 2,
  plurality_without_threshold = 3,
  quorum_not_reached = 4,
  quorum_impossible = 5,
  disagreement = 6,
  tie = 7,
  insufficient_evidence = 8,
  required_participant_failed = 9,
  all_candidates_invalid = 10,
  no_eligible_candidate = 11,
  fallback_required = 12,
  cancelled = 13,
  superseded = 14,
  revalidation_required = 15,
  committed = 16,
};

[[nodiscard]] std::string_view to_string(ConsensusState state) noexcept;

/// Full consensus/disagreement picture.  Every field is derived from evidence
/// that was current at computation time.
struct ConsensusReport {
  ConsensusState state{ConsensusState::not_evaluated};
  CandidateId leading_candidate;
  CandidateGeneration leading_generation;
  ParticipantId leading_participant;
  std::uint32_t agreeing{0};
  std::uint32_t disagreeing{0};
  std::uint32_t abstaining{0};
  std::uint32_t missing{0};
  std::uint32_t invalid_candidates{0};
  std::uint32_t eligible_candidates{0};
  std::uint32_t candidates_declared{0};
  std::uint32_t threshold_percent{0};
  std::uint32_t threshold_count{0};
  std::uint32_t vote_basis{0};
  std::uint32_t runner_up_votes{0};
  bool unresolved_disagreement{false};
  EnsembleGeneration ensemble_generation;
  CoordinatorEpoch coordinator_epoch;
  Sequence computed_sequence;
  std::vector<ParticipantId> agreeing_members;
  std::vector<ParticipantId> disagreeing_members;
  std::vector<ParticipantId> abstaining_members;
  std::vector<ParticipantId> missing_members;
  std::vector<CandidateId> eligible_candidates_list;

  [[nodiscard]] bool resolved() const noexcept {
    return state == ConsensusState::consensus ||
           state == ConsensusState::consensus_with_abstentions ||
           state == ConsensusState::plurality_without_threshold;
  }
  [[nodiscard]] std::string render() const;
};

/// Per-candidate vote tally derived from evaluations that were current when the
/// consensus was computed.
struct CandidateTally {
  CandidateId candidate;
  CandidateGeneration generation;
  ParticipantId participant;
  bool eligible{false};
  std::uint32_t agreeing{0};
  std::uint32_t disagreeing{0};
  std::uint32_t abstaining{0};
  double weighted_agree{0.0};
  std::vector<ParticipantId> agreeing_members;
  std::vector<ParticipantId> disagreeing_members;
  std::vector<ParticipantId> abstaining_members;
};

/// Everything the consensus computation is allowed to look at.  Passing the
/// inputs explicitly keeps the decision a pure function of current evidence.
struct ConsensusInputs {
  QuorumReport quorum;
  std::vector<CandidateTally> tallies;
  std::uint32_t candidates_declared{0};
  std::uint32_t invalid_candidates{0};
  bool required_participant_failed{false};
  bool require_all_required{false};
  bool cancelled{false};
  bool superseded{false};
  bool revalidation_required{false};
  bool fallback_required{false};
  /// False for a best-of-N ensemble that declares no evaluating participants.
  /// Such an ensemble decides by eligibility and deterministic arbitration, and
  /// must not be reported as lacking evidence it never asked for.
  bool requires_votes{true};
  EnsembleGeneration ensemble_generation;
  CoordinatorEpoch coordinator_epoch;
  Sequence sequence;
};

[[nodiscard]] ConsensusReport compute_consensus(const ConsensusInputs& inputs);

}  // namespace ensemble_fabric
