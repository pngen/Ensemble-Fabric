#include <filesystem>
#include <fstream>

#include "support/test_harness.hpp"
#include "support/test_support.hpp"

using namespace ensemble_fabric;

namespace {

struct Attack {
  EnsembleFabric fabric;
  EnsembleHarness harness{fabric};
  CandidateAuthority authority;
  bool ready{false};

  Attack(const std::string& script = "ok:value=legit") {
    EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
    spec.evidence.min_candidate_payload_bytes = 1u;
    (void)harness.define(spec);
    (void)harness.open(EnsembleId(1), EnsembleGeneration(1));
    HarnessParticipant& participant =
        harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                       reference_program(script).value(), 12001u);
    (void)harness.register_participant(participant);
    Outcome<std::vector<FabricAction>> actions =
        fabric.advance(EnsembleId(1), EnsembleGeneration(1));
    if (!actions.ok() || actions.value().empty()) {
      return;
    }
    const auto* dispatch = std::get_if<DispatchAction>(&actions.value().front());
    if (dispatch == nullptr) {
      return;
    }
    authority.participant = harness.authority_of(participant);
    authority.execution = dispatch->execution.execution;
    authority.attempt_generation = dispatch->execution.attempt_generation;
    authority.candidate = dispatch->candidate;
    authority.candidate_generation = dispatch->candidate_generation;
    ready = true;
  }

  [[nodiscard]] CandidateSubmission submission() const {
    CandidateSubmission value;
    value.authority = authority;
    value.payload = {std::byte{0x4F}, std::byte{0x4B}};
    return value;
  }
};

}

EF_TEST(a_duplicate_candidate_result_is_rejected) {
  Attack attack;
  EF_CHECK(attack.ready);
  if (!attack.ready) return;
  EF_REQUIRE_OK(attack.fabric.submit_candidate(attack.submission()));
  EF_CHECK_ERR(attack.fabric.submit_candidate(attack.submission()),
               EnsembleError::duplicate_completion);
  EF_CHECK_EQ(attack.fabric.counters().duplicate_submissions_suppressed, 1u);
}

EF_TEST(a_conflicting_candidate_result_for_the_same_generation_is_rejected) {
  Attack attack;
  EF_CHECK(attack.ready);
  if (!attack.ready) return;
  CandidateSubmission first = attack.submission();
  first.payload = {std::byte{0x01}};
  EF_REQUIRE_OK(attack.fabric.submit_candidate(first));
  CandidateSubmission conflicting = attack.submission();
  conflicting.payload = {std::byte{0x02}};
  EF_CHECK_ERR(attack.fabric.submit_candidate(conflicting),
               EnsembleError::duplicate_completion);
}

EF_TEST(a_stale_candidate_generation_cannot_substitute_content) {
  Attack attack;
  EF_CHECK(attack.ready);
  if (!attack.ready) return;
  CandidateSubmission forged = attack.submission();
  forged.authority.candidate_generation = CandidateGeneration(7);
  EF_CHECK_ERR(attack.fabric.submit_candidate(forged),
               EnsembleError::stale_candidate_generation);
}

EF_TEST(a_forged_worker_boot_is_rejected) {
  Attack attack;
  EF_CHECK(attack.ready);
  if (!attack.ready) return;
  CandidateSubmission forged = attack.submission();
  forged.authority.participant.worker_boot = WorkerBootId(4242);
  EF_CHECK_ERR(attack.fabric.submit_candidate(forged), EnsembleError::stale_worker_boot);
}

EF_TEST(a_reused_coordinator_epoch_from_the_past_is_rejected) {
  Attack attack;
  EF_CHECK(attack.ready);
  if (!attack.ready) return;
  CandidateSubmission forged = attack.submission();
  forged.authority.participant.coordinator_epoch = CoordinatorEpoch(1);
  EF_CHECK(attack.fabric.epoch().value() == 1u);
  EF_REQUIRE_OK(attack.fabric.submit_candidate(forged));
  // After a restart the same submission is fenced by the new epoch.
  EF_REQUIRE_OK(attack.fabric.restart("adversarial restart"));
  EF_CHECK_ERR(attack.fabric.submit_candidate(attack.submission()),
               EnsembleError::stale_coordinator_epoch);
}

