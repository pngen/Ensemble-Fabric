#include "support/test_harness.hpp"
#include "support/test_support.hpp"

using namespace ensemble_fabric;

EF_TEST(strong_ids_are_type_safe_and_ordered) {
  const EnsembleId ensemble(7);
  const CandidateId candidate(7);
  EF_CHECK_EQ(ensemble.value(), candidate.value());
  EF_CHECK(ensemble.valid());
  EF_CHECK(!EnsembleId(0).valid());
  EF_CHECK(EnsembleId(3) < EnsembleId(4));
  EF_CHECK(EnsembleId(4) > EnsembleId(3));
}

EF_TEST(strict_parsing_rejects_malformed_identities) {
  EF_CHECK(EnsembleId::parse("42").has_value());
  EF_CHECK_EQ(EnsembleId::parse("42").value().value(), 42u);
  EF_CHECK(!EnsembleId::parse("").has_value());
  EF_CHECK(!EnsembleId::parse("0").has_value());
  EF_CHECK(!EnsembleId::parse("-1").has_value());
  EF_CHECK(!EnsembleId::parse(" 1").has_value());
  EF_CHECK(!EnsembleId::parse("1 ").has_value());
  EF_CHECK(!EnsembleId::parse("1x").has_value());
  EF_CHECK(!EnsembleId::parse("99999999999999999999").has_value());
}

EF_TEST(generation_advance_saturates_instead_of_wrapping) {
  EnsembleGeneration generation(5);
  EF_CHECK(advance_generation(generation));
  EF_CHECK_EQ(generation.value(), 6u);
  EnsembleGeneration last(static_cast<std::uint64_t>(-1));
  EF_CHECK(!advance_generation(last));
  EF_CHECK_EQ(last.value(), static_cast<std::uint64_t>(-1));
}

EF_TEST(id_allocator_never_repeats_after_observe) {
  IdAllocator<CandidateIdTag> allocator;
  const auto first = allocator.allocate();
  EF_CHECK(first.has_value());
  EF_CHECK_EQ(first.value().value(), 1u);
  allocator.observe(CandidateId(100));
  const auto second = allocator.allocate();
  EF_CHECK(second.has_value());
  EF_CHECK_EQ(second.value().value(), 101u);
}

EF_TEST(actor_comparison_ignores_the_target) {
  ParticipantAuthority lhs;
  lhs.ensemble = EnsembleId(1);
  lhs.ensemble_generation = EnsembleGeneration(1);
  lhs.participant = ParticipantId(2);
  lhs.participant_generation = ParticipantGeneration(1);
  lhs.worker = WorkerId(3);
  lhs.worker_boot = WorkerBootId(4);
  lhs.coordinator_epoch = CoordinatorEpoch(5);
  ParticipantAuthority rhs = lhs;
  rhs.ensemble_generation = EnsembleGeneration(9);
  EF_CHECK(same_actor(lhs, rhs));
  rhs.worker_boot = WorkerBootId(99);
  EF_CHECK(!same_actor(lhs, rhs));
}

EF_TEST_MAIN
