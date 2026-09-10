#include <filesystem>
#include <fstream>

#include "support/test_harness.hpp"
#include "support/test_support.hpp"

using namespace ensemble_fabric;

namespace {

std::filesystem::path scratch_path(const std::string& name) {
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "ensemble_fabric_tests";
  std::filesystem::create_directories(base);
  return base / name;
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  std::vector<std::byte> bytes;
  char buffer[4096];
  while (stream) {
    stream.read(buffer, sizeof(buffer));
    const std::streamsize count = stream.gcount();
    for (std::streamsize index = 0; index < count; ++index) {
      bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(buffer[index])));
    }
  }
  return bytes;
}

}

EF_TEST(state_round_trips_through_an_atomic_file) {
  const std::filesystem::path path = scratch_path("round_trip.state");
  std::filesystem::remove(path);
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  HarnessParticipant& participant =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                     reference_program("ok:value=persisted").value(), 7001u);
  EF_REQUIRE_OK(harness.register_participant(participant));
  EF_REQUIRE_OK(harness.run());
  EF_REQUIRE_OK(fabric.finalize(EnsembleId(1), EnsembleGeneration(1)));
  EF_REQUIRE_OK(fabric.save_state(path.string()));

  EnsembleFabric recovered;
  Outcome<CoordinatorEpoch> epoch = recovered.recover_state(path.string());
  EF_REQUIRE_OK(epoch);
  EF_CHECK(epoch.value().value() > 1u);
  Outcome<EnsembleResult> result = recovered.current_result(EnsembleId(1));
  EF_REQUIRE_OK(result);
  EF_CHECK(result.value().authoritative());
  EF_CHECK_EQ(result.value().payload_bytes, 9u);
  std::filesystem::remove(path);
}

EF_TEST(recovery_invalidates_live_process_authority) {
  const std::filesystem::path path = scratch_path("revalidation.state");
  std::filesystem::remove(path);
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  HarnessParticipant& participant =
      harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                     reference_program("gate:gate=never").value(), 7002u);
  EF_REQUIRE_OK(harness.register_participant(participant));
  // Dispatch without executing: the candidate is genuinely in flight when the
  // coordinator dies.
  EF_REQUIRE_OK(harness.declare_only());
  EF_REQUIRE_OK(fabric.save_state(path.string()));

  EnsembleFabric recovered;
  EF_REQUIRE_OK(recovered.recover_state(path.string()));
  Outcome<EnsembleSnapshot> snapshot = recovered.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_CHECK(snapshot.value().execution_status == ExecutionStatus::revalidation_required);
  for (const ParticipantSnapshot& view : snapshot.value().participants) {
    EF_CHECK(view.status == ParticipantStatus::revalidation_required);
    EF_CHECK(!view.authoritative);
  }
  EF_CHECK(recovered.counters().revalidations_required >= 1u);
  std::filesystem::remove(path);
}

EF_TEST(a_corrupt_payload_is_rejected) {
  const std::filesystem::path path = scratch_path("corrupt.state");
  EnsembleFabric fabric;
  EF_REQUIRE_OK(fabric.save_state(path.string()));
  std::vector<std::byte> bytes = read_bytes(path);
  EF_CHECK(bytes.size() > 48u);
  bytes[bytes.size() - 1u] = static_cast<std::byte>(static_cast<unsigned char>(bytes.back()) ^ 0xFFu);
  write_bytes(path, bytes);

  EnsembleFabric recovered;
  EF_CHECK_ERR(recovered.recover_state(path.string()), EnsembleError::checksum_mismatch);
  std::filesystem::remove(path);
}

EF_TEST(a_truncated_state_file_is_rejected) {
  const std::filesystem::path path = scratch_path("truncated.state");
  EnsembleFabric fabric;
  EF_REQUIRE_OK(fabric.save_state(path.string()));
  std::vector<std::byte> bytes = read_bytes(path);
  bytes.resize(bytes.size() / 2u);
  write_bytes(path, bytes);
  EnsembleFabric recovered;
  EF_CHECK_ERR(recovered.recover_state(path.string()), EnsembleError::persistence_corruption);
  std::filesystem::remove(path);
}

EF_TEST(trailing_bytes_after_the_payload_are_rejected) {
  const std::filesystem::path path = scratch_path("trailing.state");
  EnsembleFabric fabric;
  EF_REQUIRE_OK(fabric.save_state(path.string()));
  std::vector<std::byte> bytes = read_bytes(path);
  bytes.push_back(std::byte{0x00});
  write_bytes(path, bytes);
  EnsembleFabric recovered;
  EF_CHECK_ERR(recovered.recover_state(path.string()), EnsembleError::trailing_data);
  std::filesystem::remove(path);
}

EF_TEST(bad_magic_and_unsupported_versions_are_rejected) {
  const std::filesystem::path path = scratch_path("magic.state");
  EnsembleFabric fabric;
  EF_REQUIRE_OK(fabric.save_state(path.string()));
  std::vector<std::byte> bytes = read_bytes(path);
  std::vector<std::byte> bad_magic = bytes;
  bad_magic[0] = std::byte{0x00};
  write_bytes(path, bad_magic);
  EnsembleFabric recovered;
  EF_CHECK_ERR(recovered.recover_state(path.string()), EnsembleError::malformed_persistence);

  std::vector<std::byte> bad_version = bytes;
  bad_version[4] = std::byte{0x7Fu};
  write_bytes(path, bad_version);
  EnsembleFabric second;
  EF_CHECK_ERR(second.recover_state(path.string()), EnsembleError::unsupported_version);

  // A corrupted header checksum is detected independently of the version word.
  std::vector<std::byte> bad_header = bytes;
  bad_header[36] = static_cast<std::byte>(static_cast<unsigned char>(bad_header[36]) ^ 0xFFu);
  write_bytes(path, bad_header);
  EnsembleFabric third;
  EF_CHECK_ERR(third.recover_state(path.string()), EnsembleError::checksum_mismatch);
  std::filesystem::remove(path);
}

EF_TEST(a_missing_state_file_is_reported_as_an_io_error) {
  EnsembleFabric fabric;
  const std::filesystem::path path = scratch_path("does_not_exist.state");
  std::filesystem::remove(path);
  EF_CHECK_ERR(fabric.recover_state(path.string()), EnsembleError::persistence_io_error);
}

EF_TEST(oversized_declared_payloads_are_rejected_before_allocation) {
  const std::filesystem::path path = scratch_path("oversized.state");
  std::vector<std::byte> container(64u, std::byte{0x00});
  container[0] = std::byte{0x45};
  container[1] = std::byte{0x4E};
  container[2] = std::byte{0x42};
  container[3] = std::byte{0x46};
  container[4] = std::byte{0x01};
  container[8] = std::byte{0xFF};
  container[9] = std::byte{0xFF};
  container[10] = std::byte{0xFF};
  container[11] = std::byte{0xFF};
  write_bytes(path, container);
  EnsembleFabric fabric;
  EF_CHECK_ERR(fabric.recover_state(path.string()), EnsembleError::checksum_mismatch);
  std::filesystem::remove(path);
}

EF_TEST_MAIN