EF_TEST(an_oversized_candidate_payload_is_rejected) {
  Attack attack;
  EF_CHECK(attack.ready);
  if (!attack.ready) return;
  CandidateSubmission oversized = attack.submission();
  Limits limits = Limits::defaults();
  oversized.payload.assign(limits.max_candidate_payload_bytes + 1u, std::byte{0x41});
  EF_CHECK_ERR(attack.fabric.submit_candidate(oversized),
               EnsembleError::resource_limit_exceeded);
}

EF_TEST(an_empty_payload_is_invalid_content_not_a_valid_candidate) {
  Attack attack;
  EF_CHECK(attack.ready);
  if (!attack.ready) return;
  CandidateSubmission empty = attack.submission();
  empty.payload.clear();
  EF_REQUIRE_OK(attack.fabric.submit_candidate(empty));
  Outcome<EnsembleSnapshot> snapshot = attack.fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_CHECK(snapshot.value().candidates.front().state == CandidateState::invalid);
  EF_CHECK(snapshot.value().candidates.front().failure == EnsembleError::malformed_candidate);
}

EF_TEST(a_duplicate_evaluation_is_rejected_and_cannot_inflate_votes) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  spec.participants.push_back(judge_participant(ParticipantId(100)));
  spec.evidence.min_evaluations_per_candidate = 1u;
  spec.quorum.min_evaluations = 1u;
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                 reference_program("ok:value=A").value(), 12100u);
  HarnessParticipant& judge =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                     reference_program("evaluate:judgment=accept,score=1").value(), 12101u);
  EF_REQUIRE_OK(harness.register_all());
  EF_REQUIRE_OK(harness.step());
  EF_REQUIRE_OK(harness.step());

  Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_CHECK_EQ(snapshot.value().evaluations.size(), 1u);
  if (!snapshot.ok() || snapshot.value().evaluations.empty()) {
    return;
  }
  const EvaluationSnapshot evaluation = snapshot.value().evaluations.front();
  EvaluationSubmission duplicate;
  duplicate.authority.evaluator = harness.authority_of(judge);
  duplicate.authority.execution = evaluation.execution;
  duplicate.authority.attempt_generation = evaluation.attempt_generation;
  duplicate.authority.evaluation = evaluation.id;
  duplicate.authority.evaluation_generation = evaluation.generation;
  duplicate.authority.candidate = evaluation.candidate;
  duplicate.authority.candidate_generation = evaluation.candidate_generation;
  duplicate.criterion = evaluation.criterion;
  duplicate.kind = evaluation.kind;
  duplicate.judgment = evaluation.judgment;
  duplicate.score_defined = true;
  duplicate.score = 1.0;
  duplicate.weight = 1.0;
  EF_CHECK_ERR(fabric.submit_evaluation(duplicate), EnsembleError::duplicate_evaluation);
  EF_CHECK(fabric.counters().duplicate_votes_suppressed >= 1u);
}

EF_TEST(a_judge_cannot_evaluate_a_candidate_for_a_different_generation) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  spec.participants.push_back(judge_participant(ParticipantId(100)));
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                 reference_program("ok:value=A").value(), 12200u);
  HarnessParticipant& judge =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                     reference_program("evaluate:judgment=accept,score=1").value(), 12201u);
  EF_REQUIRE_OK(harness.register_all());
  EF_REQUIRE_OK(harness.step());
  EF_REQUIRE_OK(harness.step());

  Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  if (!snapshot.ok() || snapshot.value().evaluations.empty()) {
    return;
  }
  const EvaluationSnapshot evaluation = snapshot.value().evaluations.front();
  EvaluationSubmission forged;
  forged.authority.evaluator = harness.authority_of(judge);
  forged.authority.execution = evaluation.execution;
  forged.authority.attempt_generation = evaluation.attempt_generation;
  forged.authority.evaluation = EvaluationId(evaluation.id.value() + 1000u);
  forged.authority.evaluation_generation = evaluation.generation;
  forged.authority.candidate = evaluation.candidate;
  forged.authority.candidate_generation = CandidateGeneration(evaluation.candidate_generation.value() + 5u);
  forged.criterion = evaluation.criterion;
  forged.kind = evaluation.kind;
  forged.judgment = CategoricalJudgment::accept;
  EF_CHECK_ERR(fabric.submit_evaluation(forged), EnsembleError::stale_candidate_generation);
}

