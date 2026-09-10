#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "ensemble_fabric/client.hpp"
#include "ensemble_fabric/fabric.hpp"
#include "ensemble_fabric/version.hpp"

namespace {

void usage() {
  std::cout << "ensemble_inspect - inspect durable ensemble state or a running coordinator\n"
               "  --file <path>              read a persisted state file\n"
               "  --coordinator <host:port>  query a running coordinator\n"
               "  --ensemble <id>            restrict output to one ensemble\n"
               "  --list                     list ensembles only\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string file;
  std::string host;
  std::uint16_t port = 0;
  std::uint64_t ensemble = 0;
  bool list_only = false;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    auto next = [&](std::string& target) {
      if (index + 1 < argc) {
        target = argv[++index];
      }
    };
    if (argument == "--file") {
      next(file);
    } else if (argument == "--coordinator") {
      std::string value;
      next(value);
      const std::size_t colon = value.rfind(':');
      if (colon == std::string::npos) {
        std::cerr << "ensemble_inspect: --coordinator expects host:port\n";
        return 2;
      }
      host = value.substr(0, colon);
      port = static_cast<std::uint16_t>(std::strtoul(value.substr(colon + 1u).c_str(), nullptr, 10));
    } else if (argument == "--ensemble") {
      std::string value;
      next(value);
      ensemble = std::strtoull(value.c_str(), nullptr, 10);
    } else if (argument == "--list") {
      list_only = true;
    } else if (argument == "--help" || argument == "-h") {
      usage();
      return 0;
    } else {
      std::cerr << "ensemble_inspect: unknown argument '" << argument << "'\n";
      usage();
      return 2;
    }
  }

  if (!file.empty()) {
    ensemble_fabric::EnsembleFabric fabric;
    ensemble_fabric::Outcome<ensemble_fabric::CoordinatorEpoch> recovered =
        fabric.recover_state(file);
    if (!recovered.ok()) {
      std::cerr << "ensemble_inspect: " << recovered.error().message << "\n";
      return 3;
    }
    std::cout << "recovered state epoch=" << recovered.value().to_string() << std::endl;
    for (const ensemble_fabric::EnsembleSummary& summary : fabric.list_ensembles()) {
      if (ensemble != 0u && summary.id.value() != ensemble) {
        continue;
      }
      std::cout << "ensemble " << summary.id.to_string() << " generation "
                << summary.generation.to_string() << " status "
                << ensemble_fabric::to_string(summary.execution_status)
                << " participants=" << summary.participants << " candidates=" << summary.candidates
                << " evaluations=" << summary.evaluations
                << " result=" << (summary.has_result ? "yes" : "no") << std::endl;
      if (list_only) {
        continue;
      }
      ensemble_fabric::Outcome<ensemble_fabric::EnsembleSnapshot> snapshot = fabric.inspect(summary.id);
      if (snapshot.ok()) {
        std::cout << snapshot.value().render() << std::endl;
      }
    }
    return 0;
  }

  if (port != 0) {
    ensemble_fabric::Endpoint endpoint;
    endpoint.host = host;
    endpoint.port = port;
    ensemble_fabric::Outcome<ensemble_fabric::EnsembleClient> connected =
        ensemble_fabric::EnsembleClient::connect(endpoint);
    if (!connected.ok()) {
      std::cerr << "ensemble_inspect: " << connected.error().message << "\n";
      return 3;
    }
    ensemble_fabric::EnsembleClient client = std::move(connected).value();
    if (ensemble == 0u) {
      std::cerr << "ensemble_inspect: --ensemble is required when querying a coordinator\n";
      return 2;
    }
    ensemble_fabric::Outcome<ensemble_fabric::InspectResponseMessage> report =
        client.inspect(ensemble_fabric::EnsembleId(ensemble));
    if (!report.ok()) {
      std::cerr << "ensemble_inspect: " << report.error().message << "\n";
      return 4;
    }
    ensemble_fabric::Outcome<std::string> text =
        ensemble_fabric::decode_report(report.value().encoded_report, client.limits());
    if (!text.ok()) {
      std::cerr << "ensemble_inspect: " << text.error().message << "\n";
      return 4;
    }
    std::cout << text.value() << std::endl;
    return 0;
  }

  usage();
  return 2;
}
