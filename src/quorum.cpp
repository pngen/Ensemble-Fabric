#include "ensemble_fabric/quorum.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

#include "detail/text.hpp"

namespace ensemble_fabric {

std::string_view to_string(QuorumState state) noexcept {
  switch (state) {
    case QuorumState::not_evaluated: return "NOT_EVALUATED";
    case QuorumState::reached: return "REACHED";
    case QuorumState::not_reached: return "NOT_REACHED";
    case QuorumState::impossible: return "IMPOSSIBLE";
    case QuorumState::lost: return "LOST";
  }
  return "UNKNOWN_QUORUM";
}

namespace {

[[nodiscard]] std::uint32_t percentage_ceiling(std::uint32_t total, std::uint32_t percent) noexcept {
  if (percent == 0u) {
    return 0u;
  }
  const std::uint64_t scaled = static_cast<std::uint64_t>(total) * percent;
  return static_cast<std::uint32_t>((scaled + 99ull) / 100ull);
}

/// Deduplicated vote key: one logical participant generation may cast at most
/// one vote per candidate.  This is what makes vote inflation structurally
/// impossible rather than merely policed.
struct VoteKey {
  std::uint64_t participant;
  std::uint64_t generation;
  std::uint64_t candidate;

  friend bool operator<(const VoteKey& lhs, const VoteKey& rhs) noexcept {
    if (lhs.participant != rhs.participant) return lhs.participant < rhs.participant;
    if (lhs.generation != rhs.generation) return lhs.generation < rhs.generation;
    return lhs.candidate < rhs.candidate;
  }
};

}  // namespace

QuorumReport compute_quorum(const QuorumPolicy& policy,
                            const std::vector<QuorumMember>& membership,
                            const std::vector<EligibleVote>& votes,
                            std::uint32_t valid_candidates,
                            EnsembleGeneration ensemble_generation,
                            CoordinatorEpoch epoch,
                            Sequence computed_sequence) {
  QuorumReport report;
  report.basis = policy.vote_basis;
  report.ensemble_generation = ensemble_generation;
  report.coordinator_epoch = epoch;
  report.computed_sequence = computed_sequence;
  report.threshold_percent = policy.consensus_threshold_percent;
  report.valid_candidates = valid_candidates;

  // ---- Deduplicate votes before anything else -----------------------------
  std::map<VoteKey, EligibleVote> unique_votes;
  for (const EligibleVote& vote : votes) {
    const VoteKey key{vote.participant.value(), vote.generation.value(), vote.candidate.value()};
    const auto inserted = unique_votes.emplace(key, vote);
    if (!inserted.second) {
      report.duplicate_votes_suppressed += 1u;
    }
  }

  report.evaluations = static_cast<std::uint32_t>(unique_votes.size());

  std::set<std::uint64_t> contributor_ids;
  std::set<std::uint64_t> abstainer_ids;
  std::map<std::uint64_t, double> votes_per_candidate;
  for (const auto& entry : unique_votes) {
    const EligibleVote& vote = entry.second;
    if (vote.abstained) {
      abstainer_ids.insert(vote.participant.value());
    } else {
      contributor_ids.insert(vote.participant.value());
      votes_per_candidate[vote.candidate.value()] += 1.0;
    }
  }

  // ---- Denominator --------------------------------------------------------
  std::vector<const QuorumMember*> denominator;
  for (const QuorumMember& member : membership) {
    if (!member.authorized) {
      report.excluded_stale += 1u;
    }
    if (!member.required) {
      report.excluded_optional += 1u;
    }
    if (member.failed) {
      report.failures += 1u;
      report.failed_members.push_back(member.participant);
    }
    if (!member.available) {
      report.excluded_unavailable += 1u;
    }

    bool in_basis = false;
    switch (policy.vote_basis) {
      case VoteBasis::declared_participants:
        in_basis = true;
        break;
      case VoteBasis::authoritative_participants:
        in_basis = member.authorized;
        break;
      case VoteBasis::contributing_participants:
        in_basis = contributor_ids.count(member.participant.value()) != 0u ||
                   abstainer_ids.count(member.participant.value()) != 0u;
        break;
      case VoteBasis::required_participants:
        in_basis = member.required;
        break;
      case VoteBasis::eligible_participants:
        in_basis = member.authorized && member.available && !member.failed;
        break;
    }
    if (!in_basis) {
      continue;
    }
    if (!policy.count_optional_participants && !member.required) {
      continue;
    }
    if (!policy.count_failed_in_denominator && member.failed) {
      continue;
    }
    if (!policy.count_unavailable_in_denominator && !member.available) {
      continue;
    }
    denominator.push_back(&member);
  }

  report.denominator = static_cast<std::uint32_t>(denominator.size());
  for (const QuorumMember* member : denominator) {
    report.members.push_back(member->participant);
    report.member_generations.push_back(member->generation);
    if (contributor_ids.count(member->participant.value()) != 0u) {
      report.contributors += 1u;
      report.contributors_list.push_back(member->participant);
    } else if (abstainer_ids.count(member->participant.value()) != 0u) {
      report.abstentions += 1u;
      report.abstaining_members.push_back(member->participant);
    }
  }

  for (const QuorumMember& member : membership) {
    if (member.required) {
      report.required_total += 1u;
      if (contributor_ids.count(member.participant.value()) != 0u) {
        report.required_present += 1u;
      }
    }
  }

  // ---- Role quotas --------------------------------------------------------
  report.role_quota_required = static_cast<std::uint32_t>(policy.role_quota.size());
  for (const RoleQuota& quota : policy.role_quota) {
    std::uint32_t count = 0;
    for (const QuorumMember* member : denominator) {
      if (member->roles.contains(quota.role) && member->authorized && member->role_available) {
        count += 1u;
      }
    }
    if (count >= quota.min_count) {
      report.role_quota_met += 1u;
    } else {
      report.notes.push_back("role quota for " + std::string(ensemble_fabric::to_string(quota.role)) +
                             " not met: have " + std::to_string(count) + ", need " +
                             std::to_string(quota.min_count));
    }
  }

  // ---- Budgets ------------------------------------------------------------
  report.failure_budget_exceeded =
      policy.max_failures != unbounded && report.failures > policy.max_failures;
  report.abstention_budget_exceeded =
      policy.max_abstentions != unbounded && report.abstentions > policy.max_abstentions;

  // ---- Threshold ----------------------------------------------------------
  if (policy.consensus_threshold_percent > 0u) {
    report.threshold_count = percentage_ceiling(report.denominator,
                                                policy.consensus_threshold_percent);
  } else {
    report.threshold_count = 0u;
  }

  // ---- Possible/reached/not reached --------------------------------------
  std::uint32_t achievable = 0;
  for (const QuorumMember& member : membership) {
    if (member.authorized && member.available && !member.failed && member.role_available) {
      achievable += 1u;
    }
  }
  bool quota_achievable = true;
  for (const RoleQuota& quota : policy.role_quota) {
    std::uint32_t possible = 0;
    for (const QuorumMember& member : membership) {
      if (member.roles.contains(quota.role) && member.authorized && member.available &&
          !member.failed && member.role_available) {
        possible += 1u;
      }
    }
    if (possible < quota.min_count) {
      quota_achievable = false;
    }
  }

  const bool participant_requirement_met = report.denominator >= policy.min_participants;
  const bool role_requirement_met = report.role_quota_met == report.role_quota_required;
  const bool evaluation_requirement_met = report.evaluations >= policy.min_evaluations;
  const bool candidate_requirement_met = valid_candidates >= policy.min_valid_candidates;

  // A denominator can only shrink during an execution, so a denominator below
  // the stated minimum can never recover: that is impossibility, not a delay.
  // A role quota is different: it is impossible only when no member that could
  // still act holds the role.
  if (report.denominator < policy.min_participants || !quota_achievable) {
    report.state = QuorumState::impossible;
    report.notes.push_back("quorum is impossible: denominator " +
                           std::to_string(report.denominator) +
                           " with " + std::to_string(achievable) +
                           " member(s) still capable of contributing");
  } else if (participant_requirement_met && role_requirement_met && evaluation_requirement_met &&
             candidate_requirement_met && !report.failure_budget_exceeded &&
             !report.abstention_budget_exceeded) {
    report.state = QuorumState::reached;
  } else {
    report.state = QuorumState::not_reached;
    if (!participant_requirement_met) {
      report.notes.push_back("denominator " + std::to_string(report.denominator) +
                             " is below the required minimum " +
                             std::to_string(policy.min_participants));
    }
    if (!evaluation_requirement_met) {
      report.notes.push_back("evaluations " + std::to_string(report.evaluations) +
                             " are below the required minimum " +
                             std::to_string(policy.min_evaluations));
    }
    if (!candidate_requirement_met) {
      report.notes.push_back("valid candidates " + std::to_string(valid_candidates) +
                             " are below the required minimum " +
                             std::to_string(policy.min_valid_candidates));
    }
  }

  std::uint32_t leading = 0;
  for (const auto& entry : votes_per_candidate) {
    const std::uint32_t count = static_cast<std::uint32_t>(entry.second);
    if (count > leading) {
      leading = count;
    }
  }
  report.leading_votes = leading;
  return report;
}

std::string QuorumReport::render() const {
  std::string out = "quorum ";
  out.append(ensemble_fabric::to_string(state));
  out.append(" basis=").append(ensemble_fabric::to_string(basis));
  out.append(" denominator=").append(std::to_string(denominator));
  out.append(" contributors=").append(std::to_string(contributors));
  out.append(" abstentions=").append(std::to_string(abstentions));
  out.append(" failures=").append(std::to_string(failures));
  out.append(" evaluations=").append(std::to_string(evaluations));
  out.append(" valid_candidates=").append(std::to_string(valid_candidates));
  out.append(" threshold_count=").append(std::to_string(threshold_count));
  out.append(" role_quota=").append(std::to_string(role_quota_met))
      .append("/")
      .append(std::to_string(role_quota_required));
  out.append(" generation=").append(ensemble_generation.to_string());
  out.append(" epoch=").append(coordinator_epoch.to_string());
  return out;
}

}  // namespace ensemble_fabric
