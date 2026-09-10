#include <filesystem>

#include "support/test_harness.hpp"
#include "support/test_support.hpp"

using namespace ensemble_fabric;

namespace {

EnsembleSpec recovery_spec() {
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1), ParticipantId(2)});
  spec.participants.push_back(judge_participant(ParticipantId(100)));
  spec.evidence.min_evaluations_per_candidate = 1u;
  spec.quorum.min_evaluations = 1u;
  return spec;
}

}

EF_TEST(a_restart_advances_the_epoch_and_fences_live_authority) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = recovery_spec();
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  HarnessParticipant& participant =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                     reference_program("gate:gate=never").value(), 13001u);
  EF_REQUIRE_OK(harness.register_participant(participant));
  // Dispatch without executing: the candidate is genuinely in flight.
  EF_REQUIRE_OK(harness.declare_only());

  const CoordinatorEpoch before = fabric.epoch();
  ParticipantAuthority old_epoch_authority = harness.authority_of(participant);
  Outcome<CoordinatorEpoch> after = fabric.restart("coordinator restart");
  EF_REQUIRE_OK(after);
  EF_CHECK(after.ok() && after.value().value() > before.value());

  Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_CHECK(snapshot.ok() && snapshot.value().participants.front().status ==
                                ParticipantStatus::revalidation_required);
  EF_CHECK(snapshot.ok() && !snapshot.value().participants.front().authoritative);

  // Traffic from the previous epoch is rejected outright.
  CandidateSubmission submission;
  submission.authority.participant = old_epoch_authority;
  submission.authority.execution = snapshot.ok() ? snapshot.value().execution
                                                 : EnsembleExecutionId(1);
  submission.authority.attempt_generation = EnsembleAttemptGeneration(1);
  submission.authority.candidate = CandidateId(1);
  submission.authority.candidate_generation = CandidateGeneration(1);
  submission.payload = {std::byte{0x41}};
  EF_CHECK_ERR(fabric.submit_candidate(submission), EnsembleError::stale_coordinator_epoch);
}

EF_TEST(recovered_in_flight_work_requires_revalidation_and_cannot_commit) {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "ensemble_fabric_tests";
  std::filesystem::create_directories(directory);
  const std::filesystem::path path = directory / "recovery.state";
  std::filesystem::remove(path);

  {
    EnsembleFabric fabric;
    EnsembleHarness harness(fabric);
    EnsembleSpec spec = recovery_spec();
    EF_REQUIRE_OK(harness.define(spec));
    EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
    HarnessParticipant& participant =
        harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                       reference_program("gate:gate=never").value(), 13002u);
    EF_REQUIRE_OK(harness.register_participant(participant));
    // Dispatch without executing: the candidate is genuinely in flight.
  EF_REQUIRE_OK(harness.declare_only());
    EF_REQUIRE_OK(fabric.save_state(path.string()));
  }

  EnsembleFabric recovered;
  EF_REQUIRE_OK(recovered.recover_state(path.string()));
  Outcome<EnsembleSnapshot> snapshot = recovered.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_CHECK(snapshot.ok() && snapshot.value().execution_status ==
                                ExecutionStatus::revalidation_required);
  EF_CHECK_ERR(recovered.finalize(EnsembleId(1), EnsembleGeneration(1)),
               EnsembleError::revalidation_required);
  std::filesystem::remove(path);
}

