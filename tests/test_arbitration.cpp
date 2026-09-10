#include "support/test_harness.hpp"
#include "support/test_support.hpp"

using namespace ensemble_fabric;

namespace {

ArbitrationCandidateInput candidate(std::uint64_t id, CandidateState state,
                                    std::uint32_t accepts = 0u, std::uint32_t rejects = 0u,
                                    double score = 0.0, std::uint32_t verifier_pass = 0u,
                                    std::uint32_t verifier_fail = 0u) {
  ArbitrationCandidateInput value;
  value.candidate = CandidateId(id);
  value.generation = CandidateGeneration(1);
  value.participant = ParticipantId(100u + id);
  value.participant_generation = ParticipantGeneration(1);
  value.role = ParticipantRole::candidate;
  value.state = state;
  value.evaluation.candidate = CandidateId(id);
  value.evaluation.generation = CandidateGeneration(1);
  value.evaluation.accepts = accepts;
  value.evaluation.rejects = rejects;
  value.evaluation.evaluations = accepts + rejects;
  value.evaluation.verifier_pass = verifier_pass;
  value.evaluation.verifier_fail = verifier_fail;
  if (value.evaluation.evaluations != 0u || verifier_pass != 0u || verifier_fail != 0u) {
    value.evaluation.weighted_score_sum = score * static_cast<double>(accepts + rejects);
    value.evaluation.weight_sum = static_cast<double>(accepts + rejects);
    value.evaluation.min_score = score;
    value.evaluation.max_score = score;
  }
  return value;
}

ArbitrationInputs inputs(std::vector<ArbitrationCandidateInput> candidates) {
  ArbitrationInputs value;
  value.candidates = std::move(candidates);
  value.policy.factors = {ArbitrationFactor::judge_acceptance_ratio,
                          ArbitrationFactor::aggregate_score};
  value.policy.tie_break = TieBreakPolicy::lowest_candidate_id;
  value.quorum_satisfied = true;
  value.ensemble_generation = EnsembleGeneration(1);
  value.coordinator_epoch = CoordinatorEpoch(1);
  value.sequence = Sequence(1);
  return value;
}

}

EF_TEST(hard_eligibility_precedes_every_ranking_factor) {
  ArbitrationInputs value = inputs({candidate(10, CandidateState::valid, 3u, 0u, 100.0),
                                    candidate(11, CandidateState::valid, 1u, 0u, 1.0)});
  const ArbitrationReport report = arbitrate(value);
  EF_CHECK_EQ(report.selected.value(), 10u);
  EF_CHECK(!report.tie_unresolved);
}

EF_TEST(a_hard_invalid_candidate_never_wins_on_score) {
  ArbitrationInputs value = inputs({candidate(10, CandidateState::invalid, 5u, 0u, 1000.0),
                                    candidate(11, CandidateState::valid, 1u, 0u, 1.0)});
  const ArbitrationReport report = arbitrate(value);
  EF_CHECK_EQ(report.selected.value(), 11u);
  bool eliminated = false;
  for (const CandidateRanking& ranking : report.ranking) {
    if (ranking.candidate.value() == 10u) {
      eliminated = !ranking.eligible && ranking.elimination == EliminationReason::invalid_candidate;
    }
  }
  EF_CHECK(eliminated);
}

EF_TEST(a_stale_candidate_generation_is_ineligible) {
  ArbitrationInputs value = inputs({candidate(10, CandidateState::valid, 5u, 0u, 10.0)});
  value.candidates.front().generation_current = false;
  const ArbitrationReport report = arbitrate(value);
  EF_CHECK(report.no_eligible_candidate);
  EF_CHECK_EQ(report.ranking.front().elimination, EliminationReason::stale_generation);
}

EF_TEST(verifier_gating_rejects_a_highly_ranked_candidate) {
  ArbitrationInputs value =
      inputs({candidate(10, CandidateState::valid, 3u, 0u, 100.0, 0u, 1u),
              candidate(11, CandidateState::valid, 1u, 0u, 1.0, 1u, 0u)});
  value.aggregation.require_verification_pass = true;
  const ArbitrationReport report = arbitrate(value);
  EF_CHECK_EQ(report.selected.value(), 11u);
  for (const CandidateRanking& ranking : report.ranking) {
    if (ranking.candidate.value() == 10u) {
      EF_CHECK_EQ(ranking.elimination, EliminationReason::verifier_failed);
    }
  }
}

