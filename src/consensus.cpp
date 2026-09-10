#include "ensemble_fabric/consensus.hpp"

#include <algorithm>

namespace ensemble_fabric {

std::string_view to_string(ConsensusState state) noexcept {
  switch (state) {
    case ConsensusState::not_evaluated: return "NOT_EVALUATED";
    case ConsensusState::consensus: return "CONSENSUS";
    case ConsensusState::consensus_with_abstentions: return "CONSENSUS_WITH_ABSTENTIONS";
    case ConsensusState::plurality_without_threshold: return "PLURALITY_WITHOUT_THRESHOLD";
    case ConsensusState::quorum_not_reached: return "QUORUM_NOT_REACHED";
    case ConsensusState::quorum_impossible: return "QUORUM_IMPOSSIBLE";
    case ConsensusState::disagreement: return "DISAGREEMENT";
    case ConsensusState::tie: return "TIE";
    case ConsensusState::insufficient_evidence: return "INSUFFICIENT_EVIDENCE";
    case ConsensusState::required_participant_failed: return "REQUIRED_PARTICIPANT_FAILED";
    case ConsensusState::all_candidates_invalid: return "ALL_CANDIDATES_INVALID";
    case ConsensusState::no_eligible_candidate: return "NO_ELIGIBLE_CANDIDATE";
    case ConsensusState::fallback_required: return "FALLBACK_REQUIRED";
    case ConsensusState::cancelled: return "CANCELLED";
    case ConsensusState::superseded: return "SUPERSEDED";
    case ConsensusState::revalidation_required: return "REVALIDATION_REQUIRED";
    case ConsensusState::committed: return "COMMITTED";
  }
  return "UNKNOWN_CONSENSUS";
}

namespace {

[[nodiscard]] bool better_tally(const CandidateTally& lhs, const CandidateTally& rhs) noexcept {
  if (lhs.agreeing != rhs.agreeing) {
    return lhs.agreeing > rhs.agreeing;
  }
  if (lhs.weighted_agree != rhs.weighted_agree) {
    return lhs.weighted_agree > rhs.weighted_agree;
  }
  return lhs.candidate.value() < rhs.candidate.value();
}

}  // namespace

ConsensusReport compute_consensus(const ConsensusInputs& inputs) {
  ConsensusReport report;
  report.ensemble_generation = inputs.ensemble_generation;
  report.coordinator_epoch = inputs.coordinator_epoch;
  report.computed_sequence = inputs.sequence;
  report.candidates_declared = inputs.candidates_declared;
  report.invalid_candidates = inputs.invalid_candidates;
  report.threshold_percent = inputs.quorum.threshold_percent;
  report.threshold_count = inputs.quorum.threshold_count;
  report.vote_basis = inputs.quorum.denominator;
  report.abstaining = inputs.quorum.abstentions;
  report.abstaining_members = inputs.quorum.abstaining_members;
  report.missing = inputs.quorum.denominator > inputs.quorum.contributors + inputs.quorum.abstentions
                       ? inputs.quorum.denominator - inputs.quorum.contributors -
                             inputs.quorum.abstentions
                       : 0u;

  std::vector<CandidateTally> eligible;
  for (const CandidateTally& tally : inputs.tallies) {
    if (tally.eligible) {
      eligible.push_back(tally);
      report.eligible_candidates_list.push_back(tally.candidate);
    }
  }
  report.eligible_candidates = static_cast<std::uint32_t>(eligible.size());
  std::sort(eligible.begin(), eligible.end(), better_tally);

  // Terminal conditions win over any vote arithmetic: a cancelled or
  // superseded execution cannot later report success.
  if (inputs.superseded) {
    report.state = ConsensusState::superseded;
    return report;
  }
  if (inputs.revalidation_required) {
    report.state = ConsensusState::revalidation_required;
    return report;
  }
  if (inputs.cancelled) {
    report.state = ConsensusState::cancelled;
    return report;
  }
  if (inputs.required_participant_failed && inputs.require_all_required) {
    report.state = ConsensusState::required_participant_failed;
    return report;
  }
  if (inputs.quorum.state == QuorumState::impossible) {
    report.state = ConsensusState::quorum_impossible;
    return report;
  }
  if (eligible.empty()) {
    if (inputs.candidates_declared > 0u && inputs.invalid_candidates >= inputs.candidates_declared) {
      report.state = ConsensusState::all_candidates_invalid;
    } else {
      report.state = ConsensusState::no_eligible_candidate;
    }
    return report;
  }
  if (inputs.fallback_required) {
    report.state = ConsensusState::fallback_required;
    return report;
  }
  if (inputs.quorum.state != QuorumState::reached) {
    report.state = ConsensusState::quorum_not_reached;
    return report;
  }

  const CandidateTally& leader = eligible.front();
  report.leading_candidate = leader.candidate;
  report.leading_generation = leader.generation;
  report.leading_participant = leader.participant;
  report.agreeing = leader.agreeing;
  report.agreeing_members = leader.agreeing_members;
  report.eligible_candidates = static_cast<std::uint32_t>(eligible.size());

  std::uint32_t runner_up = 0;
  std::vector<ParticipantId> disagreeing;
  for (std::size_t index = 1; index < eligible.size(); ++index) {
    if (eligible[index].agreeing > runner_up) {
      runner_up = eligible[index].agreeing;
    }
    for (const ParticipantId id : eligible[index].agreeing_members) {
      disagreeing.push_back(id);
    }
  }
  for (const ParticipantId id : leader.disagreeing_members) {
    disagreeing.push_back(id);
  }
  report.runner_up_votes = runner_up;
  report.disagreeing = static_cast<std::uint32_t>(disagreeing.size());
  report.disagreeing_members = disagreeing;

  if (leader.agreeing == 0u) {
    if (!inputs.requires_votes) {
      // No evaluations were ever required: eligibility decides, and arbitration
      // ranks the eligible candidates deterministically.
      report.state = eligible.size() == 1u ? ConsensusState::consensus
                                           : ConsensusState::plurality_without_threshold;
      return report;
    }
    report.state = ConsensusState::insufficient_evidence;
    return report;
  }
  if (report.threshold_percent > 0u) {
    if (leader.agreeing < report.threshold_count) {
      if (runner_up == leader.agreeing) {
        report.state = ConsensusState::tie;
        report.unresolved_disagreement = true;
      } else {
        report.state = ConsensusState::disagreement;
        report.unresolved_disagreement = true;
      }
      return report;
    }
  } else if (runner_up == leader.agreeing && runner_up > 0u) {
    // Without an explicit threshold an exact tie is still a tie.  It is never
    // silently converted into a win for the lowest identity.
    report.state = ConsensusState::tie;
    report.unresolved_disagreement = true;
    return report;
  }

  if (report.abstaining > 0u) {
    report.state = ConsensusState::consensus_with_abstentions;
  } else if (report.threshold_percent == 0u) {
    report.state = ConsensusState::plurality_without_threshold;
  } else {
    report.state = ConsensusState::consensus;
  }
  return report;
}

std::string ConsensusReport::render() const {
  std::string out = "consensus ";
  out.append(ensemble_fabric::to_string(state));
  out.append(" leading=").append(leading_candidate.to_string());
  out.append(" agreeing=").append(std::to_string(agreeing));
  out.append(" disagreeing=").append(std::to_string(disagreeing));
  out.append(" abstaining=").append(std::to_string(abstaining));
  out.append(" missing=").append(std::to_string(missing));
  out.append(" eligible=").append(std::to_string(eligible_candidates));
  out.append(" invalid=").append(std::to_string(invalid_candidates));
  out.append(" basis=").append(std::to_string(vote_basis));
  out.append(" threshold=").append(std::to_string(threshold_count));
  if (unresolved_disagreement) {
    out.append(" unresolved_disagreement=true");
  }
  return out;
}

}  // namespace ensemble_fabric
