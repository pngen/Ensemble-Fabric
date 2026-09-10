#include "support/test_harness.hpp"
#include "support/test_support.hpp"

using namespace ensemble_fabric;

namespace {

EnsembleSpec fallback_spec(FallbackTrigger trigger, std::uint32_t attempts = 1u,
                           bool replace = false) {
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  spec.participants.push_back(fallback_participant(ParticipantId(5)));
  spec.fallback.enabled = true;
  spec.fallback.triggers.push_back(trigger);
  spec.fallback.fallback_participants.push_back(ParticipantId(5));
  spec.fallback.max_depth = 1u;
  spec.retry.max_attempts = attempts;
  spec.retry.retry_on_unavailable = true;
  spec.retry.replace_participant = replace;
  return spec;
}

}

EF_TEST(fallback_is_not_activated_without_an_explicit_trigger) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = fallback_spec(FallbackTrigger::required_participant_failed);
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                 reference_program("ok:value=primary").value(), 5001u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                 reference_program("ok:value=fallback").value(), 5002u);
  EF_REQUIRE_OK(harness.register_all());
  EF_REQUIRE_OK(harness.run());
  Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_CHECK(!snapshot.value().fallback_activated);
  EF_CHECK_EQ(fabric.counters().fallback_activations, 0u);
}

EF_TEST(fallback_activates_for_an_unavailable_participant_and_is_inspectable) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = fallback_spec(FallbackTrigger::participant_unavailable);
  spec.participants[0].required = true;
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  HarnessParticipant& primary =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                     reference_program("ok:value=primary").value(), 5003u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                 reference_program("ok:value=fallback").value(), 5004u);
  EF_REQUIRE_OK(harness.register_participant(primary));
  EF_REQUIRE_OK(harness.register_participant(harness.participants().back()));

  EF_REQUIRE_OK(harness.step());
  EF_REQUIRE_OK(fabric.report_participant_failure(harness.authority_of(primary),
                                                ParticipantFailureKind::unavailable, "gone"));
  EF_REQUIRE_OK(harness.run());
  Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_CHECK(snapshot.value().fallback_activated);
  EF_CHECK_EQ(snapshot.value().fallback_depth, 1u);
  EF_CHECK_EQ(fabric.counters().fallback_activations, 1u);
}

EF_TEST(a_stale_primary_result_cannot_replace_a_fallback_result) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = fallback_spec(FallbackTrigger::participant_unavailable);
  spec.participants[0].required = true;
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  HarnessParticipant& primary =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                     reference_program("gate:gate=never").value(), 5005u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                 reference_program("ok:value=fallback").value(), 5006u);
  EF_REQUIRE_OK(harness.register_participant(primary));
  EF_REQUIRE_OK(harness.register_participant(harness.participants().back()));

  // The primary participant is never dispatched because it is not authoritative
  // after its failure; the late result is fenced by the runtime.
  EF_REQUIRE_OK(fabric.report_participant_failure(harness.authority_of(primary),
                                                ParticipantFailureKind::unavailable, "gone"));
  EF_REQUIRE_OK(harness.run());
  Outcome<EnsembleResult> result = fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(result);
  EF_CHECK(result.value().fallback_used);
  EF_CHECK_EQ(result.value().selected_participant.value(), 5u);
}

EF_TEST(fallback_does_not_activate_because_of_an_unfavourable_rank) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = fallback_spec(FallbackTrigger::retry_exhausted);
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                 reference_program("ok:value=primary").value(), 5007u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                 reference_program("ok:value=fallback").value(), 5008u);
  EF_REQUIRE_OK(harness.register_all());
  EF_REQUIRE_OK(harness.run());
  EF_CHECK(!fabric.inspect(EnsembleId(1)).value().fallback_activated);
  EF_REQUIRE_OK(fabric.finalize(EnsembleId(1), EnsembleGeneration(1)));
}

EF_TEST_MAIN
