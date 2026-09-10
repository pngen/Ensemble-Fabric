#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ensemble_fabric/ids.hpp"
#include "ensemble_fabric/role.hpp"
#include "ensemble_fabric/spec.hpp"

namespace ensemble_fabric {

/// Typed quorum state.  "Not reached" and "impossible" are different facts: the
/// first may still be satisfied later in the generation, the second cannot.
enum class QuorumState : std::uint8_t {
  not_evaluated = 0,
  reached = 1,
  not_reached = 2,
  impossible = 3,
  lost = 4,
};

[[nodiscard]] std::string_view to_string(QuorumState state) noexcept;

/// The complete, auditable quorum computation.  Membership is generation-bound:
/// every entry names the participant generation that holds the seat, so a
/// replacement participant can never inherit its predecessor's vote.
struct QuorumReport {
  QuorumState state{QuorumState::not_evaluated};
  VoteBasis basis{VoteBasis::authoritative_participants};
  std::uint32_t denominator{0};
  std::uint32_t contributors{0};
  std::uint32_t abstentions{0};
  std::uint32_t failures{0};
  std::uint32_t excluded_optional{0};
  std::uint32_t excluded_unavailable{0};
  std::uint32_t excluded_stale{0};
  std::uint32_t required_total{0};
  std::uint32_t required_present{0};
  std::uint32_t evaluations{0};
  std::uint32_t valid_candidates{0};
  std::uint32_t duplicate_votes_suppressed{0};
  std::uint32_t stale_votes_rejected{0};
  std::uint32_t threshold_percent{0};
  std::uint32_t threshold_count{0};
  std::uint32_t leading_votes{0};
  std::uint32_t role_quota_met{0};
  std::uint32_t role_quota_required{0};
  bool failure_budget_exceeded{false};
  bool abstention_budget_exceeded{false};
  EnsembleGeneration ensemble_generation;
  CoordinatorEpoch coordinator_epoch;
  Sequence computed_sequence;
  std::vector<ParticipantId> members;
  std::vector<ParticipantGeneration> member_generations;
  std::vector<ParticipantId> contributors_list;
  std::vector<ParticipantId> abstaining_members;
  std::vector<ParticipantId> failed_members;
  std::vector<std::string> notes;

  [[nodiscard]] bool satisfied() const noexcept { return state == QuorumState::reached; }
  [[nodiscard]] std::string render() const;
};

/// A vote cast by one logical participant generation for one candidate.
/// Duplicate submissions from the same (participant, generation) pair collapse
/// into a single vote so that vote inflation is structurally impossible.
struct EligibleVote {
  ParticipantId participant;
  ParticipantGeneration generation;
  ParticipantRole role{ParticipantRole::judge};
  CandidateId candidate;
  CandidateGeneration candidate_generation;
  double weight{1.0};
  bool abstained{false};
};

/// Computes the quorum report from already-authorized membership and votes.
///
/// p membership lists every logical participant that holds a quorum seat in
/// the current generation, together with its generation and whether it is
/// required, optional, currently authorized, and failed.
struct QuorumMember {
  ParticipantId participant;
  ParticipantGeneration generation;
  RoleSet roles;
  bool required{true};
  bool authorized{true};
  bool available{true};
  bool failed{false};
  bool fallback{false};
  bool role_available{true};
};

[[nodiscard]] QuorumReport compute_quorum(const QuorumPolicy& policy,
                                          const std::vector<QuorumMember>& membership,
                                          const std::vector<EligibleVote>& votes,
                                          std::uint32_t valid_candidates,
                                          EnsembleGeneration ensemble_generation,
                                          CoordinatorEpoch epoch,
                                          Sequence computed_sequence);

}  // namespace ensemble_fabric
