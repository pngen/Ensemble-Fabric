#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "ensemble_fabric/client.hpp"
#include "ensemble_fabric/fabric.hpp"
#include "ensemble_fabric/reference_backend.hpp"
#include "ensemble_fabric/version.hpp"
#include "ensemble_fabric/worker.hpp"

#include "support/test_harness.hpp"
#include "support/test_support.hpp"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <signal.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace {

using namespace ensemble_fabric;

std::string g_coordinator_path;
std::string g_worker_path;

/// A real operating-system process.  Termination is forceful, which is exactly
/// what a worker-death test needs.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess() { terminate(); }

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  [[nodiscard]] bool spawn(const std::string& executable,
                           const std::vector<std::string>& arguments) {
#if defined(_WIN32)
    std::string command = "\"" + executable + "\"";
    for (const std::string& argument : arguments) {
      command += " \"" + argument + "\"";
    }
    std::vector<char> buffer(command.begin(), command.end());
    buffer.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    // CREATE_NO_WINDOW keeps the reference deployment free of stray consoles.
    const BOOL created = CreateProcessA(nullptr, buffer.data(), nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information);
    if (created == FALSE) {
      return false;
    }
    CloseHandle(information.hThread);
    handle_ = information.hProcess;
    return true;
#else
    std::string command = "\"" + executable + "\"";
    for (const std::string& argument : arguments) {
      command += " '" + argument + "'";
    }
    pid_ = ::fork();
    if (pid_ < 0) {
      return false;
    }
    if (pid_ == 0) {
      ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
      ::_exit(127);
    }
    return true;
#endif
  }

  void terminate() {
#if defined(_WIN32)
    if (handle_ != nullptr) {
      if (WaitForSingleObject(handle_, 0) == WAIT_TIMEOUT) {
        TerminateProcess(handle_, 9);
      }
      WaitForSingleObject(handle_, INFINITE);
      CloseHandle(handle_);
      handle_ = nullptr;
    }
#else
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      int status = 0;
      ::waitpid(pid_, &status, 0);
      pid_ = -1;
    }
#endif
  }

 private:
#if defined(_WIN32)
  void* handle_{nullptr};
#else
  int pid_{-1};
#endif
};

std::filesystem::path scratch_directory() {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "ensemble_fabric_multiprocess";
  std::filesystem::create_directories(directory);
  return directory;
}

void remove_if_present(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
}

/// Waits for a file to appear without sleeping: each poll returns immediately.
[[nodiscard]] bool await_file(const std::filesystem::path& path, std::uint64_t attempts = 4000000u) {
  std::error_code error;
  for (std::uint64_t attempt = 0; attempt < attempts; ++attempt) {
    if (std::filesystem::exists(path, error)) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
  std::ifstream stream(path);
  std::string value;
  std::getline(stream, value);
  return value;
}

[[nodiscard]] bool await_condition(const std::function<bool()>& predicate,
                                   std::uint64_t attempts = 4000000u) {
  for (std::uint64_t attempt = 0; attempt < attempts; ++attempt) {
    if (predicate()) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

/// True when the rendered inspection report shows p id with the expected
/// authority.  The report is the inspection surface over the runtime, so this
/// asserts on what an operator would actually see.
[[nodiscard]] bool participant_authoritative(const std::string& report, std::uint64_t id,
                                             bool expected) {
  const std::string marker = "participant " + std::to_string(id) + " generation=";
  const std::size_t start = report.find(marker);
  if (start == std::string::npos) {
    return false;
  }
  const std::size_t end = report.find('\n', start);
  const std::string line =
      report.substr(start, end == std::string::npos ? std::string::npos : end - start);
  const bool authoritative = line.find("not_authoritative") == std::string::npos &&
                             line.find(" authoritative") != std::string::npos;
  return authoritative == expected;
}

[[nodiscard]] EnsembleSpec multiprocess_spec() {
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1), ParticipantId(2)});
  spec.label = "multiprocess";
  spec.arbitration.tie_break = TieBreakPolicy::lowest_candidate_id;
  spec.participants.push_back(judge_participant(ParticipantId(101)));
  spec.quorum.min_participants = 1u;
  spec.quorum.min_evaluations = 1u;
  spec.quorum.min_valid_candidates = 1u;
  spec.quorum.count_optional_participants = false;
  spec.quorum.vote_basis = VoteBasis::contributing_participants;
  spec.quorum.consensus_threshold_percent = 50u;
  spec.evidence.min_evaluations_per_candidate = 1u;
  spec.aggregation.kind = AggregationKind::judge_arbitration;
  spec.aggregation.require_judge_acceptance = true;
  spec.arbitration.factors = {ArbitrationFactor::judge_acceptance_ratio,
                              ArbitrationFactor::aggregate_score};
  spec.retry.max_attempts = 3u;
  spec.retry.retry_on_unavailable = true;
  spec.retry.replace_participant = true;
  spec.judging_criteria = {CriterionRef{1u, 1u}};
  return spec;
}

