#include "support/test_harness.hpp"
#include "support/test_support.hpp"

using namespace ensemble_fabric;

EF_TEST(checked_add_and_multiply_detect_overflow) {
  std::uint64_t out = 0;
  EF_CHECK(checked_add(10, 20, 100, out));
  EF_CHECK_EQ(out, 30u);
  EF_CHECK(!checked_add(60, 60, 100, out));
  EF_CHECK(!checked_add(0, 101, 100, out));
  EF_CHECK(checked_mul(10, 10, 100, out));
  EF_CHECK_EQ(out, 100u);
  EF_CHECK(!checked_mul(11, 10, 100, out));
  EF_CHECK(checked_mul(0, 1000000, 100, out));
  EF_CHECK_EQ(out, 0u);
}

EF_TEST(default_limits_validate) {
  const Limits limits = Limits::defaults();
  EF_REQUIRE_OK(limits.validate());
}

EF_TEST(incoherent_limits_are_rejected) {
  Limits limits = Limits::defaults();
  limits.max_participants_per_ensemble = 0;
  EF_CHECK_ERR(limits.validate(), EnsembleError::invalid_argument);

  limits = Limits::defaults();
  limits.max_candidate_payload_bytes = limits.max_frame_bytes + 1u;
  EF_CHECK_ERR(limits.validate(), EnsembleError::invalid_argument);

  limits = Limits::defaults();
  limits.max_frame_bytes = 8u;
  EF_CHECK_ERR(limits.validate(), EnsembleError::invalid_argument);
}

EF_TEST(encoder_refuses_to_grow_past_its_bound) {
  Limits limits = Limits::defaults();
  limits.max_persistence_bytes = 64u;
  Encoder encoder(limits);
  for (int index = 0; index < 40; ++index) {
    encoder.u8(0x5Au);
  }
  EF_CHECK(encoder.size() <= 64u);
}

EF_TEST(decoder_rejects_oversized_declarations_before_allocating) {
  Limits limits = Limits::defaults();
  limits.max_metadata_bytes = 8u;
  Encoder encoder(limits);
  encoder.u32(0xFFFFFFFFu);
  const std::vector<std::byte> buffer = encoder.buffer();
  Decoder decoder(std::span<const std::byte>(buffer.data(), buffer.size()), limits);
  EF_CHECK_ERR(decoder.string(limits.max_metadata_bytes), EnsembleError::payload_too_large);
}

EF_TEST(decoder_rejects_trailing_bytes) {
  Encoder encoder;
  encoder.u32(7u);
  encoder.u32(8u);
  const std::vector<std::byte> buffer = encoder.buffer();
  Decoder decoder(std::span<const std::byte>(buffer.data(), buffer.size()));
  EF_REQUIRE_OK(decoder.u32());
  EF_CHECK_ERR(decoder.require_exhausted("test"), EnsembleError::trailing_data);
}

EF_TEST(decoder_reports_truncation) {
  Encoder encoder;
  encoder.u64(1u);
  const std::vector<std::byte> buffer = encoder.buffer();
  Decoder decoder(std::span<const std::byte>(buffer.data(), 3u));
  EF_CHECK_ERR(decoder.u64(), EnsembleError::malformed_persistence);
}

EF_TEST_MAIN
