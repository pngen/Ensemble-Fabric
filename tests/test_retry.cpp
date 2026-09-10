#include "support/test_harness.hpp"
#include "support/test_support.hpp"

using namespace ensemble_fabric;

namespace {

EnsembleSpec retry_spec(std::uint32_t attempts, bool retry_malformed, bool replace) {
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  spec.retry.max_attempts = attempts;
  spec.retry.retry_on_malformed_candidate = retry_malformed;
  spec.retry.retry_on_participant_failed = true;
  spec.retry.replace_participant = replace;
  spec.retry.rerun_evaluations = true;
  return spec;
}

}

EF_TEST(a_retryable_failure_is_retried_with_a_new_candidate_generation) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = retry_spec(2u, true, false);
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  HarnessParticipant& participant =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                     reference_program("malformed;ok:value=recovered").value(), 6001u);
  EF_REQUIRE_OK(harness.register_participant(participant));
  EF_REQUIRE_OK(harness.run());

  Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_CHECK_EQ(snapshot.value().candidates.size(), 1u);
  EF_CHECK_EQ(snapshot.value().candidates.front().attempts_used, 2u);
  EF_CHECK_EQ(snapshot.value().candidates.front().attempt, 2u);
  EF_CHECK(snapshot.value().candidates.front().generation.value() > 1u);
  EF_CHECK(snapshot.value().candidates.front().state == CandidateState::valid);
  EF_CHECK_EQ(fabric.counters().retries_issued, 0u);
}

EF_TEST(a_non_retryable_failure_is_not_retried) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = retry_spec(3u, false, false);
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  HarnessParticipant& participant =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                     reference_program("malformed;ok:value=recovered").value(), 6002u);
  EF_REQUIRE_OK(harness.register_participant(participant));
  EF_REQUIRE_OK(harness.run());

  Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_CHECK_EQ(snapshot.value().candidates.front().attempts_used, 1u);
  EF_CHECK(snapshot.value().candidates.front().state == CandidateState::failed);
}

EF_TEST(retries_are_bounded_by_the_policy) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = retry_spec(3u, true, false);
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  HarnessParticipant& participant =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                     reference_program("malformed").value(), 6003u);
  EF_REQUIRE_OK(harness.register_participant(participant));
  EF_REQUIRE_OK(harness.run());
  Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_CHECK_EQ(snapshot.value().candidates.front().attempts_used, 3u);
  EF_CHECK(snapshot.value().candidates.front().state == CandidateState::failed);
}

EF_TEST(a_retry_supersedes_prior_evaluations) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = retry_spec(2u, true, false);
  spec.participants.push_back(judge_participant(ParticipantId(100)));
  spec.evidence.min_evaluations_per_candidate = 1u;
  spec.quorum.min_evaluations = 1u;
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                 reference_program("malformed;ok:value=recovered").value(), 6004u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                 reference_program("evaluate:judgment=accept,score=1").value(), 6005u);
  EF_REQUIRE_OK(harness.register_all());
  EF_REQUIRE_OK(harness.run());

  Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_CHECK_EQ(snapshot.value().evaluations.size(), 1u);
  EF_CHECK_EQ(snapshot.value().evaluations.front().candidate_generation.value(), 2u);
}

EF_TEST(a_participant_failure_without_replacement_does_not_resurrect_the_candidate) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = retry_spec(3u, true, false);
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  HarnessParticipant& participant =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                     reference_program("ok:value=never").value(), 6006u);
  EF_REQUIRE_OK(harness.register_participant(participant));
  // The incarnation dies before it can serve the slot, and the policy forbids a
  // replacement, so the logical candidate can never be filled.
  EF_REQUIRE_OK(fabric.report_participant_failure(harness.authority_of(participant),
                                                ParticipantFailureKind::unavailable, "died"));
  EF_REQUIRE_OK(harness.run());
  Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_CHECK(snapshot.value().candidates.front().state == CandidateState::failed);
}

EF_TEST_MAIN