/// A real three-process deployment: the coordinator runs as its own OS process
/// and every participant is its own OS process.
struct Deployment {
  std::filesystem::path directory;
  std::filesystem::path state;
  std::filesystem::path port_file;
  ChildProcess coordinator;
  std::uint16_t port{0};
  std::vector<std::unique_ptr<ChildProcess>> workers;

  explicit Deployment(const std::string& name) {
    directory = scratch_directory() / name;
    std::filesystem::create_directories(directory);
    state = directory / "coordinator.state";
    port_file = directory / "coordinator.port";
    remove_if_present(state);
    remove_if_present(port_file);
  }

  [[nodiscard]] bool start_coordinator() {
    remove_if_present(port_file);
    if (!coordinator.spawn(g_coordinator_path, {"--port", "0", "--state", state.string(),
                                                "--port-file", port_file.string()})) {
      std::fprintf(stderr, "test_multiprocess: cannot spawn the coordinator '%s'\n",
                   g_coordinator_path.c_str());
      return false;
    }
    // Wait until the published port is readable: the file is written
    // atomically, and each poll returns immediately.
    std::error_code error;
    for (std::uint64_t attempt = 0; attempt < 4000000u; ++attempt) {
      if (std::filesystem::exists(port_file, error)) {
        port = static_cast<std::uint16_t>(std::strtoul(read_text(port_file).c_str(), nullptr, 10));
        if (port != 0u) {
          return true;
        }
      }
      std::this_thread::yield();
    }
    std::fprintf(stderr, "test_multiprocess: the coordinator did not publish a port at '%s'\n",
                 port_file.string().c_str());
    return false;
  }

  [[nodiscard]] Outcome<EnsembleClient> connect_client() {
    Endpoint endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = port;
    return EnsembleClient::connect(endpoint);
  }

  [[nodiscard]] bool start_worker(const std::string& name, std::uint64_t participant,
                                  const std::string& roles, const std::string& script,
                                  std::uint64_t boot, std::uint64_t worker_id) {
    const std::filesystem::path ready = directory / (name + ".ready");
    remove_if_present(ready);
    auto process = std::make_unique<ChildProcess>();
    const std::string endpoint = "127.0.0.1:" + std::to_string(port);
    std::vector<std::string> arguments = {"--coordinator", endpoint,
                                          "--participant", std::to_string(participant),
                                          "--roles", roles,
                                          "--ensemble", "1",
                                          "--ensemble-generation", "1",
                                          "--script", script,
                                          "--worker", std::to_string(worker_id),
                                          "--ready-file", ready.string()};
    if (boot != 0u) {
      arguments.push_back("--boot");
      arguments.push_back(std::to_string(boot));
    }
    if (!process->spawn(g_worker_path, arguments)) {
      return false;
    }
    workers.push_back(std::move(process));
    return await_file(ready);
  }

  [[nodiscard]] ChildProcess& worker(std::size_t index) { return *workers[index]; }

  void stop_workers() {
    for (auto& worker_process : workers) {
      worker_process->terminate();
    }
    workers.clear();
  }

  void shutdown() {
    stop_workers();
    coordinator.terminate();
  }
};

}  // namespace

EF_TEST(a_real_multiprocess_ensemble_runs_end_to_end) {
  EF_REQUIRE(!g_coordinator_path.empty() && !g_worker_path.empty());
  Deployment deployment("end_to_end");
  EF_REQUIRE(deployment.start_coordinator());

  Outcome<EnsembleClient> connected = deployment.connect_client();
  EF_REQUIRE_OK(connected);
  EnsembleClient client = std::move(connected).value();
  Outcome<DefineEnsembleAck> defined = client.define_ensemble(multiprocess_spec());
  EF_REQUIRE_OK(defined);
  EF_REQUIRE(defined.value().accepted);

  EF_REQUIRE(deployment.start_worker("candidate_a", 1u, "CANDIDATE", "ok:value=alpha", 0u, 50001u));
  EF_REQUIRE(deployment.start_worker("candidate_b", 2u, "CANDIDATE", "ok:value=beta", 0u, 50002u));
  EF_REQUIRE(deployment.start_worker("judge", 101u, "JUDGE",
                                     "evaluate:judgment=accept,score=0.5", 0u, 50003u));

  Outcome<OpenExecutionAck> opened = client.open_execution(EnsembleId(1), defined.value().generation);
  EF_REQUIRE_OK(opened);
  EF_REQUIRE(opened.value().accepted);

  Outcome<FinalizeResponseMessage> finalized =
      client.finalize(EnsembleId(1), defined.value().generation);
  EF_REQUIRE_OK(finalized);
  EF_CHECK(finalized.ok() && finalized.value().accepted);
  EF_CHECK(finalized.ok() && finalized.value().decision == EnsembleDecision::committed);
  if (finalized.ok() && !finalized.value().encoded_result.empty()) {
    Outcome<EnsembleResult> result =
        decode_result_payload(finalized.value().encoded_result, client.limits());
    EF_REQUIRE_OK(result);
    EF_CHECK(result.value().authoritative());
    EF_CHECK(result.value().payload_bytes > 0u);
    EF_CHECK(!result.value().participants.empty());
    for (const ParticipantOutcome& outcome : result.value().participants) {
      EF_CHECK(outcome.authoritative);
    }
  }
  client.close();
  deployment.shutdown();
}

