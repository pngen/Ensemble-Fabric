#include <barrier>
#include <thread>
#include <vector>

#include "support/test_harness.hpp"
#include "support/test_support.hpp"

using namespace ensemble_fabric;

namespace {

EnsembleSpec two_candidate_spec() {
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1), ParticipantId(2)});
  spec.arbitration.factors = {ArbitrationFactor::aggregate_score};
  spec.arbitration.tie_break = TieBreakPolicy::lowest_candidate_id;
  return spec;
}

struct Dispatched {
  EnsembleFabric fabric;
  EnsembleHarness harness{fabric};
  CandidateAuthority first;
  CandidateAuthority second;
  EnsembleExecutionId execution;
  EnsembleAttemptGeneration attempt{EnsembleAttemptGeneration(1)};
};

/// Dispatches both candidates and returns the authority each one must present.
[[nodiscard]] bool dispatch_both(Dispatched& context) {
  EnsembleSpec spec = two_candidate_spec();
  if (!context.harness.define(spec).ok()) {
    return false;
  }
  const Outcome<EnsembleExecutionId> opened =
      context.harness.open(EnsembleId(1), EnsembleGeneration(1));
  if (!opened.ok()) {
    return false;
  }
  context.execution = opened.value();
  HarnessParticipant& first_participant =
      context.harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                             reference_program("ok:value=A").value(), 11001u);
  HarnessParticipant& second_participant =
      context.harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                             reference_program("ok:value=B").value(), 11002u);
  if (!context.harness.register_participant(first_participant).ok() ||
      !context.harness.register_participant(second_participant).ok()) {
    return false;
  }
  const Outcome<std::vector<FabricAction>> actions =
      context.fabric.advance(EnsembleId(1), EnsembleGeneration(1));
  if (!actions.ok() || actions.value().size() != 2u) {
    return false;
  }
  for (const FabricAction& action : actions.value()) {
    const auto* dispatch = std::get_if<DispatchAction>(&action);
    if (dispatch == nullptr) {
      return false;
    }
    CandidateAuthority authority;
    if (dispatch->participant == first_participant.id) {
      authority.participant = context.harness.authority_of(first_participant);
    } else {
      authority.participant = context.harness.authority_of(second_participant);
    }
    authority.execution = dispatch->execution.execution;
    authority.attempt_generation = dispatch->execution.attempt_generation;
    authority.candidate = dispatch->candidate;
    authority.candidate_generation = dispatch->candidate_generation;
    if (dispatch->participant == first_participant.id) {
      context.first = authority;
    } else {
      context.second = authority;
    }
  }
  return true;
}

}

EF_TEST(two_simultaneous_candidate_completions_are_both_admitted) {
  // Legal winner: both submissions are admitted because they target different
  // candidate slots; the loser is not "rejected", it is a different candidate.
  Dispatched context;
  EF_CHECK(dispatch_both(context));
  std::barrier start(3);
  Status first_status;
  Status second_status;
  std::thread first([&]() {
    start.arrive_and_wait();
    CandidateSubmission submission;
    submission.authority = context.first;
    submission.payload = {std::byte{0x41}};
    first_status = context.fabric.submit_candidate(submission);
  });
  std::thread second([&]() {
    start.arrive_and_wait();
    CandidateSubmission submission;
    submission.authority = context.second;
    submission.payload = {std::byte{0x42}};
    second_status = context.fabric.submit_candidate(submission);
  });
  start.arrive_and_wait();
  first.join();
  second.join();
  EF_REQUIRE_OK(first_status);
  EF_REQUIRE_OK(second_status);
  Outcome<EnsembleSnapshot> snapshot = context.fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  for (const CandidateSnapshot& candidate : snapshot.value().candidates) {
    EF_CHECK(candidate.state == CandidateState::valid);
  }
}

EF_TEST(a_duplicate_completion_from_one_slot_is_rejected_exactly_once) {
  // Legal winner: the first admission wins; every later submission for the same
  // candidate generation is rejected with duplicate_completion.
  Dispatched context;
  EF_CHECK(dispatch_both(context));
  constexpr int kThreads = 4;
  std::barrier start(kThreads + 1);
  std::vector<Status> statuses(static_cast<std::size_t>(kThreads));
  std::vector<std::thread> threads;
  for (int index = 0; index < kThreads; ++index) {
    threads.emplace_back([&, index]() {
      start.arrive_and_wait();
      CandidateSubmission submission;
      submission.authority = context.first;
      submission.payload = {std::byte{0x41}};
      statuses[static_cast<std::size_t>(index)] = context.fabric.submit_candidate(submission);
    });
  }
  start.arrive_and_wait();
  for (std::thread& thread : threads) {
    thread.join();
  }
  int accepted = 0;
  int rejected = 0;
  for (const Status& status : statuses) {
    if (status.ok()) {
      ++accepted;
    } else {
      EF_CHECK(status.code() == EnsembleError::duplicate_completion);
      ++rejected;
    }
  }
  EF_CHECK_EQ(accepted, 1);
  EF_CHECK_EQ(rejected, kThreads - 1);
}

