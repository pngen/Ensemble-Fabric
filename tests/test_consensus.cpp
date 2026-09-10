#include "support/test_harness.hpp"
#include "support/test_support.hpp"

using namespace ensemble_fabric;

namespace {

CandidateTally tally(std::uint64_t candidate, std::uint32_t agreeing, std::uint32_t disagreeing,
                     std::uint32_t abstaining = 0u, bool eligible = true) {
  CandidateTally value;
  value.candidate = CandidateId(candidate);
  value.generation = CandidateGeneration(1);
  value.participant = ParticipantId(candidate + 100u);
  value.eligible = eligible;
  value.agreeing = agreeing;
  value.disagreeing = disagreeing;
  value.abstaining = abstaining;
  value.weighted_agree = static_cast<double>(agreeing);
  for (std::uint32_t index = 0; index < agreeing; ++index) {
    value.agreeing_members.push_back(ParticipantId(500u + index));
  }
  for (std::uint32_t index = 0; index < disagreeing; ++index) {
    value.disagreeing_members.push_back(ParticipantId(600u + index));
  }
  return value;
}

ConsensusInputs inputs(QuorumState state, std::vector<CandidateTally> tallies,
                       std::uint32_t threshold_percent = 0u) {
  ConsensusInputs value;
  value.quorum.state = state;
  value.quorum.denominator = 4u;
  value.quorum.contributors = 3u;
  value.quorum.threshold_percent = threshold_percent;
  value.quorum.threshold_count = threshold_percent == 0u ? 0u : 2u;
  value.tallies = std::move(tallies);
  value.candidates_declared = static_cast<std::uint32_t>(value.tallies.size());
  value.ensemble_generation = EnsembleGeneration(1);
  value.coordinator_epoch = CoordinatorEpoch(1);
  value.sequence = Sequence(1);
  return value;
}

}

EF_TEST(a_clear_majority_without_a_threshold_is_a_plurality) {
  const ConsensusReport report =
      compute_consensus(inputs(QuorumState::reached, {tally(10, 3, 0), tally(11, 1, 0)}));
  EF_CHECK(report.state == ConsensusState::plurality_without_threshold);
  EF_CHECK_EQ(report.leading_candidate.value(), 10u);
  EF_CHECK_EQ(report.agreeing, 3u);
}

EF_TEST(an_exact_tie_is_reported_as_a_tie_and_never_resolved_silently) {
  const ConsensusReport report =
      compute_consensus(inputs(QuorumState::reached, {tally(10, 2, 0), tally(11, 2, 0)}));
  EF_CHECK(report.state == ConsensusState::tie);
  EF_CHECK(report.unresolved_disagreement);
}

EF_TEST(a_configured_threshold_that_is_not_met_is_disagreement) {
  const ConsensusReport report =
      compute_consensus(inputs(QuorumState::reached, {tally(10, 1, 0), tally(11, 0, 1)}, 66u));
  EF_CHECK(report.state == ConsensusState::disagreement);
  EF_CHECK(!report.resolved());
}

EF_TEST(a_configured_threshold_that_is_met_is_consensus) {
  const ConsensusReport report =
      compute_consensus(inputs(QuorumState::reached, {tally(10, 3, 1), tally(11, 1, 0)}, 66u));
  EF_CHECK(report.state == ConsensusState::consensus);
  EF_CHECK(report.resolved());
}

EF_TEST(abstentions_are_reported_with_consensus) {
  ConsensusInputs value = inputs(QuorumState::reached, {tally(10, 3, 0, 2), tally(11, 1, 0)}, 50u);
  value.quorum.abstentions = 2u;
  const ConsensusReport report = compute_consensus(value);
  EF_CHECK(report.state == ConsensusState::consensus_with_abstentions);
  EF_CHECK_EQ(report.abstaining, 2u);
}

EF_TEST(quorum_failure_dominates_any_vote_arithmetic) {
  const ConsensusReport report = compute_consensus(
      inputs(QuorumState::not_reached, {tally(10, 3, 0), tally(11, 1, 0)}));
  EF_CHECK(report.state == ConsensusState::quorum_not_reached);
}

EF_TEST(quorum_impossibility_is_distinct_from_not_reached) {
  const ConsensusReport report =
      compute_consensus(inputs(QuorumState::impossible, {tally(10, 1, 0)}));
  EF_CHECK(report.state == ConsensusState::quorum_impossible);
}

EF_TEST(all_invalid_candidates_are_distinct_from_no_eligible_candidate) {
  ConsensusInputs value = inputs(QuorumState::reached, {tally(10, 0, 0, 0, false)});
  value.invalid_candidates = 1u;
  EF_CHECK(compute_consensus(value).state == ConsensusState::all_candidates_invalid);

  ConsensusInputs other = inputs(QuorumState::reached, {});
  other.candidates_declared = 1u;
  other.invalid_candidates = 0u;
  EF_CHECK(compute_consensus(other).state == ConsensusState::no_eligible_candidate);
}

EF_TEST(cancellation_and_supersession_override_every_other_state) {
  ConsensusInputs value = inputs(QuorumState::reached, {tally(10, 3, 0)});
  value.cancelled = true;
  EF_CHECK(compute_consensus(value).state == ConsensusState::cancelled);
  value.cancelled = false;
  value.superseded = true;
  EF_CHECK(compute_consensus(value).state == ConsensusState::superseded);
  value.superseded = false;
  value.revalidation_required = true;
  EF_CHECK(compute_consensus(value).state == ConsensusState::revalidation_required);
}

EF_TEST(evidence_without_any_agreement_is_insufficient) {
  const ConsensusReport report = compute_consensus(
      inputs(QuorumState::reached, {tally(10, 0, 2), tally(11, 0, 2)}));
  EF_CHECK(report.state == ConsensusState::insufficient_evidence);
}

EF_TEST(a_leader_is_reported_even_when_the_threshold_is_unmet) {
  const ConsensusReport report =
      compute_consensus(inputs(QuorumState::reached, {tally(10, 2, 0), tally(11, 1, 0)}, 90u));
  EF_CHECK_EQ(report.leading_candidate.value(), 10u);
  EF_CHECK_EQ(report.runner_up_votes, 1u);
}

EF_TEST_MAIN
