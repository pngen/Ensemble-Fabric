#include "support/test_harness.hpp"
#include "support/test_support.hpp"

using namespace ensemble_fabric;

namespace {

EnsembleSpec basic_spec(std::uint64_t id, std::vector<ParticipantId> candidates) {
  EnsembleSpec spec = parallel_spec(id, std::move(candidates));
  spec.fallback.enabled = false;
  return spec;
}

}

EF_TEST(cancellation_before_dispatch_stops_every_branch) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = basic_spec(1, {ParticipantId(1), ParticipantId(2)});
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                 reference_program("ok").value(), 4001u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                 reference_program("ok").value(), 4002u);
  EF_REQUIRE_OK(harness.register_all());
  EF_REQUIRE_OK(fabric.cancel_ensemble(EnsembleId(1), EnsembleGeneration(1), "before dispatch"));

  Outcome<std::vector<FabricAction>> actions =
      fabric.advance(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(actions);
  EF_CHECK(actions.value().empty());
  Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_CHECK(snapshot.value().execution_status == ExecutionStatus::cancelled);
  for (const CandidateSnapshot& candidate : snapshot.value().candidates) {
    EF_CHECK(candidate.state == CandidateState::cancelled);
  }
}

EF_TEST(a_late_candidate_result_after_cancellation_is_rejected) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = basic_spec(1, {ParticipantId(1)});
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  HarnessParticipant& participant =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                     reference_program("ok").value(), 4003u);
  EF_REQUIRE_OK(harness.register_participant(participant));

  Outcome<std::vector<FabricAction>> actions =
      fabric.advance(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(actions);
  const auto* dispatch = std::get_if<DispatchAction>(&actions.value().front());
  EF_CHECK(dispatch != nullptr);
  if (dispatch == nullptr) {
    return;
  }
  CandidateAuthority authority;
  authority.participant = harness.authority_of(participant);
  authority.execution = dispatch->execution.execution;
  authority.attempt_generation = dispatch->execution.attempt_generation;
  authority.candidate = dispatch->candidate;
  authority.candidate_generation = dispatch->candidate_generation;

  EF_REQUIRE_OK(fabric.cancel_ensemble(EnsembleId(1), EnsembleGeneration(1), "during work"));
  (void)fabric.advance(EnsembleId(1), EnsembleGeneration(1));

  CandidateSubmission submission;
  submission.authority = authority;
  submission.payload = {std::byte{0x41}};
  EF_CHECK_ERR(fabric.submit_candidate(submission), EnsembleError::execution_cancelled);
  EF_CHECK_ERR(fabric.finalize(EnsembleId(1), EnsembleGeneration(1)),
               EnsembleError::ensemble_cancelled);
}

EF_TEST(a_cancelled_execution_never_commits_a_result) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = basic_spec(1, {ParticipantId(1)});
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  HarnessParticipant& participant =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                     reference_program("ok").value(), 4004u);
  EF_REQUIRE_OK(harness.register_participant(participant));

  EF_REQUIRE_OK(harness.step());
  EF_REQUIRE_OK(fabric.cancel_ensemble(EnsembleId(1), EnsembleGeneration(1), "mid flight"));
  EF_REQUIRE_OK(harness.run());
  EF_CHECK_ERR(fabric.finalize(EnsembleId(1), EnsembleGeneration(1)),
               EnsembleError::ensemble_cancelled);
  EF_CHECK_ERR(fabric.current_result(EnsembleId(1)), EnsembleError::not_found);
}

EF_TEST(cancellation_is_idempotent) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = basic_spec(1, {ParticipantId(1)});
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  EF_REQUIRE_OK(fabric.cancel_ensemble(EnsembleId(1), EnsembleGeneration(1), "first"));
  EF_REQUIRE_OK(fabric.cancel_ensemble(EnsembleId(1), EnsembleGeneration(1), "second"));
}

EF_TEST(cancelling_a_historical_generation_is_rejected) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = basic_spec(1, {ParticipantId(1)});
  EF_REQUIRE_OK(harness.define(spec));
  EF_CHECK_ERR(fabric.cancel_ensemble(EnsembleId(1), EnsembleGeneration(9), "stale"),
               EnsembleError::stale_ensemble_generation);
}

EF_TEST(a_superseded_generation_can_never_commit) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = basic_spec(1, {ParticipantId(1)});
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  HarnessParticipant& participant =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                     reference_program("ok").value(), 4005u);
  EF_REQUIRE_OK(harness.register_participant(participant));
  EF_REQUIRE_OK(harness.step());

  EF_REQUIRE_OK(fabric.supersede_ensemble(EnsembleId(1), EnsembleGeneration(1)));
  EF_REQUIRE_OK(harness.step());
  EF_CHECK_ERR(fabric.finalize(EnsembleId(1), EnsembleGeneration(1)),
               EnsembleError::ensemble_superseded);
}

EF_TEST_MAIN
