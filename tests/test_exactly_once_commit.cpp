#include "support/test_harness.hpp"
#include "support/test_support.hpp"

using namespace ensemble_fabric;

namespace {

struct Fixture {
  EnsembleFabric fabric;
  EnsembleHarness harness{fabric};
  HarnessParticipant* candidate_a{nullptr};
  HarnessParticipant* candidate_b{nullptr};

  explicit Fixture(const std::string& script_a = "ok:value=A",
                   const std::string& script_b = "ok:value=B",
                   std::uint32_t judges = 0u) {
    EnsembleSpec spec = parallel_spec(1, {ParticipantId(1), ParticipantId(2)});
    for (std::uint32_t index = 0; index < judges; ++index) {
      spec.participants.push_back(judge_participant(ParticipantId(100u + index)));
    }
    if (judges != 0u) {
      spec.quorum.min_evaluations = 1u;
      spec.evidence.min_evaluations_per_candidate = 1u;
    }
    (void)harness.define(spec);
    (void)harness.open(EnsembleId(1), EnsembleGeneration(1));
    candidate_a = &harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                                  reference_program(script_a).value(), 3001u);
    candidate_b = &harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                                  reference_program(script_b).value(), 3002u);
    for (std::uint32_t index = 0; index < judges; ++index) {
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[2u + index],
                     reference_program("evaluate:judgment=accept,score=0.5").value(),
                     3100u + index);
    }
    (void)harness.register_all();
  }
};

}

EF_TEST(exactly_one_authoritative_result_is_committed) {
  Fixture fixture;
  EF_REQUIRE_OK(fixture.harness.run());
  Outcome<EnsembleResult> first = fixture.fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(first);
  EF_CHECK(first.value().authoritative());
  EF_CHECK_EQ(first.value().decision, EnsembleDecision::committed);
  EF_CHECK(fixture.fabric.counters().commits_accepted == 1u);

  Outcome<EnsembleSnapshot> snapshot = fixture.fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_CHECK_EQ(snapshot.value().commits.size(), 1u);
  EF_CHECK_EQ(snapshot.value().commits.front().decision, EnsembleDecision::committed);
}

EF_TEST(a_repeated_identical_commit_is_idempotent) {
  Fixture fixture;
  EF_REQUIRE_OK(fixture.harness.run());
  Outcome<EnsembleResult> prepared =
      fixture.fabric.prepare_result(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(prepared);
  Outcome<EnsembleResult> first = fixture.fabric.commit_result(prepared.value());
  EF_REQUIRE_OK(first);
  Outcome<EnsembleResult> second = fixture.fabric.commit_result(prepared.value());
  EF_REQUIRE_OK(second);
  EF_CHECK_EQ(first.value().id.value(), second.value().id.value());
  EF_CHECK_EQ(first.value().commit_fingerprint, second.value().commit_fingerprint);
  EF_CHECK_EQ(fixture.fabric.counters().commits_accepted, 1u);
}

EF_TEST(a_conflicting_commit_is_rejected) {
  Fixture fixture;
  EF_REQUIRE_OK(fixture.harness.run());
  Outcome<EnsembleResult> prepared =
      fixture.fabric.prepare_result(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(prepared);
  EF_REQUIRE_OK(fixture.fabric.commit_result(prepared.value()));

  EnsembleResult conflicting = prepared.value();
  conflicting.selected_candidate = CandidateId(9999);
  conflicting.commit_fingerprint = 0u;
  EF_CHECK_ERR(fixture.fabric.commit_result(conflicting),
               EnsembleError::conflicting_result_commit);
  EF_CHECK_EQ(fixture.fabric.counters().commits_rejected_conflicting, 1u);
}

EF_TEST(a_commit_for_a_historical_generation_is_rejected) {
  Fixture fixture;
  EF_REQUIRE_OK(fixture.harness.run());
  Outcome<EnsembleResult> prepared =
      fixture.fabric.prepare_result(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(prepared);
  EnsembleResult stale = prepared.value();
  stale.ensemble_generation = EnsembleGeneration(2);
  stale.commit_fingerprint = 0u;
  EF_CHECK_ERR(fixture.fabric.commit_result(stale), EnsembleError::stale_ensemble_generation);
  EF_CHECK_EQ(fixture.fabric.counters().commits_rejected_stale, 1u);
}

EF_TEST(a_commit_from_a_stale_coordinator_epoch_is_rejected) {
  Fixture fixture;
  EF_REQUIRE_OK(fixture.harness.run());
  Outcome<EnsembleResult> prepared =
      fixture.fabric.prepare_result(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(prepared);
  EnsembleResult stale = prepared.value();
  stale.coordinator_epoch = CoordinatorEpoch(stale.coordinator_epoch.value() + 5u);
  stale.commit_fingerprint = 0u;
  EF_CHECK_ERR(fixture.fabric.commit_result(stale), EnsembleError::stale_coordinator_epoch);
}

EF_TEST(finalize_requires_a_settled_execution) {
  Fixture fixture;
  // Candidates have not been dispatched yet; the execution cannot be finalized.
  EF_CHECK_ERR(fixture.fabric.finalize(EnsembleId(1), EnsembleGeneration(1)),
               EnsembleError::invalid_state_transition);
  EF_REQUIRE_OK(fixture.harness.run());
  EF_REQUIRE_OK(fixture.fabric.finalize(EnsembleId(1), EnsembleGeneration(1)));
}

EF_TEST(a_committed_result_preserves_payload_and_provenance) {
  Fixture fixture("ok:value=alpha");
  EF_REQUIRE_OK(fixture.harness.run());
  Outcome<EnsembleResult> result = fixture.fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(result);
  EF_CHECK(result.value().has_selected);
  EF_CHECK_EQ(result.value().payload_bytes, 5u);
  EF_CHECK_EQ(std::string(reinterpret_cast<const char*>(result.value().payload.data()),
                          result.value().payload.size()),
              std::string("alpha"));
  EF_CHECK(result.value().selected_participant.valid());
  EF_CHECK_EQ(result.value().selected_candidate_generation.value(), 1u);
  EF_CHECK(result.value().selected_role == ParticipantRole::candidate);
  EF_CHECK(!result.value().explanation.empty());
}

EF_TEST(the_same_evidence_produces_the_same_decision_regardless_of_order) {
  Fixture forward("ok:value=A", "ok:value=B");
  Fixture backward("ok:value=B", "ok:value=A");
  EF_REQUIRE_OK(forward.harness.run());
  EF_REQUIRE_OK(backward.harness.run());
  Outcome<EnsembleResult> first =
      forward.fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
  Outcome<EnsembleResult> second =
      backward.fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(first);
  EF_REQUIRE_OK(second);
  EF_CHECK_EQ(first.value().selected_candidate.value(), 1u);
  EF_CHECK_EQ(second.value().selected_candidate.value(), 1u);
  EF_CHECK_EQ(first.value().quorum.render(), second.value().quorum.render());
}

EF_TEST(cancelling_a_committed_execution_does_not_revoke_the_result) {
  Fixture fixture;
  EF_REQUIRE_OK(fixture.harness.run());
  EF_REQUIRE_OK(fixture.fabric.finalize(EnsembleId(1), EnsembleGeneration(1)));
  EF_CHECK_ERR(fixture.fabric.cancel_ensemble(EnsembleId(1), EnsembleGeneration(1), "late"),
               EnsembleError::result_already_committed);
  Outcome<EnsembleResult> result = fixture.fabric.current_result(EnsembleId(1));
  EF_REQUIRE_OK(result);
  EF_CHECK(result.value().authoritative());
}

EF_TEST_MAIN