EF_TEST(concurrent_finalize_attempts_commit_exactly_one_result) {
  // Legal winner: exactly one of the competing finalize calls commits; the
  // others observe the identical authoritative result or a typed rejection.
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = two_candidate_spec();
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                 reference_program("ok:value=A").value(), 11100u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                 reference_program("ok:value=B").value(), 11101u);
  EF_REQUIRE_OK(harness.register_all());
  EF_REQUIRE_OK(harness.run());

  constexpr int kThreads = 4;
  std::barrier start(kThreads + 1);
  std::vector<Outcome<EnsembleResult>> results;
  results.resize(static_cast<std::size_t>(kThreads), fail<EnsembleResult>(EnsembleError::internal_error, "unset"));
  std::vector<std::thread> threads;
  for (int index = 0; index < kThreads; ++index) {
    threads.emplace_back([&, index]() {
      start.arrive_and_wait();
      results[static_cast<std::size_t>(index)] =
          fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
    });
  }
  start.arrive_and_wait();
  for (std::thread& thread : threads) {
    thread.join();
  }
  for (const Outcome<EnsembleResult>& result : results) {
    EF_REQUIRE_OK(result);
    if (result.ok()) {
      EF_CHECK(result.value().authoritative());
    }
  }
  EF_CHECK_EQ(fabric.counters().commits_accepted, 1u);
}

EF_TEST(cancellation_versus_commit_has_exactly_one_winner) {
  // Legal outcomes: either the commit linearizes first and the result stands
  // and cancellation is rejected, or cancellation linearizes first and the
  // commit is rejected.  Both are legal; both happening, or neither, is not.
  for (int repetition = 0; repetition < 24; ++repetition) {
    EnsembleFabric fabric;
    EnsembleHarness harness(fabric);
    EnsembleSpec spec = two_candidate_spec();
    EF_REQUIRE_OK(harness.define(spec));
    EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
    harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                   reference_program("ok:value=A").value(), 11200u + static_cast<std::uint64_t>(repetition) * 2u);
    harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                   reference_program("ok:value=B").value(), 11201u + static_cast<std::uint64_t>(repetition) * 2u);
    EF_REQUIRE_OK(harness.register_all());
    EF_REQUIRE_OK(harness.run());

    std::barrier start(3);
    Outcome<EnsembleResult> committed = fail<EnsembleResult>(EnsembleError::internal_error, "unset");
    Status cancelled;
    std::thread committer([&]() {
      start.arrive_and_wait();
      committed = fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
    });
    std::thread canceller([&]() {
      start.arrive_and_wait();
      cancelled = fabric.cancel_ensemble(EnsembleId(1), EnsembleGeneration(1), "race");
    });
    start.arrive_and_wait();
    committer.join();
    canceller.join();

    Outcome<EnsembleResult> stored = fabric.current_result(EnsembleId(1));
    if (committed.ok()) {
      EF_CHECK(cancelled.code() == EnsembleError::result_already_committed);
      EF_CHECK(stored.ok());
      EF_CHECK(stored.ok() && stored.value().authoritative());
      EF_CHECK_EQ(stored.ok() ? stored.value().commit_fingerprint : 0u,
                  committed.ok() ? committed.value().commit_fingerprint : 0u);
    } else {
      EF_REQUIRE_OK(cancelled);
      EF_CHECK(committed.code() == EnsembleError::ensemble_cancelled);
      EF_CHECK(!stored.ok());
      EF_CHECK(stored.code() == EnsembleError::not_found ||
               stored.code() == EnsembleError::ensemble_cancelled);
    }
  }
}

EF_TEST(quorum_formation_versus_cancellation_never_publishes_success) {
  for (int repetition = 0; repetition < 24; ++repetition) {
    EnsembleFabric fabric;
    EnsembleHarness harness(fabric);
    EnsembleSpec spec = two_candidate_spec();
    spec.quorum.consensus_threshold_percent = 60u;
    EF_REQUIRE_OK(harness.define(spec));
    EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
    harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                   reference_program("ok:value=A").value(), 11400u + static_cast<std::uint64_t>(repetition) * 2u);
    harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                   reference_program("ok:value=B").value(), 11401u + static_cast<std::uint64_t>(repetition) * 2u);
    EF_REQUIRE_OK(harness.register_all());
    EF_REQUIRE_OK(harness.run());

    std::barrier start(3);
    Status cancelled;
    Outcome<EnsembleResult> finalized = fail<EnsembleResult>(EnsembleError::internal_error, "unset");
    std::thread finalizer([&]() {
      start.arrive_and_wait();
      finalized = fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
    });
    std::thread canceller([&]() {
      start.arrive_and_wait();
      cancelled = fabric.cancel_ensemble(EnsembleId(1), EnsembleGeneration(1), "race");
    });
    start.arrive_and_wait();
    finalizer.join();
    canceller.join();

    Outcome<EnsembleResult> stored = fabric.current_result(EnsembleId(1));
    if (cancelled.ok() && finalized.code() == EnsembleError::ensemble_cancelled) {
      EF_CHECK(!stored.ok());
    } else {
      EF_CHECK(stored.ok());
      EF_CHECK(stored.ok() && stored.value().authoritative());
    }
  }
}

