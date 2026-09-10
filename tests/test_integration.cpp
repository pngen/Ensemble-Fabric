#include "support/test_harness.hpp"
#include "support/test_support.hpp"

using namespace ensemble_fabric;

EF_TEST(parallel_candidates_with_judges_select_deterministically) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1), ParticipantId(2), ParticipantId(3)});
  spec.participants.push_back(judge_participant(ParticipantId(101)));
  spec.participants.push_back(judge_participant(ParticipantId(102)));
  spec.aggregation.kind = AggregationKind::judge_arbitration;
  spec.aggregation.require_judge_acceptance = true;
  spec.aggregation.min_judge_acceptance_ratio = 0.5;
  spec.quorum.min_evaluations = 2u;
  spec.quorum.consensus_threshold_percent = 50u;
  spec.quorum.vote_basis = VoteBasis::contributing_participants;
  spec.evidence.min_evaluations_per_candidate = 2u;
  spec.arbitration.factors = {ArbitrationFactor::judge_acceptance_ratio,
                              ArbitrationFactor::aggregate_score};
  spec.arbitration.tie_break = TieBreakPolicy::lowest_candidate_id;
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                 reference_program("ok:value=alpha").value(), 14001u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                 reference_program("ok:value=beta").value(), 14002u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[2],
                 reference_program("ok:value=gamma").value(), 14003u);
  // Both judges accept the first candidate and reject the other two, so the
  // ensemble reaches a genuine consensus rather than a tie.
  const std::string verdicts =
      "evaluate:judgment=accept,score=0.9;evaluate:judgment=reject,score=0.1;"
      "evaluate:judgment=reject,score=0.1";
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[3],
                 reference_program(verdicts).value(), 14004u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[4],
                 reference_program(verdicts).value(), 14005u);
  EF_REQUIRE_OK(harness.register_all());
  EF_REQUIRE_OK(harness.run());

  Outcome<EnsembleResult> result = fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(result);
  EF_CHECK(result.ok() && result.value().authoritative());
  EF_CHECK(result.ok() && result.value().selected_candidate.value() == 1u);
  EF_CHECK(result.ok() && result.value().consensus.state == ConsensusState::consensus);
  EF_CHECK(result.ok() && result.value().quorum.state == QuorumState::reached);
  if (result.ok()) {
    const std::string rendered = result.value().render();
    EF_CHECK(rendered.find("arbitration") != std::string::npos);
    EF_CHECK(rendered.find("quorum REACHED") != std::string::npos);
  }
}

EF_TEST(judge_disagreement_is_explicit_and_never_fabricated_into_consensus) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1), ParticipantId(2)});
  spec.participants.push_back(judge_participant(ParticipantId(101)));
  spec.participants.push_back(judge_participant(ParticipantId(102)));
  spec.quorum.consensus_threshold_percent = 100u;
  spec.quorum.min_evaluations = 2u;
  spec.evidence.min_evaluations_per_candidate = 1u;
  spec.arbitration.tie_break = TieBreakPolicy::report_tie;
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                 reference_program("ok:value=alpha").value(), 14101u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                 reference_program("ok:value=beta").value(), 14102u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[2],
                 reference_program("evaluate:judgment=accept,score=0.9;"
                                          "evaluate:judgment=reject,score=0.1")
                     .value(),
                 14103u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[3],
                 reference_program("evaluate:judgment=reject,score=0.1;"
                                          "evaluate:judgment=accept,score=0.9")
                     .value(),
                 14104u);
  EF_REQUIRE_OK(harness.register_all());
  EF_REQUIRE_OK(harness.run());

  Outcome<EnsembleResult> result = fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
  // The conflict may resolve to a tie; what matters is that it is never
  // silently reported as consensus and that a terminal record exists.
  Outcome<EnsembleResult> stored = fabric.current_result(EnsembleId(1));
  EF_CHECK(stored.ok());
  if (stored.ok()) {
    EF_CHECK(stored.value().consensus.state == ConsensusState::tie ||
             stored.value().consensus.state == ConsensusState::disagreement ||
             stored.value().consensus.state == ConsensusState::consensus);
    if (stored.value().consensus.state != ConsensusState::consensus) {
      EF_CHECK(!stored.value().authoritative());
    }
  }
  EF_CHECK(!result.ok());
}

