#include <random>

#include "support/test_harness.hpp"
#include "support/test_support.hpp"

using namespace ensemble_fabric;

namespace {

struct Random {
  std::mt19937_64 engine;
  explicit Random(std::uint64_t seed) : engine(seed) {}
  [[nodiscard]] std::uint64_t next(std::uint64_t bound) { return engine() % bound; }
  [[nodiscard]] bool chance(std::uint32_t percent) { return next(100u) < percent; }
};

std::string script_for(Random& random, std::string value) {
  const std::uint32_t roll = static_cast<std::uint32_t>(random.next(100u));
  if (roll < 55u) {
    return "ok:value=" + std::move(value);
  }
  if (roll < 70u) {
    return "fail:kind=internal_error";
  }
  if (roll < 80u) {
    return "abstain:rationale=property";
  }
  if (roll < 90u) {
    return "malformed:bytes=2;ok:value=" + std::move(value);
  }
  return "fail:kind=unavailable";
}

}

EF_TEST(seeded_randomized_sequences_preserve_the_core_invariants) {
  const std::uint64_t base_seed = 0x5EED1234u;
  for (std::uint32_t iteration = 0; iteration < 120u; ++iteration) {
    const std::uint64_t seed = base_seed + iteration;
    Random random(seed);
    const std::uint32_t candidate_count = 2u + static_cast<std::uint32_t>(random.next(4u));
    const bool with_judges = random.chance(50u);
    const bool with_verifier = random.chance(30u);

    EnsembleFabric fabric;
    EnsembleHarness harness(fabric);
    std::vector<ParticipantId> candidates;
    EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
    spec.participants.clear();
    spec.stages.clear();
    spec.retry.max_attempts = 1u + static_cast<std::uint32_t>(random.next(3u));
    spec.retry.retry_on_malformed_candidate = true;
    spec.retry.retry_on_unavailable = true;

    for (std::uint32_t index = 0; index < candidate_count; ++index) {
      const ParticipantId id(index + 1u);
      candidates.push_back(id);
      spec.participants.push_back(candidate_participant(id));
    }
    spec.stages.push_back(parallel_stage(StageId(1), candidates));
    if (with_judges) {
      spec.participants.push_back(judge_participant(ParticipantId(100)));
      spec.evidence.min_evaluations_per_candidate = 1u;
      spec.quorum.min_evaluations = 1u;
    }
    if (with_verifier) {
      spec.participants.push_back(verifier_participant(ParticipantId(200)));
      spec.evidence.mandatory_criteria.push_back(CriterionRef{9u, 1u});
      spec.aggregation.require_verification_pass = true;
    }
    if (random.chance(40u)) {
      spec.quorum.min_participants = 1u + static_cast<std::uint32_t>(random.next(3u));
    }
    if (random.chance(30u)) {
      spec.quorum.consensus_threshold_percent = 20u + static_cast<std::uint32_t>(random.next(70u));
    }
    spec.arbitration.factors = {ArbitrationFactor::judge_acceptance_ratio,
                                ArbitrationFactor::aggregate_score};
    spec.arbitration.tie_break = random.chance(50u) ? TieBreakPolicy::lowest_candidate_id
                                                    : TieBreakPolicy::report_tie;
    if (!harness.define(spec).ok()) {
      EF_CHECK(false);
      return;
    }
    Outcome<EnsembleExecutionId> execution = harness.open(EnsembleId(1), EnsembleGeneration(1));
    EF_REQUIRE_OK(execution);
    if (!execution.ok()) {
      return;
    }
    std::uint64_t worker_serial = 9000u;
    for (const ParticipantId id : candidates) {
      const ParticipantSpec* declaration = find_participant(spec, id);
      harness.attach(EnsembleId(1), EnsembleGeneration(1), *declaration,
                     reference_program(script_for(random, "c" + id.to_string())).value(),
                     worker_serial++);
    }
    if (with_judges) {
      const ParticipantSpec* declaration = find_participant(spec, ParticipantId(100));
      harness.attach(EnsembleId(1), EnsembleGeneration(1), *declaration,
                     reference_program(random.chance(50u)
                                                  ? "evaluate:judgment=accept,score=0.6"
                                                  : "evaluate:judgment=reject,score=0.1")
                          .value(),
                     worker_serial++);
    }
    if (with_verifier) {
      const ParticipantSpec* declaration = find_participant(spec, ParticipantId(200));
      harness.attach(EnsembleId(1), EnsembleGeneration(1), *declaration,
                     reference_program(random.chance(50u)
                                                  ? "evaluate:verification=pass,score=0.5"
                                                  : "evaluate:verification=fail,score=0.5")
                          .value(),
                     worker_serial++);
    }
    EF_REQUIRE_OK(harness.register_all());

    // Randomly kill one participant before running, to exercise partial loss.
    if (random.chance(35u) && harness.participants().size() > 1u) {
      HarnessParticipant& victim = harness.participants().front();
      EF_REQUIRE_OK(fabric.report_participant_failure(harness.authority_of(victim),
                                                    ParticipantFailureKind::unavailable,
                                                    "property test death"));
      victim.killed = true;
    }
    EF_REQUIRE_OK(harness.run());

    Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
    EF_REQUIRE_OK(snapshot);
    if (!snapshot.ok()) {
      return;
    }
    // Invariant: a hard-invalid candidate is never selected, and a committed
    // selection is always a candidate that held content.
    Outcome<EnsembleResult> result = fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
    if (result.ok()) {
      for (const CandidateSnapshot& candidate : result.value().candidates) {
        if (candidate.id == result.value().selected_candidate) {
          EF_CHECK(is_content_state(candidate.state));
        }
      }
      EF_CHECK_EQ(result.value().ensemble_generation, EnsembleGeneration(1));
      EF_CHECK_EQ(result.value().coordinator_epoch, fabric.epoch());
      // Invariant: at most one commit record per execution generation.
      EF_CHECK_EQ(snapshot.value().commits.size() <= 1u, true);
      Outcome<EnsembleResult> repeated = fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
      EF_REQUIRE_OK(repeated);
      if (repeated.ok()) {
        EF_CHECK_EQ(repeated.value().id.value(), result.value().id.value());
        EF_CHECK_EQ(repeated.value().commit_fingerprint, result.value().commit_fingerprint);
      }
      EF_CHECK_EQ(fabric.counters().commits_accepted, 1u);
    } else {
      // A terminal non-success decision must still be exactly one typed outcome
      // and must never be reported as a committed result.
      Outcome<EnsembleResult> current = fabric.current_result(EnsembleId(1));
      if (current.ok()) {
        EF_CHECK(!current.value().authoritative());
      }
      EF_CHECK(result.code() != EnsembleError::ok);
    }
  }
}

