#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ensemble_fabric/reference_backend.hpp"
#include "ensemble_fabric/version.hpp"
#include "ensemble_fabric/worker.hpp"

namespace {

struct Options {
  std::string coordinator_host{"127.0.0.1"};
  std::uint16_t coordinator_port{0};
  std::uint64_t worker{0};
  std::uint64_t worker_boot{0};
  std::uint64_t ensemble{1};
  std::uint64_t ensemble_generation{1};
  std::uint64_t participant{0};
  std::string roles{"CANDIDATE"};
  std::string label{"worker"};
  std::string domain;
  std::string script;
  std::string ready_file;
  std::uint32_t capability_mask{0};
  std::uint32_t task_class{0};
  std::uint32_t model_family{0};
  std::uint32_t backend_features{0};
};

void usage() {
  std::cout << "ensemble_worker - Ensemble Fabric reference participant process\n"
               "  --coordinator <host:port>  coordinator endpoint (required)\n"
               "  --worker <id>              worker identity (default: process id)\n"
               "  --boot <id>                worker boot identity (default: fresh)\n"
               "  --ensemble <id>            ensemble identity (default 1)\n"
               "  --ensemble-generation <n>  ensemble generation (default 1)\n"
               "  --participant <id>         logical participant identity (required)\n"
               "  --roles <A|B>              claimed roles (default CANDIDATE)\n"
               "  --script <program>         synthetic reference program\n"
               "  --label <text>             worker label\n"
               "  --domain <text>            specialist domain\n"
               "  --capabilities <mask>      capability bit mask\n"
               "  --task-class <id>          task class\n"
               "  --model-family <id>        model family\n"
               "  --backend-features <mask>  backend feature mask\n"
               "  --ready-file <path>        written once registration succeeds\n";
}

[[nodiscard]] std::optional<ensemble_fabric::RoleSet> parse_roles(const std::string& text) {
  ensemble_fabric::RoleSet roles = ensemble_fabric::RoleSet::none();
  std::size_t start = 0;
  for (std::size_t index = 0; index <= text.size(); ++index) {
    if (index == text.size() || text[index] == '|' || text[index] == ',') {
      const std::string token = text.substr(start, index - start);
      start = index + 1u;
      if (token.empty()) {
        continue;
      }
      const std::optional<ensemble_fabric::ParticipantRole> role =
          ensemble_fabric::parse_role(token);
      if (!role.has_value()) {
        return std::nullopt;
      }
      roles.insert(role.value());
    }
  }
  if (roles.empty()) {
    return std::nullopt;
  }
  return roles;
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
    if (argument == "--coordinator") {
      std::string value;
      next(value);
      const std::size_t colon = value.rfind(':');
      if (colon == std::string::npos) {
        std::cerr << "ensemble_worker: --coordinator expects host:port\n";
        return 2;
      }
      options.coordinator_host = value.substr(0, colon);
      options.coordinator_port = static_cast<std::uint16_t>(
          std::strtoul(value.substr(colon + 1u).c_str(), nullptr, 10));
    } else if (argument == "--worker") {
      std::string value;
      next(value);
      options.worker = std::strtoull(value.c_str(), nullptr, 10);
    } else if (argument == "--boot") {
      std::string value;
      next(value);
      options.worker_boot = std::strtoull(value.c_str(), nullptr, 10);
    } else if (argument == "--ensemble") {
      std::string value;
      next(value);
      options.ensemble = std::strtoull(value.c_str(), nullptr, 10);
    } else if (argument == "--ensemble-generation") {
      std::string value;
      next(value);
      options.ensemble_generation = std::strtoull(value.c_str(), nullptr, 10);
    } else if (argument == "--participant") {
      std::string value;
      next(value);
      options.participant = std::strtoull(value.c_str(), nullptr, 10);
    } else if (argument == "--roles") {
      next(options.roles);
    } else if (argument == "--label") {
      next(options.label);
    } else if (argument == "--domain") {
      next(options.domain);
    } else if (argument == "--script") {
      next(options.script);
    } else if (argument == "--ready-file") {
      next(options.ready_file);
    } else if (argument == "--capabilities") {
      std::string value;
      next(value);
      options.capability_mask = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (argument == "--task-class") {
      std::string value;
      next(value);
      options.task_class = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (argument == "--model-family") {
      std::string value;
      next(value);
      options.model_family = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (argument == "--backend-features") {
      std::string value;
      next(value);
      options.backend_features = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (argument == "--help" || argument == "-h") {
      usage();
      return 0;
    } else {
      std::cerr << "ensemble_worker: unknown argument '" << argument << "'\n";
      usage();
      return 2;
    }
  }
  if (options.coordinator_port == 0 || options.participant == 0) {
    std::cerr << "ensemble_worker: --coordinator and --participant are required\n";
    usage();
    return 2;
  }
  const std::optional<ensemble_fabric::RoleSet> roles = parse_roles(options.roles);
  if (!roles.has_value()) {
    std::cerr << "ensemble_worker: --roles names an unknown role\n";
    return 2;
  }
  ensemble_fabric::Outcome<ensemble_fabric::ReferenceProgram> program =
      ensemble_fabric::ReferenceProgram::parse(options.script);
  if (!program.ok()) {
    std::cerr << "ensemble_worker: " << program.error().message << "\n";
    return 2;
  }

  ensemble_fabric::WorkerConfig config;
  config.coordinator.host = options.coordinator_host;
  config.coordinator.port = options.coordinator_port;
  config.worker = ensemble_fabric::WorkerId(options.worker);
  config.worker_boot = ensemble_fabric::WorkerBootId(options.worker_boot);
  config.label = options.label;
  config.profile.protocol_version = ensemble_fabric::protocol_version;
  config.profile.capability_mask = options.capability_mask;
  config.profile.task_class = options.task_class;
  config.profile.model_family = options.model_family;
  config.profile.backend_features = options.backend_features;
  ensemble_fabric::ParticipantBinding binding;
  binding.ensemble = ensemble_fabric::EnsembleId(options.ensemble);
  binding.ensemble_generation = ensemble_fabric::EnsembleGeneration(options.ensemble_generation);
  binding.participant = ensemble_fabric::ParticipantId(options.participant);
  binding.roles = roles.value();
  binding.label = options.label;
  config.bindings.push_back(binding);

  auto backend = std::make_shared<ensemble_fabric::ReferenceBackend>(std::move(program).value());
  ensemble_fabric::EnsembleWorker worker(config, backend);
  const ensemble_fabric::Status registered = worker.connect_and_register();
  if (!registered.ok()) {
    std::cerr << "ensemble_worker: registration failed: " << registered.error().message << "\n";
    return 3;
  }
  std::cout << "ensemble_worker boot=" << worker.worker_boot().to_string()
            << " participant=" << options.participant << " registered" << std::endl;
  if (!options.ready_file.empty()) {
    std::ofstream stream(options.ready_file, std::ios::trunc);
    stream << worker.worker_boot().to_string() << "\n";
  }
  const ensemble_fabric::Status served = worker.serve();
  if (!served.ok()) {
    std::cerr << "ensemble_worker: serve ended: " << served.error().message << "\n";
    return 4;
  }
  return 0;
}
