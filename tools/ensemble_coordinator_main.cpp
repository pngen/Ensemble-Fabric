#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>

#include "ensemble_fabric/coordinator.hpp"
#include "ensemble_fabric/version.hpp"

namespace {

std::atomic<bool> g_stop{false};
std::mutex g_stop_mutex;
std::condition_variable g_stop_condition;

extern "C" void handle_signal(int) {
  g_stop.store(true);
  g_stop_condition.notify_all();
}

struct Options {
  std::uint16_t port{0};
  std::string state_path;
  std::string port_file;
  bool deterministic_boot{false};
};

void usage() {
  std::cout << "ensemble_coordinator - Ensemble Fabric reference coordinator\n"
               "  --port <n>            loopback port to bind (0 selects an ephemeral port)\n"
               "  --state <path>        durable state file (enables persistence/recovery)\n"
               "  --port-file <path>    writes the bound port once listening\n"
               "  --deterministic-boot  use a fixed coordinator boot identity (tests only)\n";
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    auto next = [&](std::string& target) {
      if (index + 1 < argc) {
        target = argv[++index];
      }
    };
    if (argument == "--port") {
      std::string value;
      next(value);
      options.port = static_cast<std::uint16_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (argument == "--state") {
      next(options.state_path);
    } else if (argument == "--port-file") {
      next(options.port_file);
    } else if (argument == "--deterministic-boot") {
      options.deterministic_boot = true;
    } else if (argument == "--help" || argument == "-h") {
      usage();
      return 0;
    } else {
      std::cerr << "ensemble_coordinator: unknown argument '" << argument << "'\n";
      usage();
      return 2;
    }
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  ensemble_fabric::CoordinatorConfig config;
  config.bind.host = "127.0.0.1";
  config.bind.port = options.port;
  config.state_path = options.state_path;
  config.port_file = options.port_file;
  config.persist_on_change = !options.state_path.empty();
  if (options.deterministic_boot) {
    config.coordinator_boot = ensemble_fabric::WorkerBootId(0x0000000000C0FFEEull);
  }

  ensemble_fabric::EnsembleCoordinator coordinator(config);
  const ensemble_fabric::Status started = coordinator.start();
  if (!started.ok()) {
    std::cerr << "ensemble_coordinator: start failed: " << started.error().message << "\n";
    return 3;
  }
  std::cout << "ensemble_coordinator listening on 127.0.0.1:" << coordinator.port()
            << " epoch=" << coordinator.epoch().to_string() << std::endl;

  // The coordinator runs its own event loop; this thread only waits for a
  // termination request.  It uses a condition variable rather than polling, so
  // the process is completely idle while it serves participants.
  {
    std::unique_lock<std::mutex> lock(g_stop_mutex);
    g_stop_condition.wait(lock, []() { return g_stop.load(); });
  }
  const ensemble_fabric::Status stopped = coordinator.stop();
  if (!stopped.ok()) {
    std::cerr << "ensemble_coordinator: stop reported: " << stopped.error().message << "\n";
    return 4;
  }
  return 0;
}
