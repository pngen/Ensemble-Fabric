#include "support/test_harness.hpp"
#include "support/test_support.hpp"

using namespace ensemble_fabric;

EF_TEST(legal_transitions_are_exact) {
  EF_CHECK(is_legal_transition(CandidateState::declared, CandidateState::dispatched));
  EF_CHECK(is_legal_transition(CandidateState::dispatched, CandidateState::output_received));
  EF_CHECK(is_legal_transition(CandidateState::output_received, CandidateState::valid));
  EF_CHECK(is_legal_transition(CandidateState::valid, CandidateState::eligible));
  EF_CHECK(is_legal_transition(CandidateState::eligible, CandidateState::selected));
  EF_CHECK(is_legal_transition(CandidateState::failed, CandidateState::declared));
  EF_CHECK(!is_legal_transition(CandidateState::selected, CandidateState::valid));
  EF_CHECK(!is_legal_transition(CandidateState::retired, CandidateState::declared));
  EF_CHECK(!is_legal_transition(CandidateState::declared, CandidateState::valid));
  EF_CHECK(!is_legal_transition(CandidateState::valid, CandidateState::valid));
}

EF_TEST(terminal_and_content_states_are_distinct) {
  EF_CHECK(is_terminal(CandidateState::selected));
  EF_CHECK(is_terminal(CandidateState::cancelled));
  EF_CHECK(!is_terminal(CandidateState::valid));
  EF_CHECK(is_content_state(CandidateState::valid));
  EF_CHECK(is_content_state(CandidateState::selected));
  EF_CHECK(!is_content_state(CandidateState::invalid));
  EF_CHECK(permits_output(CandidateState::declared));
  EF_CHECK(permits_output(CandidateState::dispatched));
  EF_CHECK(!permits_output(CandidateState::valid));
}

EF_TEST(a_candidate_that_loses_its_worker_still_moves_forward) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  HarnessParticipant& participant =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants.front(),
                     reference_program("ok:value=alpha").value(), 1001u);
  EF_REQUIRE_OK(harness.register_participant(participant));
  EF_REQUIRE_OK(harness.run());

  Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_CHECK_EQ(snapshot.value().candidates.size(), 1u);
  EF_CHECK_EQ(snapshot.value().candidates.front().state, CandidateState::valid);
}

EF_TEST_MAIN