EF_TEST(committed_results_survive_a_restart_unchanged) {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "ensemble_fabric_tests";
  std::filesystem::create_directories(directory);
  const std::filesystem::path path = directory / "committed.state";
  std::filesystem::remove(path);

  std::uint64_t fingerprint = 0;
  std::uint64_t payload_digest = 0;
  {
    EnsembleFabric fabric;
    EnsembleHarness harness(fabric);
    EnsembleSpec spec = recovery_spec();
    EF_REQUIRE_OK(harness.define(spec));
    EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
    harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                   reference_program("ok:value=durable").value(), 13003u);
    harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                   reference_program("ok:value=other").value(), 13004u);
    harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[2],
                   reference_program("evaluate:judgment=accept,score=0.9;"
                                     "evaluate:judgment=reject,score=0.1")
                       .value(),
                   13005u);
    EF_REQUIRE_OK(harness.register_all());
    EF_REQUIRE_OK(harness.run());
    Outcome<EnsembleResult> result = fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
    EF_REQUIRE_OK(result);
    if (result.ok()) {
      fingerprint = result.value().commit_fingerprint;
      payload_digest = result.value().payload_digest;
    }
    EF_REQUIRE_OK(fabric.save_state(path.string()));
  }

  EnsembleFabric recovered;
  EF_REQUIRE_OK(recovered.recover_state(path.string()));
  Outcome<EnsembleResult> result = recovered.current_result(EnsembleId(1));
  EF_REQUIRE_OK(result);
  EF_CHECK(result.ok() && result.value().authoritative());
  EF_CHECK_EQ(result.value().commit_fingerprint, fingerprint);
  EF_CHECK_EQ(result.value().payload_digest, payload_digest);

  // A restarted coordinator refuses to replace an already committed result.
  EF_CHECK_ERR(recovered.cancel_ensemble(EnsembleId(1), EnsembleGeneration(1), "after restart"),
               EnsembleError::result_already_committed);
  std::filesystem::remove(path);
}

EF_TEST(a_restarted_coordinator_rejects_a_stale_epoch_registration) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = recovery_spec();
  EF_REQUIRE_OK(harness.define(spec));
  HarnessParticipant& participant =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                     reference_program("ok").value(), 13006u);
  const CoordinatorEpoch stale = fabric.epoch();
  EF_REQUIRE_OK(fabric.restart("epoch advance"));

  ParticipantRegistration registration;
  registration.ensemble = EnsembleId(1);
  registration.ensemble_generation = EnsembleGeneration(1);
  registration.participant = participant.id;
  registration.worker = participant.worker;
  registration.worker_boot = participant.boot;
  registration.coordinator_epoch = stale;
  registration.claimed_roles = participant.roles;
  EF_CHECK_ERR(fabric.register_participant(registration), EnsembleError::stale_coordinator_epoch);

  registration.coordinator_epoch = fabric.epoch();
  EF_REQUIRE_OK(fabric.register_participant(registration));
  Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_CHECK(snapshot.ok() &&
           snapshot.value().participants.front().status == ParticipantStatus::current);
  EF_CHECK(snapshot.ok() && snapshot.value().participants.front().authoritative);
}

EF_TEST(recovery_reestablishes_identity_allocators) {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "ensemble_fabric_tests";
  std::filesystem::create_directories(directory);
  const std::filesystem::path path = directory / "allocators.state";
  std::filesystem::remove(path);
  {
    EnsembleFabric fabric;
    EnsembleHarness harness(fabric);
    EnsembleSpec spec = recovery_spec();
    EF_REQUIRE_OK(harness.define(spec));
    EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
    harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                   reference_program("ok:value=A").value(), 13007u);
    EF_REQUIRE_OK(harness.register_all());
    EF_REQUIRE_OK(harness.run());
    EF_REQUIRE_OK(fabric.save_state(path.string()));
  }
  EnsembleFabric recovered;
  EF_REQUIRE_OK(recovered.recover_state(path.string()));
  Outcome<EnsembleSnapshot> snapshot = recovered.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_CHECK(snapshot.ok() && !snapshot.value().candidates.empty());
  // Redefining the ensemble in the recovered runtime must not reuse identities.
  EnsembleSpec spec = recovery_spec();
  Outcome<EnsembleGeneration> generation = recovered.define_ensemble(spec);
  EF_REQUIRE_OK(generation);
  EF_CHECK(generation.ok() && generation.value().value() == 2u);
  std::filesystem::remove(path);
}

EF_TEST_MAIN
