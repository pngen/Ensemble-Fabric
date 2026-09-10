#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "example_common.hpp"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace {

using namespace ensemble_fabric;

class Child {
 public:
  Child() = default;
  ~Child() { stop(); }
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;

  bool start(const std::string& executable, const std::vector<std::string>& arguments) {
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
    if (CreateProcessA(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &startup, &information) == FALSE) {
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

  void stop() {
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

bool await_file(const std::filesystem::path& path) {
  std::error_code error;
  for (int attempt = 0; attempt < 200000; ++attempt) {
    if (std::filesystem::exists(path, error)) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

std::string read_line(const std::filesystem::path& path) {
  std::ifstream stream(path);
  std::string value;
  std::getline(stream, value);
  return value;
}

/// Waits until the coordinator has published a usable port.  Each poll returns
/// immediately; the file is written atomically, so a visible file is complete.
std::uint16_t await_port(const std::filesystem::path& path) {
  std::error_code error;
  for (std::uint64_t attempt = 0; attempt < 4000000u; ++attempt) {
    if (std::filesystem::exists(path, error)) {
      const std::uint16_t port =
          static_cast<std::uint16_t>(std::strtoul(read_line(path).c_str(), nullptr, 10));
      if (port != 0u) {
        return port;
      }
    }
    std::this_thread::yield();
  }
  return 0;
}

}  // namespace

/// A real three-process deployment: one controller, one coordinator, and three
/// participant worker processes over loopback TCP.
int main(int argc, char** argv) {
  using namespace examples;
  std::string coordinator_path;
  std::string worker_path;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--coordinator" && index + 1 < argc) {
      coordinator_path = argv[++index];
    } else if (argument == "--worker" && index + 1 < argc) {
      worker_path = argv[++index];
    }
  }
  if (coordinator_path.empty() || worker_path.empty()) {
    std::cout << "usage: example_multiprocess --coordinator <path> --worker <path>\n"
                 "  The paths point at the ensemble_coordinator and ensemble_worker\n"
                 "  executables built alongside this example.\n";
    return 2;
  }

  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "ensemble_fabric_example_multiprocess";
  std::filesystem::create_directories(directory);
  const std::filesystem::path state = directory / "state.bin";
  const std::filesystem::path port_file = directory / "port.txt";
  std::error_code error;
  std::filesystem::remove(state, error);
  std::filesystem::remove(port_file, error);

  Child coordinator;
  if (!coordinator.start(coordinator_path,
                         {"--port", "0", "--state", state.string(), "--port-file",
                          port_file.string()})) {
    std::cerr << "example_multiprocess: cannot start the coordinator\n";
    return 1;
  }
  const std::uint16_t port = await_port(port_file);
  if (port == 0u) {
    std::cerr << "example_multiprocess: the coordinator did not report a port\n";
    return 1;
  }

  // The ensemble is defined before any participant process starts: a
  // participant registers against an ensemble generation that must already
  // exist, and the runtime never invents one on a participant's behalf.
  Endpoint target;
  target.host = "127.0.0.1";
  target.port = port;
  Outcome<EnsembleClient> connected = EnsembleClient::connect(target);
  if (!connected.ok()) {
    std::cerr << "example_multiprocess: " << connected.error().message << "\n";
    return 1;
  }
  EnsembleClient client = std::move(connected).value();

  EnsembleSpec spec;
  spec.id = EnsembleId(1);
  spec.label = "multiprocess";
  spec.task_class = "demo";
  spec.participants.push_back(candidate(ParticipantId(1)));
  spec.participants.push_back(candidate(ParticipantId(2)));
  spec.participants.push_back(judge(ParticipantId(11)));
  spec.stages.push_back(stage(StageId(1), {ParticipantId(1), ParticipantId(2)}));
  spec.quorum.min_participants = 1u;
  spec.quorum.min_evaluations = 1u;
  spec.quorum.count_optional_participants = false;
  spec.evidence.min_evaluations_per_candidate = 1u;
  spec.aggregation.kind = AggregationKind::judge_arbitration;
  spec.aggregation.require_judge_acceptance = true;
  spec.arbitration.factors = {ArbitrationFactor::judge_acceptance_ratio,
                              ArbitrationFactor::aggregate_score};
  spec.arbitration.tie_break = TieBreakPolicy::lowest_candidate_id;

  Outcome<DefineEnsembleAck> defined = client.define_ensemble(spec);
  if (!defined.ok() || !defined.value().accepted) {
    std::cerr << "example_multiprocess: definition rejected: "
              << (defined.ok() ? defined.value().detail : defined.error().message) << "\n";
    return 1;
  }

  // Participant processes start only once the ensemble generation exists, and
  // each one publishes readiness evidence before it may receive any work.
  std::vector<std::unique_ptr<Child>> workers;
  const std::string endpoint = "127.0.0.1:" + std::to_string(port);
  auto spawn_worker = [&](const std::string& name, const std::string& participant,
                          const std::string& roles, const std::string& script,
                          const std::string& worker_id) {
    auto child = std::make_unique<Child>();
    const std::filesystem::path ready = directory / (name + ".ready");
    std::filesystem::remove(ready, error);
    if (!child->start(worker_path, {"--coordinator", endpoint, "--participant", participant,
                                    "--roles", roles, "--ensemble", "1",
                                    "--ensemble-generation", "1", "--script", script,
                                    "--worker", worker_id, "--ready-file", ready.string()})) {
      return false;
    }
    workers.push_back(std::move(child));
    return await_file(ready);
  };
  if (!spawn_worker("candidate-a", "1", "CANDIDATE", "ok:value=multiprocess-alpha", "70001") ||
      !spawn_worker("candidate-b", "2", "CANDIDATE", "ok:value=multiprocess-beta", "70002") ||
      !spawn_worker("judge", "11", "JUDGE",
                    "evaluate:judgment=accept,score=0.7;evaluate:judgment=reject,score=0.2",
                    "70003")) {
    std::cerr << "example_multiprocess: a worker failed to register\n";
    return 1;
  }

  Outcome<OpenExecutionAck> opened = client.open_execution(EnsembleId(1), defined.value().generation);
  if (!opened.ok() || !opened.value().accepted) {
    std::cerr << "example_multiprocess: execution rejected\n";
    return 1;
  }
  std::cout << "multiprocess: coordinator epoch=" << client.coordinator_epoch().to_string()
            << " port=" << port << std::endl;
  Outcome<FinalizeResponseMessage> finalized = client.finalize(EnsembleId(1), defined.value().generation);
  if (!finalized.ok()) {
    std::cerr << "example_multiprocess: finalize failed: " << finalized.error().message << "\n";
    return 1;
  }
  if (!finalized.value().encoded_result.empty()) {
    Outcome<EnsembleResult> result =
        decode_result_payload(finalized.value().encoded_result, client.limits());
    if (result.ok()) {
      report(result.value(), "multiprocess");
    }
  }
  client.close();
  for (auto& worker : workers) {
    worker->stop();
  }
  coordinator.stop();
  std::filesystem::remove_all(directory, error);
  return finalized.value().accepted ? 0 : 1;
}