EF_TEST(killing_a_candidate_worker_is_fenced_and_a_fresh_boot_takes_over) {
  EF_REQUIRE(!g_coordinator_path.empty() && !g_worker_path.empty());
  Deployment deployment("worker_death");
  EF_REQUIRE(deployment.start_coordinator());

  Outcome<EnsembleClient> connected = deployment.connect_client();
  EF_REQUIRE_OK(connected);
  EnsembleClient client = std::move(connected).value();
  Outcome<DefineEnsembleAck> defined = client.define_ensemble(multiprocess_spec());
  EF_REQUIRE_OK(defined);
  EF_REQUIRE(defined.value().accepted);

  const std::filesystem::path gate = deployment.directory / "never.gate";
  remove_if_present(gate);
  // The first candidate blocks until its gate file appears, so it is genuinely
  // in flight when the process is killed.  The gate is never created.
  EF_REQUIRE(deployment.start_worker("candidate_a", 1u, "CANDIDATE",
                                     "gate:gate=" + gate.string() + ";ok:value=stale-a", 0u,
                                     51001u));
  EF_REQUIRE(deployment.start_worker("candidate_b", 2u, "CANDIDATE", "ok:value=beta", 0u, 51002u));
  EF_REQUIRE(deployment.start_worker("judge", 101u, "JUDGE",
                                     "evaluate:judgment=accept,score=0.5;"
                                     "evaluate:judgment=reject,score=0.1",
                                     0u, 51003u));

  Outcome<OpenExecutionAck> opened = client.open_execution(EnsembleId(1), defined.value().generation);
  EF_REQUIRE_OK(opened);
  EF_REQUIRE(opened.value().accepted);

  // Wait until the coordinator reports both candidates as dispatched.
  const bool dispatched = await_condition([&client]() {
    Outcome<InspectResponseMessage> report = client.inspect(EnsembleId(1));
    if (!report.ok()) {
      return false;
    }
    Outcome<std::string> text = decode_report(report.value().encoded_report, client.limits());
    return text.ok() && text.value().find("state=DISPATCHED") != std::string::npos;
  });
  EF_CHECK(dispatched);

  // Kill the candidate worker as a real operating-system process.
  deployment.worker(0).terminate();

  // A fresh process with a new boot identity takes over the logical participant.
  EF_REQUIRE(deployment.start_worker("candidate_a_prime", 1u, "CANDIDATE", "ok:value=fresh-a", 0u,
                                     51004u));

  Outcome<FinalizeResponseMessage> finalized =
      client.finalize(EnsembleId(1), defined.value().generation);
  EF_REQUIRE_OK(finalized);
  EF_CHECK(finalized.ok() && finalized.value().accepted);
  if (finalized.ok() && !finalized.value().encoded_result.empty()) {
    Outcome<EnsembleResult> result =
        decode_result_payload(finalized.value().encoded_result, client.limits());
    EF_REQUIRE_OK(result);
    EF_CHECK(result.value().authoritative());
    for (const CandidateSnapshot& candidate : result.value().candidates) {
      if (candidate.participant.value() == 1u) {
        EF_CHECK(candidate.generation.value() >= 2u);
      }
    }
    const std::string payload(reinterpret_cast<const char*>(result.value().payload.data()),
                              result.value().payload.size());
    EF_CHECK(payload != "stale-a");
  }
  client.close();
  deployment.shutdown();
}