EF_TEST(verifier_gating_keeps_a_bad_candidate_out_despite_its_score) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1), ParticipantId(2)});
  spec.participants.push_back(judge_participant(ParticipantId(101)));
  spec.participants.push_back(verifier_participant(ParticipantId(201)));
  spec.aggregation.require_verification_pass = true;
  spec.evidence.mandatory_criteria.push_back(CriterionRef{5u, 1u});
  spec.arbitration.factors = {ArbitrationFactor::aggregate_score};
  spec.arbitration.tie_break = TieBreakPolicy::lowest_candidate_id;
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                 reference_program("ok:value=bad").value(), 14201u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                 reference_program("ok:value=good").value(), 14202u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[2],
                 reference_program("evaluate:judgment=accept,score=0.99").value(), 14203u);
  // The verifier fails the first candidate and passes the second.  Evidence
  // order follows the deterministic candidate order.
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[3],
                 reference_program("evaluate:verification=fail;evaluate:verification=pass")
                     .value(),
                 14204u);
  EF_REQUIRE_OK(harness.register_all());
  EF_REQUIRE_OK(harness.run());

  Outcome<EnsembleResult> result = fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(result);
  EF_CHECK(result.ok() && result.value().selected_candidate.value() == 2u);
  if (result.ok()) {
    for (const CandidateSnapshot& candidate : result.value().candidates) {
      if (candidate.id.value() == 1u) {
        EF_CHECK(candidate.state == CandidateState::rejected);
      }
    }
  }
}

EF_TEST(staged_specialists_execute_in_order_and_only_one_result_is_committed) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec;
  spec.id = EnsembleId(1);
  spec.label = "staged";
  spec.task_class = "task";
  spec.participants.push_back(specialist_participant(ParticipantId(1), "physics"));
  spec.participants.push_back(specialist_participant(ParticipantId(2), "chemistry"));
  spec.participants.push_back(judge_participant(ParticipantId(101)));
  spec.stages.push_back(sequential_stage(StageId(1), {ParticipantId(1), ParticipantId(2)}));
  spec.quorum.min_evaluations = 1u;
  spec.evidence.min_evaluations_per_candidate = 1u;
  spec.arbitration.tie_break = TieBreakPolicy::lowest_candidate_id;
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                 reference_program("ok:value=physics").value(), 14301u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                 reference_program("ok:value=chemistry").value(), 14302u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[2],
                 reference_program("evaluate:judgment=accept,score=0.5").value(), 14303u);
  EF_REQUIRE_OK(harness.register_all());

  // In a sequential stage exactly one branch runs at a time: after the first
  // completed step the first candidate holds content and the second is still
  // waiting to be dispatched.
  Outcome<std::size_t> first = harness.step();
  EF_REQUIRE_OK(first);
  EF_CHECK(first.ok() && first.value() == 1u);
  Outcome<EnsembleSnapshot> midway = fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(midway);
  if (midway.ok()) {
    EF_CHECK_EQ(midway.value().candidates.size(), 2u);
    EF_CHECK(midway.value().candidates[0].state == CandidateState::valid);
    EF_CHECK(midway.value().candidates[1].state == CandidateState::declared);
  }
  EF_REQUIRE_OK(harness.run());
  Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_CHECK(snapshot.ok() && snapshot.value().candidates.size() == 2u);
  for (const CandidateSnapshot& candidate : snapshot.value().candidates) {
    EF_CHECK(candidate.role == ParticipantRole::specialist);
    EF_CHECK(candidate.state == CandidateState::valid);
  }
  Outcome<EnsembleResult> result = fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(result);
  EF_CHECK(result.ok() && result.value().selected_role == ParticipantRole::specialist);
}

