#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "ensemble_fabric/client.hpp"
#include "ensemble_fabric/version.hpp"

namespace {

struct Options {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  std::string mode;
  std::uint64_t ensemble{1};
  std::uint64_t generation{1};
  std::uint32_t candidates{3};
  std::uint32_t judges{1};
  std::string reason{"controller requested cancellation"};
};

void usage() {
  std::cout << "ensemble_client - Ensemble Fabric controller\n"
               "  --coordinator <host:port>  coordinator endpoint (required)\n"
               "  --inspect <ensemble>       print the current inspection report\n"
               "  --result <ensemble>        print the committed result\n"
               "  --cancel <ensemble>        cancel the current execution\n"
               "  --demo-parallel <n>        define, open, and finalize an n-candidate\n"
               "                             parallel ensemble with <judges> judges\n"
               "  --generation <n>           ensemble generation (default 1)\n"
               "  --judges <n>               judge participants for --demo-parallel\n";
}

[[nodiscard]] ensemble_fabric::EnsembleSpec build_parallel_spec(std::uint64_t ensemble_id,
                                                               std::uint32_t candidates,
                                                               std::uint32_t judges) {
  ensemble_fabric::EnsembleSpec spec;
  spec.id = ensemble_fabric::EnsembleId(ensemble_id);
  spec.label = "client-demo-parallel";
  spec.task_class = "demo";
  ensemble_fabric::StageSpec stage;
  stage.id = ensemble_fabric::StageId(1);
  stage.label = "primary";
  stage.mode = ensemble_fabric::TopologyMode::parallel;
  for (std::uint32_t index = 0; index < candidates; ++index) {
    ensemble_fabric::ParticipantSpec participant;
    participant.id = ensemble_fabric::ParticipantId(index + 1u);
    participant.label = "candidate-" + std::to_string(index + 1u);
    participant.roles.insert(ensemble_fabric::ParticipantRole::candidate);
    participant.required = false;
    participant.domain = "demo";
    spec.participants.push_back(participant);
    stage.participants.push_back(participant.id);
  }
  for (std::uint32_t index = 0; index < judges; ++index) {
    ensemble_fabric::ParticipantSpec judge;
    judge.id = ensemble_fabric::ParticipantId(1000u + index + 1u);
    judge.label = "judge-" + std::to_string(index + 1u);
    judge.roles.insert(ensemble_fabric::ParticipantRole::judge);
    judge.required = true;
    judge.domain = "demo";
    spec.participants.push_back(judge);
  }
  spec.stages.push_back(stage);
  spec.quorum.min_participants = 1u;
  spec.quorum.min_evaluations = judges;
  spec.quorum.min_valid_candidates = 1u;
  spec.quorum.vote_basis = ensemble_fabric::VoteBasis::required_participants;
  spec.quorum.count_optional_participants = false;
  spec.aggregation.kind = ensemble_fabric::AggregationKind::judge_arbitration;
  spec.aggregation.require_judge_acceptance = true;
  spec.arbitration.factors = {ensemble_fabric::ArbitrationFactor::judge_acceptance_ratio,
                             ensemble_fabric::ArbitrationFactor::aggregate_score};
  spec.arbitration.tie_break = ensemble_fabric::TieBreakPolicy::lowest_candidate_id;
  spec.evidence.min_evaluations_per_candidate = 1u;
  spec.judging_criteria = {ensemble_fabric::CriterionRef{1u, 1u}};
  spec.deterministic_seed = 7u;
  return spec;
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
        std::cerr << "ensemble_client: --coordinator expects host:port\n";
        return 2;
      }
      options.host = value.substr(0, colon);
      options.port =
          static_cast<std::uint16_t>(std::strtoul(value.substr(colon + 1u).c_str(), nullptr, 10));
    } else if (argument == "--inspect") {
      std::string value;
      next(value);
      options.mode = "inspect";
      options.ensemble = std::strtoull(value.c_str(), nullptr, 10);
    } else if (argument == "--result") {
      std::string value;
      next(value);
      options.mode = "result";
      options.ensemble = std::strtoull(value.c_str(), nullptr, 10);
    } else if (argument == "--cancel") {
      std::string value;
      next(value);
      options.mode = "cancel";
      options.ensemble = std::strtoull(value.c_str(), nullptr, 10);
    } else if (argument == "--demo-parallel") {
      std::string value;
      next(value);
      options.mode = "demo";
      options.candidates = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (argument == "--judges") {
      std::string value;
      next(value);
      options.judges = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (argument == "--generation") {
      std::string value;
      next(value);
      options.generation = std::strtoull(value.c_str(), nullptr, 10);
    } else if (argument == "--help" || argument == "-h") {
      usage();
      return 0;
    } else {
      std::cerr << "ensemble_client: unknown argument '" << argument << "'\n";
      usage();
      return 2;
    }
  }
  if (options.port == 0 || options.mode.empty()) {
    usage();
    return 2;
  }

  ensemble_fabric::Endpoint endpoint;
  endpoint.host = options.host;
  endpoint.port = options.port;
  ensemble_fabric::Outcome<ensemble_fabric::EnsembleClient> connected =
      ensemble_fabric::EnsembleClient::connect(endpoint);
  if (!connected.ok()) {
    std::cerr << "ensemble_client: " << connected.error().message << "\n";
    return 3;
  }
  ensemble_fabric::EnsembleClient client = std::move(connected).value();
  const ensemble_fabric::EnsembleId ensemble(options.ensemble);
  const ensemble_fabric::EnsembleGeneration generation(options.generation);

  if (options.mode == "inspect") {
    ensemble_fabric::Outcome<ensemble_fabric::InspectResponseMessage> report =
        client.inspect(ensemble);
    if (!report.ok()) {
      std::cerr << "ensemble_client: " << report.error().message << "\n";
      return 4;
    }
    ensemble_fabric::Outcome<std::string> text =
        ensemble_fabric::decode_report(report.value().encoded_report, client.limits());
    if (!text.ok()) {
      std::cerr << "ensemble_client: " << text.error().message << "\n";
      return 4;
    }
    std::cout << text.value() << std::endl;
    return 0;
  }
  if (options.mode == "result") {
    ensemble_fabric::Outcome<ensemble_fabric::ResultResponseMessage> response =
        client.query_result(ensemble, generation);
    if (!response.ok()) {
      std::cerr << "ensemble_client: " << response.error().message << "\n";
      return 4;
    }
    if (!response.value().has_result) {
      std::cout << "no committed result for ensemble " << options.ensemble << std::endl;
      return 1;
    }
    ensemble_fabric::Outcome<ensemble_fabric::EnsembleResult> result =
        ensemble_fabric::decode_result_payload(response.value().encoded_result, client.limits());
    if (!result.ok()) {
      std::cerr << "ensemble_client: " << result.error().message << "\n";
      return 4;
    }
    std::cout << result.value().render() << std::endl;
    return result.value().authoritative() ? 0 : 5;
  }
  if (options.mode == "cancel") {
    const ensemble_fabric::Status cancelled =
        client.cancel_ensemble(ensemble, generation, options.reason);
    if (!cancelled.ok()) {
      std::cerr << "ensemble_client: " << cancelled.error().message << "\n";
      return 4;
    }
    std::cout << "cancellation recorded" << std::endl;
    return 0;
  }

  // demo mode: define, open, and finalize.
  const ensemble_fabric::EnsembleSpec spec = build_parallel_spec(options.ensemble, options.candidates,
                                                                 options.judges);
  ensemble_fabric::Outcome<ensemble_fabric::DefineEnsembleAck> defined =
      client.define_ensemble(spec);
  if (!defined.ok() || !defined.value().accepted) {
    std::cerr << "ensemble_client: definition rejected: "
              << (defined.ok() ? defined.value().detail : defined.error().message) << "\n";
    return 4;
  }
  ensemble_fabric::Outcome<ensemble_fabric::OpenExecutionAck> opened =
      client.open_execution(ensemble, defined.value().generation);
  if (!opened.ok() || !opened.value().accepted) {
    std::cerr << "ensemble_client: execution rejected: "
              << (opened.ok() ? opened.value().detail : opened.error().message) << "\n";
    return 4;
  }
  ensemble_fabric::Outcome<ensemble_fabric::FinalizeResponseMessage> finalized =
      client.finalize(ensemble, defined.value().generation);
  if (!finalized.ok()) {
    std::cerr << "ensemble_client: finalize failed: " << finalized.error().message << "\n";
    return 4;
  }
  std::cout << "finalize decision="
            << ensemble_fabric::to_string(finalized.value().decision)
            << " accepted=" << (finalized.value().accepted ? "true" : "false")
            << " detail=" << finalized.value().detail << std::endl;
  if (!finalized.value().encoded_result.empty()) {
    ensemble_fabric::Outcome<ensemble_fabric::EnsembleResult> result =
        ensemble_fabric::decode_result_payload(finalized.value().encoded_result, client.limits());
    if (result.ok()) {
      std::cout << result.value().render() << std::endl;
    }
  }
  return finalized.value().accepted ? 0 : 5;
}
