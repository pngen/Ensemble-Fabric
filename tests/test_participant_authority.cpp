#include "support/test_harness.hpp"
#include "support/test_support.hpp"

using namespace ensemble_fabric;

EF_TEST(registration_requires_the_current_epoch_and_generation) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  EF_REQUIRE_OK(harness.define(spec));
  HarnessParticipant& participant =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants.front(),
                     reference_program("ok").value(), 2001u);
  ParticipantRegistration registration;
  registration.ensemble = EnsembleId(1);
  registration.ensemble_generation = EnsembleGeneration(2);
  registration.participant = participant.id;
  registration.worker = participant.worker;
  registration.worker_boot = participant.boot;
  registration.coordinator_epoch = fabric.epoch();
  registration.claimed_roles = participant.roles;
  EF_CHECK_ERR(fabric.register_participant(registration), EnsembleError::stale_ensemble_generation);

  registration.ensemble_generation = EnsembleGeneration(1);
  registration.coordinator_epoch = CoordinatorEpoch(fabric.epoch().value() + 1u);
  EF_CHECK_ERR(fabric.register_participant(registration), EnsembleError::stale_coordinator_epoch);
  EF_REQUIRE_OK(harness.register_participant(participant));
}

EF_TEST(duplicate_registration_is_rejected_and_a_thief_cannot_take_over) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  EF_REQUIRE_OK(harness.define(spec));
  HarnessParticipant& participant =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants.front(),
                     reference_program("ok").value(), 2002u);
  EF_REQUIRE_OK(harness.register_participant(participant));

  ParticipantRegistration duplicate;
  duplicate.ensemble = EnsembleId(1);
  duplicate.ensemble_generation = EnsembleGeneration(1);
  duplicate.participant = participant.id;
  duplicate.worker = WorkerId(9999);
  duplicate.worker_boot = WorkerBootId(8888);
  duplicate.coordinator_epoch = fabric.epoch();
  duplicate.claimed_roles = participant.roles;
  EF_CHECK_ERR(fabric.register_participant(duplicate), EnsembleError::participant_duplicate);
}

EF_TEST(a_worker_may_not_promote_itself_into_another_role) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  EF_REQUIRE_OK(harness.define(spec));
  HarnessParticipant& participant =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants.front(),
                     reference_program("ok").value(), 2003u);

  ParticipantRegistration registration;
  registration.ensemble = EnsembleId(1);
  registration.ensemble_generation = EnsembleGeneration(1);
  registration.participant = participant.id;
  registration.worker = participant.worker;
  registration.worker_boot = participant.boot;
  registration.coordinator_epoch = fabric.epoch();
  registration.claimed_roles.insert(ParticipantRole::judge);
  EF_CHECK_ERR(fabric.register_participant(registration), EnsembleError::participant_role_mismatch);
}

EF_TEST(incompatible_participants_are_rejected) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  EF_REQUIRE_OK(harness.define(spec));
  HarnessParticipant& participant =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants.front(),
                     reference_program("ok").value(), 2004u);
  participant.profile.capability_mask = 0u;

  ParticipantRegistration registration;
  registration.ensemble = EnsembleId(1);
  registration.ensemble_generation = EnsembleGeneration(1);
  registration.participant = participant.id;
  registration.worker = participant.worker;
  registration.worker_boot = participant.boot;
  registration.coordinator_epoch = fabric.epoch();
  registration.claimed_roles = participant.roles;
  registration.profile = participant.profile;
  registration.profile.protocol_version = 0u;
  EF_CHECK_ERR(fabric.register_participant(registration), EnsembleError::participant_incompatible);
}

EF_TEST(a_stale_boot_cannot_report_a_failure_for_a_live_participant) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  HarnessParticipant& participant =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants.front(),
                     reference_program("ok").value(), 2005u);
  EF_REQUIRE_OK(harness.register_participant(participant));

  ParticipantAuthority stale = harness.authority_of(participant);
  stale.worker_boot = WorkerBootId(stale.worker_boot.value() + 1u);
  EF_CHECK_ERR(fabric.report_participant_failure(stale, ParticipantFailureKind::unavailable, "x"),
               EnsembleError::stale_worker_boot);
}

EF_TEST(a_replacement_participant_advances_the_generation_and_does_not_inherit_evidence) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  EF_REQUIRE_OK(harness.define(spec));
  HarnessParticipant& participant =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants.front(),
                     reference_program("ok").value(), 2006u);
  EF_REQUIRE_OK(harness.register_participant(participant));
  const ParticipantGeneration first = participant.generation;

  // A live incarnation may not be displaced by a second process: that is
  // impersonation, and it is rejected.
  ParticipantRegistration takeover;
  takeover.ensemble = EnsembleId(1);
  takeover.ensemble_generation = EnsembleGeneration(1);
  takeover.participant = participant.id;
  takeover.worker = WorkerId(participant.worker.value() + 1u);
  takeover.worker_boot = WorkerBootId(participant.boot.value() + 500u);
  takeover.coordinator_epoch = fabric.epoch();
  takeover.claimed_roles = participant.roles;
  EF_CHECK_ERR(fabric.register_participant(takeover), EnsembleError::participant_duplicate);

  // Once the incarnation is gone, a replacement is accepted and advances the
  // participant generation so that no earlier evidence is inherited.
  EF_REQUIRE_OK(fabric.report_participant_failure(harness.authority_of(participant),
                                                 ParticipantFailureKind::unavailable, "lost"));
  participant.boot = WorkerBootId(participant.boot.value() + 500u);
  EF_REQUIRE_OK(harness.register_participant(participant));
  EF_CHECK(participant.generation > first);

  Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_CHECK_EQ(snapshot.value().participants.front().replacement_count, 1u);
}

EF_TEST(candidate_output_from_a_previous_generation_is_fenced) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  HarnessParticipant& participant =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants.front(),
                     reference_program("ok").value(), 2007u);
  EF_REQUIRE_OK(harness.register_participant(participant));

  Outcome<std::vector<FabricAction>> actions =
      fabric.advance(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(actions);
  EF_CHECK_EQ(actions.value().size(), 1u);
  const auto* dispatch = std::get_if<DispatchAction>(&actions.value().front());
  EF_CHECK(dispatch != nullptr);
  if (dispatch == nullptr) {
    return;
  }

  ParticipantAuthority authority = harness.authority_of(participant);
  authority.participant_generation = ParticipantGeneration(authority.participant_generation.value() + 1u);
  CandidateSubmission submission;
  submission.authority.participant = authority;
  submission.authority.execution = dispatch->execution.execution;
  submission.authority.attempt_generation = dispatch->execution.attempt_generation;
  submission.authority.candidate = dispatch->candidate;
  submission.authority.candidate_generation = dispatch->candidate_generation;
  submission.payload = {std::byte{0x41}};
  EF_CHECK_ERR(fabric.submit_candidate(submission), EnsembleError::stale_participant_generation);
}

EF_TEST_MAIN
