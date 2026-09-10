#include "support/test_harness.hpp"
#include "support/test_support.hpp"

using namespace ensemble_fabric;

namespace {

QuorumMember member(std::uint64_t id, bool required, bool authorized = true, bool available = true,
                    bool failed = false, ParticipantRole role = ParticipantRole::candidate) {
  QuorumMember value;
  value.participant = ParticipantId(id);
  value.generation = ParticipantGeneration(1);
  value.roles.insert(role);
  value.required = required;
  value.authorized = authorized;
  value.available = available;
  value.failed = failed;
  return value;
}

EligibleVote vote(std::uint64_t participant, std::uint64_t candidate, bool abstained = false) {
  EligibleVote value;
  value.participant = ParticipantId(participant);
  value.generation = ParticipantGeneration(1);
  value.candidate = CandidateId(candidate);
  value.candidate_generation = CandidateGeneration(1);
  value.abstained = abstained;
  return value;
}

}

EF_TEST(absolute_quorum_counts_distinct_authoritative_members) {
  QuorumPolicy policy;
  policy.min_participants = 3u;
  policy.min_valid_candidates = 1u;
  policy.vote_basis = VoteBasis::authoritative_participants;
  policy.count_optional_participants = false;
  const std::vector<QuorumMember> membership = {member(1, true), member(2, true), member(3, true),
                                                member(4, false)};
  const std::vector<EligibleVote> votes = {vote(1, 10), vote(2, 10)};
  const QuorumReport report = compute_quorum(policy, membership, votes, 1, EnsembleGeneration(1),
                                             CoordinatorEpoch(1), Sequence(1));
  EF_CHECK_EQ(report.denominator, 3u);
  EF_CHECK(report.state == QuorumState::reached);
  EF_CHECK_EQ(report.contributors, 2u);
}

EF_TEST(optional_participants_can_be_excluded_from_the_denominator) {
  QuorumPolicy policy;
  policy.min_participants = 2u;
  policy.count_optional_participants = false;
  const std::vector<QuorumMember> membership = {member(1, true), member(2, true), member(3, false)};
  const QuorumReport report = compute_quorum(policy, membership, {}, 1, EnsembleGeneration(1),
                                             CoordinatorEpoch(1), Sequence(1));
  EF_CHECK_EQ(report.denominator, 2u);
  EF_CHECK_EQ(report.excluded_optional, 1u);
}

EF_TEST(a_failed_participant_leaves_the_denominator_only_when_the_policy_says_so) {
  QuorumPolicy policy;
  policy.min_participants = 3u;
  policy.count_failed_in_denominator = false;
  std::vector<QuorumMember> membership = {
      member(1, true), member(2, true), member(3, true, true, true, true)};
  QuorumReport report = compute_quorum(policy, membership, {}, 1, EnsembleGeneration(1),
                                       CoordinatorEpoch(1), Sequence(1));
  EF_CHECK_EQ(report.denominator, 2u);
  EF_CHECK_EQ(report.failures, 1u);
  EF_CHECK(report.state != QuorumState::reached);

  policy.count_failed_in_denominator = true;
  report = compute_quorum(policy, membership, {}, 1, EnsembleGeneration(1), CoordinatorEpoch(1),
                          Sequence(1));
  EF_CHECK_EQ(report.denominator, 3u);
  EF_CHECK(report.state == QuorumState::reached);
}

EF_TEST(duplicate_votes_from_one_participant_generation_never_inflate_quorum) {
  QuorumPolicy policy;
  policy.min_participants = 1u;
  policy.consensus_threshold_percent = 0u;
  const std::vector<QuorumMember> membership = {member(1, true), member(2, true)};
  const std::vector<EligibleVote> votes = {vote(1, 10), vote(1, 10), vote(1, 10)};
  const QuorumReport report = compute_quorum(policy, membership, votes, 1, EnsembleGeneration(1),
                                             CoordinatorEpoch(1), Sequence(1));
  EF_CHECK_EQ(report.duplicate_votes_suppressed, 2u);
  EF_CHECK_EQ(report.leading_votes, 1u);
  EF_CHECK_EQ(report.contributors, 1u);
}