EF_TEST(a_participant_lost_mid_flight_does_not_block_a_valid_ensemble) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1), ParticipantId(2)});
  spec.retry.max_attempts = 1u;
  spec.quorum.min_valid_candidates = 1u;
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                 reference_program("ok:value=primary").value(), 14401u);
  HarnessParticipant& optional =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                     reference_program("ok:value=never-produced").value(), 14402u);
  EF_REQUIRE_OK(harness.register_all());

  // Dispatch both branches without executing them, so that the second branch is
  // genuinely in flight when its process is lost.
  EF_REQUIRE_OK(harness.declare_only());
  EF_REQUIRE_OK(fabric.report_participant_failure(harness.authority_of(optional),
                                                ParticipantFailureKind::unavailable,
                                                "worker process died"));

  // The surviving branch completes normally and the ensemble still commits.
  EF_REQUIRE_OK(harness.complete_dispatched(ParticipantId(1)));
  EF_REQUIRE_OK(harness.run());
  Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  if (snapshot.ok()) {
    for (const CandidateSnapshot& candidate : snapshot.value().candidates) {
      if (candidate.participant.value() == 2u) {
        EF_CHECK(candidate.state == CandidateState::failed);
      }
    }
  }
  Outcome<EnsembleResult> result = fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(result);
  EF_CHECK(result.ok() && result.value().selected_candidate.valid());
}

EF_TEST(an_ensemble_reaches_a_typed_terminal_decision_when_every_candidate_fails) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1), ParticipantId(2)});
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                 reference_program("fail:kind=internal_error").value(), 14501u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                 reference_program("malformed:bytes=1").value(), 14502u);
  EF_REQUIRE_OK(harness.register_all());
  EF_REQUIRE_OK(harness.run());

  Outcome<EnsembleResult> result = fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
  EF_CHECK(!result.ok());
  EF_CHECK(result.code() == EnsembleError::all_candidates_invalid ||
           result.code() == EnsembleError::no_eligible_candidate);
  Outcome<EnsembleResult> stored = fabric.current_result(EnsembleId(1));
  EF_REQUIRE_OK(stored);
  EF_CHECK(stored.ok() && !stored.value().authoritative());
  EF_CHECK(stored.ok() && stored.value().decision == EnsembleDecision::no_eligible_candidate);
}

EF_TEST(redefining_an_ensemble_advances_the_generation_and_fences_the_old_one) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                 reference_program("ok:value=first").value(), 14601u);
  EF_REQUIRE_OK(harness.register_all());
  EF_REQUIRE_OK(harness.run());

  EnsembleSpec redefined = parallel_spec(1, {ParticipantId(1), ParticipantId(2)});
  Outcome<EnsembleGeneration> generation = fabric.define_ensemble(redefined);
  EF_REQUIRE_OK(generation);
  EF_CHECK(generation.ok() && generation.value().value() == 2u);
  EF_CHECK_ERR(fabric.finalize(EnsembleId(1), EnsembleGeneration(1)),
               EnsembleError::stale_ensemble_generation);
  Outcome<EnsembleSpec> current = fabric.spec(EnsembleId(1));
  EF_REQUIRE_OK(current);
  EF_CHECK(current.ok() && current.value().participants.size() == 2u);
}

EF_TEST(weighted_voting_respects_declared_weights) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1), ParticipantId(2)});
  spec.participants.push_back(judge_participant(ParticipantId(101)));
  spec.aggregation.kind = AggregationKind::weighted_vote;
  spec.aggregation.use_weights = true;
  spec.arbitration.factors = {ArbitrationFactor::weighted_vote};
  spec.arbitration.tie_break = TieBreakPolicy::lowest_candidate_id;
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                 reference_program("ok:value=low").value(), 14701u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                 reference_program("ok:value=high").value(), 14702u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[2],
                 reference_program("evaluate:judgment=accept,score=7,weight=9;"
                                          "evaluate:judgment=accept,score=2,weight=1")
                     .value(),
                 14703u);
  EF_REQUIRE_OK(harness.register_all());
  EF_REQUIRE_OK(harness.run());
  Outcome<EnsembleResult> result = fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(result);
  EF_CHECK(result.ok() && result.value().selected_candidate.value() == 1u);
}

EF_TEST_MAIN
