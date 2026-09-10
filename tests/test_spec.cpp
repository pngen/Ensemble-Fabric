#include "support/test_harness.hpp"
#include "support/test_support.hpp"

using namespace ensemble_fabric;

EF_TEST(valid_parallel_specification_is_accepted) {
  const Limits limits = Limits::defaults();
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1), ParticipantId(2)});
  EF_REQUIRE_OK(validate_spec(spec, limits));
  const std::uint64_t fingerprint = compute_spec_fingerprint(spec);
  EF_CHECK(fingerprint != 0u);
  EnsembleSpec copy = spec;
  copy.label = "renamed";
  EF_CHECK(compute_spec_fingerprint(copy) != fingerprint);
}

EF_TEST(specifications_without_candidate_producers_are_rejected) {
  EnsembleSpec spec;
  spec.id = EnsembleId(1);
  spec.label = "judges-only";
  spec.participants.push_back(judge_participant(ParticipantId(1)));
  EF_CHECK_ERR(validate_spec(spec, Limits::defaults()), EnsembleError::invalid_argument);
}

EF_TEST(duplicate_participant_identities_are_rejected) {
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  spec.participants.push_back(candidate_participant(ParticipantId(1)));
  EF_CHECK_ERR(validate_spec(spec, Limits::defaults()), EnsembleError::participant_duplicate);
}

EF_TEST(stage_may_not_reference_a_non_producing_participant) {
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  spec.participants.push_back(judge_participant(ParticipantId(2)));
  spec.stages.front().participants.push_back(ParticipantId(2));
  EF_CHECK_ERR(validate_spec(spec, Limits::defaults()), EnsembleError::invalid_argument);
}

EF_TEST(stage_domain_requirements_must_be_satisfiable) {
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  spec.stages.front().required_domains.push_back("physics");
  EF_CHECK_ERR(validate_spec(spec, Limits::defaults()), EnsembleError::invalid_argument);
  spec.participants.front().domain = "physics";
  EF_REQUIRE_OK(validate_spec(spec, Limits::defaults()));
}

EF_TEST(role_quota_must_name_a_declared_role) {
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  spec.quorum.role_quota.push_back(RoleQuota{ParticipantRole::verifier, 1u});
  EF_CHECK_ERR(validate_spec(spec, Limits::defaults()), EnsembleError::invalid_argument);
}

EF_TEST(verification_gating_requires_a_verifier) {
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  spec.aggregation.require_verification_pass = true;
  EF_CHECK_ERR(validate_spec(spec, Limits::defaults()), EnsembleError::invalid_argument);
  spec.participants.push_back(verifier_participant(ParticipantId(9)));
  EF_REQUIRE_OK(validate_spec(spec, Limits::defaults()));
}

EF_TEST(fallback_requires_triggers_and_fallback_participants) {
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  spec.fallback.enabled = true;
  EF_CHECK_ERR(validate_spec(spec, Limits::defaults()), EnsembleError::invalid_argument);
  spec.fallback.triggers.push_back(FallbackTrigger::retry_exhausted);
  EF_CHECK_ERR(validate_spec(spec, Limits::defaults()), EnsembleError::invalid_argument);
  spec.participants.push_back(fallback_participant(ParticipantId(5)));
  spec.fallback.fallback_participants.push_back(ParticipantId(5));
  EF_REQUIRE_OK(validate_spec(spec, Limits::defaults()));
}

EF_TEST(fallback_participants_without_fallback_policy_are_rejected) {
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  spec.participants.push_back(fallback_participant(ParticipantId(5)));
  spec.fallback.fallback_participants.push_back(ParticipantId(5));
  EF_CHECK_ERR(validate_spec(spec, Limits::defaults()), EnsembleError::invalid_argument);
}

EF_TEST(consensus_threshold_is_bounded) {
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  spec.quorum.consensus_threshold_percent = 101u;
  EF_CHECK_ERR(validate_spec(spec, Limits::defaults()), EnsembleError::invalid_argument);
  spec.quorum.consensus_threshold_percent = 100u;
  EF_REQUIRE_OK(validate_spec(spec, Limits::defaults()));
}

EF_TEST(duplicate_arbitration_factors_are_rejected) {
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  spec.arbitration.factors = {ArbitrationFactor::aggregate_score,
                              ArbitrationFactor::aggregate_score};
  EF_CHECK_ERR(validate_spec(spec, Limits::defaults()), EnsembleError::invalid_argument);
}

EF_TEST(retry_attempts_are_bounded_by_the_limits) {
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  spec.retry.max_attempts = Limits::defaults().max_retries + 2u;
  EF_CHECK_ERR(validate_spec(spec, Limits::defaults()), EnsembleError::resource_limit_exceeded);
  spec.retry.max_attempts = 0u;
  EF_CHECK_ERR(validate_spec(spec, Limits::defaults()), EnsembleError::invalid_argument);
}

EF_TEST(stage_and_participant_rendering_is_deterministic) {
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1), ParticipantId(2)});
  const std::string first = render_spec(spec);
  const std::string second = render_spec(spec);
  EF_CHECK_EQ(first, second);
  EF_CHECK(first.find("CANDIDATE") != std::string::npos);
}

EF_TEST(a_specification_without_stages_still_dispatches_every_producer) {
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1), ParticipantId(2)});
  spec.stages.clear();
  EF_REQUIRE_OK(validate_spec(spec, Limits::defaults()));

  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                 reference_program("ok:value=one").value(), 21001u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                 reference_program("ok:value=two").value(), 21002u);
  EF_REQUIRE_OK(harness.register_all());
  EF_REQUIRE_OK(harness.run());
  Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_CHECK(snapshot.ok() && snapshot.value().candidates.size() == 2u);
}

EF_TEST_MAIN