/// Prepares a planned but unanswered evaluation so that content validation can
/// be exercised independently of identity validation.
struct PlannedEvaluation {
  EnsembleFabric fabric;
  EnsembleHarness harness{fabric};
  HarnessParticipant* judge{nullptr};
  EvaluationAction action;

  [[nodiscard]] bool run() {
    EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
    spec.participants.push_back(judge_participant(ParticipantId(100)));
    spec.evidence.min_evaluations_per_candidate = 1u;
    if (!harness.define(spec).ok() || !harness.open(EnsembleId(1), EnsembleGeneration(1)).ok()) {
      return false;
    }
    harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                   reference_program("ok:value=A").value(), 12300u);
    judge = &harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                            reference_program("evaluate:judgment=accept,score=1").value(), 12301u);
    if (!harness.register_all().ok()) {
      return false;
    }
    // Dispatch and complete the candidate without performing the evaluation.
    const Outcome<std::vector<FabricAction>> first =
        fabric.advance(EnsembleId(1), EnsembleGeneration(1));
    if (!first.ok() || first.value().empty()) {
      return false;
    }
    const auto* dispatch = std::get_if<DispatchAction>(&first.value().front());
    if (dispatch == nullptr) {
      return false;
    }
    CandidateSubmission submission;
    submission.authority.participant = harness.authority_of(*harness.find(ParticipantId(1)));
    submission.authority.execution = dispatch->execution.execution;
    submission.authority.attempt_generation = dispatch->execution.attempt_generation;
    submission.authority.candidate = dispatch->candidate;
    submission.authority.candidate_generation = dispatch->candidate_generation;
    submission.payload = {std::byte{0x41}};
    if (!fabric.submit_candidate(submission).ok()) {
      return false;
    }
    const Outcome<std::vector<FabricAction>> second =
        fabric.advance(EnsembleId(1), EnsembleGeneration(1));
    if (!second.ok() || second.value().empty()) {
      return false;
    }
    for (const FabricAction& planned_action : second.value()) {
      if (const auto* evaluation = std::get_if<EvaluationAction>(&planned_action)) {
        action = *evaluation;
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] EvaluationSubmission submission() const {
    EvaluationSubmission value;
    value.authority.evaluator = harness.authority_of(*judge);
    value.authority.execution = action.execution.execution;
    value.authority.attempt_generation = action.execution.attempt_generation;
    value.authority.evaluation = action.evaluation;
    value.authority.evaluation_generation = action.evaluation_generation;
    value.authority.candidate = action.candidate;
    value.authority.candidate_generation = action.candidate_generation;
    value.criterion = action.criterion;
    value.kind = action.criterion_kind;
    value.judgment = CategoricalJudgment::accept;
    return value;
  }
};

EF_TEST(a_non_finite_score_is_rejected) {
  PlannedEvaluation planned;
  EF_REQUIRE(planned.run());
  EvaluationSubmission forged = planned.submission();
  forged.score_defined = true;
  forged.score = std::numeric_limits<double>::quiet_NaN();
  EF_CHECK_ERR(planned.fabric.submit_evaluation(forged), EnsembleError::malformed_evaluation);

  EvaluationSubmission negative_weight = planned.submission();
  negative_weight.weight = -1.0;
  EF_CHECK_ERR(planned.fabric.submit_evaluation(negative_weight),
               EnsembleError::malformed_evaluation);

  EvaluationSubmission valid = planned.submission();
  valid.score_defined = true;
  valid.score = 0.5;
  EF_REQUIRE_OK(planned.fabric.submit_evaluation(valid));
}

EF_TEST(an_impossible_lifecycle_transition_is_rejected) {
  Attack attack;
  EF_CHECK(attack.ready);
  if (!attack.ready) return;
  CandidateSubmission output = attack.submission();
  EF_REQUIRE_OK(attack.fabric.submit_candidate(output));
  // The candidate is valid; a further failure report for a non-dispatched state
  // must be rejected rather than silently applied.
  CandidateFailureSubmission failure;
  failure.authority = attack.authority;
  failure.participant_failure = static_cast<std::uint32_t>(ParticipantFailureKind::internal_error);
  EF_CHECK_ERR(attack.fabric.submit_candidate_failure(failure),
               EnsembleError::duplicate_completion);
}