EF_TEST(abstentions_are_counted_separately_from_contributors) {
  QuorumPolicy policy;
  policy.min_participants = 2u;
  policy.count_abstentions = true;
  const std::vector<QuorumMember> membership = {member(1, true), member(2, true)};
  const std::vector<EligibleVote> votes = {vote(1, 10), vote(2, 10, true)};
  const QuorumReport report = compute_quorum(policy, membership, votes, 1, EnsembleGeneration(1),
                                             CoordinatorEpoch(1), Sequence(1));
  EF_CHECK_EQ(report.contributors, 1u);
  EF_CHECK_EQ(report.abstentions, 1u);
  EF_CHECK_EQ(report.leading_votes, 1u);
}

EF_TEST(quorum_becomes_impossible_once_too_few_members_can_still_contribute) {
  QuorumPolicy policy;
  policy.min_participants = 3u;
  const std::vector<QuorumMember> membership = {member(1, true), member(2, true, true, true, true),
                                                member(3, true, true, true, true)};
  const QuorumReport report = compute_quorum(policy, membership, {}, 1, EnsembleGeneration(1),
                                             CoordinatorEpoch(1), Sequence(1));
  EF_CHECK(report.state == QuorumState::impossible);
}

EF_TEST(role_quota_is_evaluated_against_authoritative_members) {
  QuorumPolicy policy;
  policy.min_participants = 1u;
  policy.role_quota.push_back(RoleQuota{ParticipantRole::judge, 2u});
  std::vector<QuorumMember> membership = {member(1, true),
                                          member(2, true, true, true, false, ParticipantRole::judge)};
  QuorumReport report = compute_quorum(policy, membership, {}, 1, EnsembleGeneration(1),
                                       CoordinatorEpoch(1), Sequence(1));
  EF_CHECK(report.state != QuorumState::reached);
  EF_CHECK_EQ(report.role_quota_met, 0u);

  membership.push_back(member(3, true, true, true, false, ParticipantRole::judge));
  report = compute_quorum(policy, membership, {}, 1, EnsembleGeneration(1), CoordinatorEpoch(1),
                          Sequence(1));
  EF_CHECK_EQ(report.role_quota_required, 1u);
  EF_CHECK_EQ(report.role_quota_met, 1u);
  EF_CHECK(report.state == QuorumState::reached);
}

EF_TEST(evaluation_and_candidate_minimums_gate_quorum) {
  QuorumPolicy policy;
  policy.min_participants = 1u;
  policy.min_evaluations = 2u;
  policy.min_valid_candidates = 1u;
  const std::vector<QuorumMember> membership = {member(1, true)};
  const std::vector<EligibleVote> votes = {vote(1, 10)};
  const QuorumReport report = compute_quorum(policy, membership, votes, 1, EnsembleGeneration(1),
                                             CoordinatorEpoch(1), Sequence(1));
  EF_CHECK(report.state == QuorumState::not_reached);
  EF_CHECK(!report.notes.empty());
}

EF_TEST(threshold_count_is_derived_from_the_stated_denominator) {
  QuorumPolicy policy;
  policy.min_participants = 1u;
  policy.consensus_threshold_percent = 66u;
  const std::vector<QuorumMember> membership = {member(1, true), member(2, true), member(3, true)};
  const QuorumReport report = compute_quorum(policy, membership, {}, 1, EnsembleGeneration(1),
                                             CoordinatorEpoch(1), Sequence(1));
  EF_CHECK_EQ(report.threshold_count, 2u);
  EF_CHECK_EQ(report.denominator, 3u);
}

EF_TEST(failure_and_abstention_budgets_are_enforced) {
  QuorumPolicy policy;
  policy.min_participants = 1u;
  policy.max_failures = 0u;
  const std::vector<QuorumMember> membership = {member(1, true, true, true, true),
                                                member(2, true)};
  const QuorumReport report = compute_quorum(policy, membership, {}, 1, EnsembleGeneration(1),
                                             CoordinatorEpoch(1), Sequence(1));
  EF_CHECK(report.failure_budget_exceeded);
  EF_CHECK_EQ(report.failures, 1u);
  EF_CHECK(report.state == QuorumState::not_reached);
}

EF_TEST_MAIN