EF_TEST(recovered_participants_require_revalidation_before_new_work) {
  EF_REQUIRE(!g_coordinator_path.empty() && !g_worker_path.empty());
  Deployment deployment("revalidation");
  EF_REQUIRE(deployment.start_coordinator());

  Outcome<EnsembleClient> connected = deployment.connect_client();
  EF_REQUIRE_OK(connected);
  EnsembleClient client = std::move(connected).value();
  Outcome<DefineEnsembleAck> defined = client.define_ensemble(multiprocess_spec());
  EF_REQUIRE_OK(defined);
  EF_REQUIRE(deployment.start_worker("candidate_a", 1u, "CANDIDATE", "ok:value=alpha", 0u, 52001u));

  Outcome<OpenExecutionAck> opened = client.open_execution(EnsembleId(1), defined.value().generation);
  EF_REQUIRE_OK(opened);
  EF_REQUIRE(opened.value().accepted);

  bool authoritative_before = false;
  {
    Outcome<InspectResponseMessage> report = client.inspect(EnsembleId(1));
    EF_REQUIRE_OK(report);
    Outcome<std::string> text = decode_report(report.value().encoded_report, client.limits());
    EF_REQUIRE_OK(text);
    authoritative_before = participant_authoritative(text.value(), 1u, true);
  }
  EF_CHECK(authoritative_before);
  client.close();

  deployment.stop_workers();
  deployment.coordinator.terminate();
  EF_REQUIRE(deployment.start_coordinator());

  Outcome<EnsembleClient> reconnected = deployment.connect_client();
  EF_REQUIRE_OK(reconnected);
  EnsembleClient restarted = std::move(reconnected).value();
  Outcome<InspectResponseMessage> after = restarted.inspect(EnsembleId(1));
  EF_REQUIRE_OK(after);
  Outcome<std::string> text = decode_report(after.value().encoded_report, restarted.limits());
  EF_REQUIRE_OK(text);
  // Recovered dynamic evidence is never silently current.
  EF_CHECK(text.value().find("REVALIDATION_REQUIRED") != std::string::npos);
  EF_CHECK(participant_authoritative(text.value(), 1u, false));
  restarted.close();
  deployment.shutdown();
}

EF_TEST(a_coordinator_restart_advances_the_epoch_and_preserves_committed_results) {
  EF_REQUIRE(!g_coordinator_path.empty() && !g_worker_path.empty());
  Deployment deployment("restart");
  EF_REQUIRE(deployment.start_coordinator());

  Outcome<EnsembleClient> connected = deployment.connect_client();
  EF_REQUIRE_OK(connected);
  EnsembleClient client = std::move(connected).value();
  Outcome<DefineEnsembleAck> defined = client.define_ensemble(multiprocess_spec());
  EF_REQUIRE_OK(defined);
  EF_REQUIRE(defined.value().accepted);
  EF_REQUIRE(deployment.start_worker("candidate_a", 1u, "CANDIDATE", "ok:value=durable", 0u, 53001u));
  EF_REQUIRE(deployment.start_worker("candidate_b", 2u, "CANDIDATE", "ok:value=other", 0u, 53003u));
  EF_REQUIRE(deployment.start_worker("judge", 101u, "JUDGE",
                                     "evaluate:judgment=accept,score=0.5;"
                                     "evaluate:judgment=reject,score=0.1",
                                     0u, 53002u));

  Outcome<OpenExecutionAck> opened = client.open_execution(EnsembleId(1), defined.value().generation);
  EF_REQUIRE_OK(opened);
  EF_REQUIRE(opened.value().accepted);
  Outcome<FinalizeResponseMessage> finalized =
      client.finalize(EnsembleId(1), defined.value().generation);
  EF_REQUIRE_OK(finalized);
  EF_REQUIRE(finalized.value().accepted);
  const CoordinatorEpoch first_epoch = client.coordinator_epoch();
  std::uint64_t committed_fingerprint = 0;
  if (!finalized.value().encoded_result.empty()) {
    Outcome<EnsembleResult> result =
        decode_result_payload(finalized.value().encoded_result, client.limits());
    EF_REQUIRE_OK(result);
    committed_fingerprint = result.value().commit_fingerprint;
  }
  client.close();

  deployment.stop_workers();
  deployment.coordinator.terminate();
  EF_REQUIRE(deployment.start_coordinator());

  Outcome<EnsembleClient> reconnected = deployment.connect_client();
  EF_REQUIRE_OK(reconnected);
  EnsembleClient restarted = std::move(reconnected).value();
  EF_CHECK(restarted.coordinator_epoch().value() > first_epoch.value());

  Outcome<ResultResponseMessage> recovered =
      restarted.query_result(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(recovered);
  EF_CHECK(recovered.value().has_result);
  if (recovered.value().has_result) {
    Outcome<EnsembleResult> result =
        decode_result_payload(recovered.value().encoded_result, restarted.limits());
    EF_REQUIRE_OK(result);
    EF_CHECK(result.value().authoritative());
    EF_CHECK_EQ(result.value().commit_fingerprint, committed_fingerprint);
  }
  restarted.close();
  deployment.shutdown();
}

int main(int argc, char** argv) {
  if (argc > 1) {
    g_coordinator_path = argv[1];
  }
  if (argc > 2) {
    g_worker_path = argv[2];
  }
  return ef_test::run_all(argc, argv);
}