EF_TEST(a_participant_cannot_submit_another_participants_candidate) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1), ParticipantId(2)});
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  HarnessParticipant& first =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                     reference_program("ok:value=A").value(), 12400u);
  HarnessParticipant& second =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                     reference_program("ok:value=B").value(), 12401u);
  EF_REQUIRE_OK(harness.register_all());
  Outcome<std::vector<FabricAction>> actions =
      fabric.advance(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(actions);

  CandidateAuthority forged;
  for (const FabricAction& action : actions.value()) {
    const auto* dispatch = std::get_if<DispatchAction>(&action);
    if (dispatch != nullptr && dispatch->participant == second.id) {
      forged.participant = harness.authority_of(first);
      forged.execution = dispatch->execution.execution;
      forged.attempt_generation = dispatch->execution.attempt_generation;
      forged.candidate = dispatch->candidate;
      forged.candidate_generation = dispatch->candidate_generation;
    }
  }
  CandidateSubmission submission;
  submission.authority = forged;
  submission.payload = {std::byte{0x41}};
  EF_CHECK_ERR(fabric.submit_candidate(submission), EnsembleError::participant_role_mismatch);
}

EF_TEST(a_corrupted_state_file_cannot_be_smuggled_past_recovery) {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "ensemble_fabric_tests";
  std::filesystem::create_directories(directory);
  const std::filesystem::path path = directory / "adversarial.state";
  std::filesystem::remove(path);
  EnsembleFabric fabric;
  EF_REQUIRE_OK(fabric.save_state(path.string()));
  {
    std::ofstream stream(path, std::ios::binary | std::ios::app);
    stream.write("garbage", 7);
  }
  EnsembleFabric recovered;
  EF_CHECK_ERR(recovered.recover_state(path.string()), EnsembleError::trailing_data);
  std::filesystem::remove(path);
}

EF_TEST(repeated_start_stop_of_the_runtime_is_stable) {
  for (int iteration = 0; iteration < 8; ++iteration) {
    EnsembleFabric fabric;
    EnsembleHarness harness(fabric);
    EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
    EF_REQUIRE_OK(harness.define(spec));
    EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
    HarnessParticipant& participant =
        harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                       reference_program("ok:value=x").value(),
                       12500u + static_cast<std::uint64_t>(iteration));
    EF_REQUIRE_OK(harness.register_participant(participant));
    EF_REQUIRE_OK(harness.run());
    EF_REQUIRE_OK(fabric.finalize(EnsembleId(1), EnsembleGeneration(1)));
    Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
    EF_REQUIRE_OK(snapshot);
  }
}

EF_TEST(concurrent_queries_against_mutations_stay_consistent) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1), ParticipantId(2)});
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                 reference_program("ok:value=A").value(), 12600u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                 reference_program("ok:value=B").value(), 12601u);
  EF_REQUIRE_OK(harness.register_all());

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> queries{0};
  std::vector<std::thread> readers;
  for (int index = 0; index < 3; ++index) {
    readers.emplace_back([&fabric, &stop, &queries]() {
      while (!stop.load()) {
        Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
        if (snapshot.ok()) {
          queries.fetch_add(1u);
        }
      }
    });
  }
  // Wait until at least one reader has actually observed state, then mutate.
  for (std::uint64_t attempt = 0; attempt < 1000000u && queries.load() == 0u; ++attempt) {
    std::this_thread::yield();
  }
  EF_REQUIRE_OK(harness.run());
  EF_REQUIRE_OK(fabric.finalize(EnsembleId(1), EnsembleGeneration(1)));
  stop.store(true);
  for (std::thread& reader : readers) {
    reader.join();
  }
  EF_CHECK(queries.load() > 0u);
  Outcome<EnsembleResult> result = fabric.current_result(EnsembleId(1));
  EF_REQUIRE_OK(result);
  EF_CHECK(result.ok() && result.value().authoritative());
}

EF_TEST_MAIN