EF_TEST(a_mandatory_predicate_failure_beats_a_favourable_score) {
  ArbitrationInputs value = inputs({candidate(10, CandidateState::valid, 3u, 0u, 500.0),
                                    candidate(11, CandidateState::valid, 1u, 0u, 1.0)});
  value.evidence.mandatory_criteria.push_back(CriterionRef{7u, 1u});
  value.candidates[0].evaluation.hard_predicates = 1u;
  value.candidates[0].evaluation.hard_failures = 1u;
  value.candidates[1].evaluation.hard_predicates = 1u;
  const ArbitrationReport report = arbitrate(value);
  EF_CHECK_EQ(report.selected.value(), 11u);
}

EF_TEST(missing_mandatory_evidence_is_not_a_pass) {
  ArbitrationInputs value = inputs({candidate(10, CandidateState::valid, 3u, 0u, 500.0)});
  value.evidence.mandatory_criteria.push_back(CriterionRef{7u, 1u});
  const ArbitrationReport report = arbitrate(value);
  EF_CHECK(report.no_eligible_candidate);
  EF_CHECK_EQ(report.ranking.front().elimination, EliminationReason::missing_required_evidence);
}

EF_TEST(an_unknown_mandatory_predicate_is_not_silently_a_pass) {
  ArbitrationInputs value = inputs({candidate(10, CandidateState::valid, 3u, 0u, 500.0)});
  value.evidence.mandatory_criteria.push_back(CriterionRef{7u, 1u});
  value.candidates[0].evaluation.hard_predicates = 1u;
  value.candidates[0].evaluation.hard_unknowns = 1u;
  value.evidence.unknown_verification_is_failure = true;
  const ArbitrationReport report = arbitrate(value);
  EF_CHECK(report.no_eligible_candidate);
  EF_CHECK_EQ(report.ranking.front().elimination, EliminationReason::hard_predicate_failed);
}

EF_TEST(a_tie_is_reported_when_the_policy_demands_it) {
  ArbitrationInputs value = inputs({candidate(10, CandidateState::valid, 2u, 0u, 5.0),
                                    candidate(11, CandidateState::valid, 2u, 0u, 5.0)});
  value.policy.tie_break = TieBreakPolicy::report_tie;
  const ArbitrationReport report = arbitrate(value);
  EF_CHECK(report.tie_unresolved);
  EF_CHECK(!report.selected.valid());
}

EF_TEST(stable_identity_resolves_a_tie_deterministically) {
  ArbitrationInputs value = inputs({candidate(11, CandidateState::valid, 2u, 0u, 5.0),
                                    candidate(10, CandidateState::valid, 2u, 0u, 5.0)});
  value.policy.tie_break = TieBreakPolicy::lowest_candidate_id;
  const ArbitrationReport first = arbitrate(value);
  const ArbitrationReport second = arbitrate(value);
  EF_CHECK_EQ(first.selected.value(), 10u);
  EF_CHECK_EQ(second.selected.value(), 10u);
}

EF_TEST(ranking_is_independent_of_input_order) {
  std::vector<ArbitrationCandidateInput> forward = {candidate(10, CandidateState::valid, 1u, 0u, 1.0),
                                                    candidate(11, CandidateState::valid, 3u, 0u, 9.0),
                                                    candidate(12, CandidateState::valid, 2u, 0u, 4.0)};
  std::vector<ArbitrationCandidateInput> backward(forward.rbegin(), forward.rend());
  const ArbitrationReport first = arbitrate(inputs(forward));
  const ArbitrationReport second = arbitrate(inputs(backward));
  EF_CHECK_EQ(first.selected.value(), second.selected.value());
  EF_CHECK_EQ(first.selected.value(), 11u);
}

EF_TEST(every_elimination_carries_a_reason_and_a_detail) {
  ArbitrationInputs value = inputs({candidate(10, CandidateState::invalid),
                                    candidate(11, CandidateState::failed),
                                    candidate(12, CandidateState::abstained),
                                    candidate(13, CandidateState::valid, 1u, 0u, 1.0)});
  const ArbitrationReport report = arbitrate(value);
  for (const CandidateRanking& ranking : report.ranking) {
    if (!ranking.eligible) {
      EF_CHECK(ranking.elimination != EliminationReason::none);
      EF_CHECK(!ranking.elimination_detail.empty());
    }
  }
  EF_CHECK_EQ(report.eliminated, 3u);
  EF_CHECK_EQ(report.considered, 4u);
}

EF_TEST(a_lower_cost_wins_when_cost_is_the_only_differing_factor) {
  ArbitrationInputs value = inputs({candidate(10, CandidateState::valid, 1u, 0u, 1.0),
                                    candidate(11, CandidateState::valid, 1u, 0u, 1.0)});
  value.policy.factors = {ArbitrationFactor::execution_cost};
  value.candidates[0].execution_cost = 90u;
  value.candidates[1].execution_cost = 10u;
  const ArbitrationReport report = arbitrate(value);
  EF_CHECK_EQ(report.selected.value(), 11u);
}

EF_TEST_MAIN