EF_TEST(a_stale_worker_completion_cannot_race_a_fresh_retry) {
  // Legal winner: only the current participant generation may publish; the
  // stale incarnation is rejected with a typed authority failure.
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = two_candidate_spec();
  spec.retry.max_attempts = 2u;
  spec.retry.retry_on_unavailable = true;
  spec.retry.replace_participant = true;
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  HarnessParticipant& first_participant =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                     reference_program("ok:value=A").value(), 11600u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                 reference_program("ok:value=B").value(), 11601u);
  EF_REQUIRE_OK(harness.register_all());

  Outcome<std::vector<FabricAction>> actions =
      fabric.advance(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(actions);
  CandidateAuthority stale;
  for (const FabricAction& action : actions.value()) {
    const auto* dispatch = std::get_if<DispatchAction>(&action);
    if (dispatch != nullptr && dispatch->participant == first_participant.id) {
      stale.participant = harness.authority_of(first_participant);
      stale.execution = dispatch->execution.execution;
      stale.attempt_generation = dispatch->execution.attempt_generation;
      stale.candidate = dispatch->candidate;
      stale.candidate_generation = dispatch->candidate_generation;
    }
  }
  EF_CHECK(stale.candidate.valid());
  EF_REQUIRE_OK(fabric.report_participant_failure(harness.authority_of(first_participant),
                                                ParticipantFailureKind::unavailable, "dead"));

  std::barrier start(3);
  Status stale_status;
  Status replacement_status;
  std::thread stale_thread([&]() {
    start.arrive_and_wait();
    CandidateSubmission submission;
    submission.authority = stale;
    submission.payload = {std::byte{0x7F}};
    stale_status = fabric.submit_candidate(submission);
  });
  std::thread replacement_thread([&]() {
    start.arrive_and_wait();
    first_participant.boot = WorkerBootId(first_participant.boot.value() + 777u);
    replacement_status = harness.register_participant(first_participant);
  });
  start.arrive_and_wait();
  stale_thread.join();
  replacement_thread.join();
  EF_REQUIRE_OK(replacement_status);
  EF_CHECK(!stale_status.ok());
  EF_CHECK(stale_status.code() == EnsembleError::stale_participant_generation ||
           stale_status.code() == EnsembleError::stale_worker_boot ||
           stale_status.code() == EnsembleError::participant_not_authoritative ||
           stale_status.code() == EnsembleError::stale_candidate_generation);
}

EF_TEST(supersession_racing_a_commit_has_exactly_one_winner) {
  for (int repetition = 0; repetition < 16; ++repetition) {
    EnsembleFabric fabric;
    EnsembleHarness harness(fabric);
    EnsembleSpec spec = two_candidate_spec();
    EF_REQUIRE_OK(harness.define(spec));
    EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
    harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                   reference_program("ok:value=A").value(), 11800u + static_cast<std::uint64_t>(repetition) * 2u);
    harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                   reference_program("ok:value=B").value(), 11801u + static_cast<std::uint64_t>(repetition) * 2u);
    EF_REQUIRE_OK(harness.register_all());
    EF_REQUIRE_OK(harness.run());

    std::barrier start(3);
    Outcome<EnsembleResult> committed = fail<EnsembleResult>(EnsembleError::internal_error, "unset");
    Status superseded;
    std::thread committer([&]() {
      start.arrive_and_wait();
      committed = fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
    });
    std::thread superseder([&]() {
      start.arrive_and_wait();
      superseded = fabric.supersede_ensemble(EnsembleId(1), EnsembleGeneration(1));
    });
    start.arrive_and_wait();
    committer.join();
    superseder.join();
    EF_REQUIRE_OK(superseded);
    if (committed.ok()) {
      Outcome<EnsembleResult> stored = fabric.current_result(EnsembleId(1));
      EF_CHECK(stored.ok());
      EF_CHECK(stored.ok() && stored.value().authoritative());
    } else {
      EF_CHECK(committed.code() == EnsembleError::ensemble_superseded);
    }
  }
}

EF_TEST_MAIN