EF_TEST(quorum_is_never_satisfied_by_duplicate_identities) {
  QuorumPolicy policy;
  policy.min_participants = 2u;
  policy.vote_basis = VoteBasis::contributing_participants;
  std::vector<QuorumMember> membership;
  for (std::uint64_t id = 1; id <= 2u; ++id) {
    QuorumMember member;
    member.participant = ParticipantId(id);
    member.generation = ParticipantGeneration(1);
    member.roles.insert(ParticipantRole::judge);
    member.required = true;
    membership.push_back(member);
  }
  std::vector<EligibleVote> votes;
  for (int index = 0; index < 10; ++index) {
    EligibleVote vote;
    vote.participant = ParticipantId(1);
    vote.generation = ParticipantGeneration(1);
    vote.candidate = CandidateId(10);
    vote.candidate_generation = CandidateGeneration(1);
    votes.push_back(vote);
  }
  const QuorumReport report = compute_quorum(policy, membership, votes, 1, EnsembleGeneration(1),
                                             CoordinatorEpoch(1), Sequence(1));
  EF_CHECK_EQ(report.contributors, 1u);
  EF_CHECK(report.state != QuorumState::reached);
}

EF_TEST(a_superseded_or_cancelled_execution_never_reports_success) {
  for (int mode = 0; mode < 2; ++mode) {
    EnsembleFabric fabric;
    EnsembleHarness harness(fabric);
    EnsembleSpec spec = parallel_spec(1, {ParticipantId(1), ParticipantId(2)});
    EF_REQUIRE_OK(harness.define(spec));
    EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
    harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                   reference_program("ok:value=A").value(), 9500u);
    harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                   reference_program("ok:value=B").value(), 9501u);
    EF_REQUIRE_OK(harness.register_all());
    EF_REQUIRE_OK(harness.step());
    if (mode == 0) {
      EF_REQUIRE_OK(fabric.cancel_ensemble(EnsembleId(1), EnsembleGeneration(1), "property"));
    } else {
      EF_REQUIRE_OK(fabric.supersede_ensemble(EnsembleId(1), EnsembleGeneration(1)));
    }
    EF_REQUIRE_OK(harness.run());
    Outcome<EnsembleResult> result = fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
    EF_CHECK(!result.ok());
    EF_CHECK_ERR(fabric.current_result(EnsembleId(1)), EnsembleError::not_found);
  }
}

EF_TEST(deterministic_inputs_produce_deterministic_decisions) {
  for (int repetition = 0; repetition < 4; ++repetition) {
    EnsembleFabric fabric;
    EnsembleHarness harness(fabric);
    EnsembleSpec spec = parallel_spec(1, {ParticipantId(1), ParticipantId(2), ParticipantId(3)});
    spec.arbitration.factors = {ArbitrationFactor::aggregate_score};
    EF_REQUIRE_OK(harness.define(spec));
    EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
    for (std::size_t index = 0; index < spec.participants.size(); ++index) {
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[index],
                     reference_program("ok:value=value-" + std::to_string(index)).value(),
                     9600u + index);
    }
    EF_REQUIRE_OK(harness.register_all());
    EF_REQUIRE_OK(harness.run());
    Outcome<EnsembleResult> result = fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
    EF_REQUIRE_OK(result);
    if (result.ok()) {
      EF_CHECK_EQ(result.value().selected_candidate.value(), 1u);
      EF_CHECK_EQ(result.value().commit_fingerprint,
                  commit_fingerprint(result.value()));
    }
  }
}

EF_TEST_MAIN
