#include "support/test_harness.hpp"
#include "support/test_support.hpp"

using namespace ensemble_fabric;

EF_TEST(a_script_parses_into_a_renderable_program) {
  Outcome<ReferenceProgram> program =
      ReferenceProgram::parse("seed=42;ok:value=hello;fail:kind=unavailable;"
                              "evaluate:judgment=accept,score=0.75,verification=pass,rationale=good");
  EF_REQUIRE_OK(program);
  if (!program.ok()) {
    return;
  }
  EF_CHECK_EQ(program.value().seed, 42u);
  EF_CHECK_EQ(program.value().steps.size(), 3u);
  EF_CHECK(program.value().steps[0].kind == ReferenceStepKind::produce_ok);
  EF_CHECK(program.value().steps[1].failure == ParticipantFailureKind::unavailable);
  EF_CHECK(program.value().steps[2].judgment == CategoricalJudgment::accept);
  EF_CHECK(program.value().steps[2].score_defined);
  const std::string rendered = program.value().render();
  EF_CHECK(rendered.find("seed=42") != std::string::npos);
}

EF_TEST(unknown_script_content_is_rejected) {
  EF_CHECK_ERR(ReferenceProgram::parse("nonsense"), EnsembleError::invalid_argument);
  EF_CHECK_ERR(ReferenceProgram::parse("ok:bogus=1"), EnsembleError::invalid_argument);
  EF_CHECK_ERR(ReferenceProgram::parse("fail:kind=nonsense"), EnsembleError::invalid_enum_value);
  EF_CHECK_ERR(ReferenceProgram::parse("evaluate:score=abc"), EnsembleError::invalid_argument);
  EF_CHECK_ERR(ReferenceProgram::parse("evaluate:score=nan"), EnsembleError::invalid_argument);
  EF_CHECK_ERR(ReferenceProgram::parse("evaluate:weight=0"), EnsembleError::invalid_argument);
  EF_CHECK_ERR(ReferenceProgram::parse("ok:value"), EnsembleError::invalid_argument);
}

EF_TEST(the_last_step_repeats_deterministically) {
  Outcome<ReferenceProgram> program = ReferenceProgram::parse("ok:value=first;ok:value=second");
  EF_REQUIRE_OK(program);
  if (!program.ok()) {
    return;
  }
  EF_CHECK_EQ(program.value().step_at(0).value, std::string("first"));
  EF_CHECK_EQ(program.value().step_at(1).value, std::string("second"));
  EF_CHECK_EQ(program.value().step_at(2).value, std::string("second"));
  EF_CHECK_EQ(program.value().step_at(99).value, std::string("second"));
}

EF_TEST(the_backend_produces_content_abstention_and_typed_failure) {
  Outcome<ReferenceProgram> program =
      ReferenceProgram::parse("ok:value=payload;abstain:rationale=none;malformed:bytes=4");
  EF_REQUIRE_OK(program);
  if (!program.ok()) {
    return;
  }
  ReferenceBackend backend(std::move(program).value());
  DispatchCandidateMessage request;
  request.candidate = CandidateId(3);
  request.attempt = 1u;

  Outcome<ParticipantProduction> first = backend.produce(request);
  EF_REQUIRE_OK(first);
  EF_CHECK(first.value().kind == ParticipantProduction::Kind::content);
  EF_CHECK_EQ(first.value().payload.size(), 7u);

  Outcome<ParticipantProduction> second = backend.produce(request);
  EF_REQUIRE_OK(second);
  EF_CHECK(second.value().kind == ParticipantProduction::Kind::abstention);

  Outcome<ParticipantProduction> third = backend.produce(request);
  EF_REQUIRE_OK(third);
  EF_CHECK(third.value().kind == ParticipantProduction::Kind::failure);
  EF_CHECK(third.value().failure == ParticipantFailureKind::malformed_output);
}

EF_TEST(the_backend_replays_identically) {
  Outcome<ReferenceProgram> program = ReferenceProgram::parse("ok:value=stable");
  EF_REQUIRE_OK(program);
  if (!program.ok()) {
    return;
  }
  ReferenceBackend first(std::move(program).value());
  ReferenceBackend second(reference_program("ok:value=stable").value());
  DispatchCandidateMessage request;
  request.candidate = CandidateId(1);
  request.attempt = 1u;
  const Outcome<ParticipantProduction> a = first.produce(request);
  const Outcome<ParticipantProduction> b = second.produce(request);
  EF_REQUIRE_OK(a);
  EF_REQUIRE_OK(b);
  EF_CHECK(a.value().payload == b.value().payload);
}

EF_TEST(an_empty_script_is_a_deterministic_default) {
  Outcome<ReferenceProgram> program = ReferenceProgram::parse("");
  EF_REQUIRE_OK(program);
  EF_CHECK(program.ok() && program.value().steps.size() == 1u);
}

EF_TEST_MAIN
