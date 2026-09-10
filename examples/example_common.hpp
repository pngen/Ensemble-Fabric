#pragma once

#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "ensemble_fabric/ensemble_fabric.hpp"
#include "ensemble_fabric/local_driver.hpp"

namespace examples {

using namespace ensemble_fabric;

[[nodiscard]] inline std::shared_ptr<ParticipantBackend> scripted(const std::string& script) {
  Outcome<ReferenceProgram> program = ReferenceProgram::parse(script);
  if (!program.ok()) {
    std::cerr << "example: invalid script: " << program.error().message << "\n";
    std::exit(2);
  }
  return std::make_shared<ReferenceBackend>(std::move(program).value());
}

inline void report(const EnsembleResult& result, const char* label) {
  std::cout << label << ": decision=" << to_string(result.decision)
            << " selected=" << (result.has_selected ? result.selected_candidate.to_string()
                                                    : std::string("none"))
            << " payload=\""
            << std::string(reinterpret_cast<const char*>(result.payload.data()),
                           result.payload.size())
            << "\"" << std::endl;
  std::cout << "  " << result.quorum.render() << std::endl;
  std::cout << "  " << result.consensus.render() << std::endl;
  std::cout << "  " << result.arbitration.render() << std::endl;
}

[[nodiscard]] inline ParticipantSpec candidate(ParticipantId id, const std::string& domain = {}) {
  ParticipantSpec participant;
  participant.id = id;
  participant.label = "candidate-" + id.to_string();
  participant.roles.insert(ParticipantRole::candidate);
  participant.domain = domain;
  return participant;
}

[[nodiscard]] inline ParticipantSpec judge(ParticipantId id) {
  ParticipantSpec participant;
  participant.id = id;
  participant.label = "judge-" + id.to_string();
  participant.roles.insert(ParticipantRole::judge);
  participant.required = true;
  participant.domain = "judging";
  return participant;
}

[[nodiscard]] inline ParticipantSpec specialist(ParticipantId id, const std::string& domain) {
  ParticipantSpec participant;
  participant.id = id;
  participant.label = "specialist-" + id.to_string();
  participant.roles.insert(ParticipantRole::specialist);
  participant.domain = domain;
  return participant;
}

[[nodiscard]] inline ParticipantSpec verifier(ParticipantId id) {
  ParticipantSpec participant;
  participant.id = id;
  participant.label = "verifier-" + id.to_string();
  participant.roles.insert(ParticipantRole::verifier);
  participant.required = true;
  participant.domain = "verification";
  return participant;
}

[[nodiscard]] inline ParticipantSpec fallback(ParticipantId id) {
  ParticipantSpec participant;
  participant.id = id;
  participant.label = "fallback-" + id.to_string();
  participant.roles.insert(ParticipantRole::fallback);
  participant.domain = "fallback";
  return participant;
}

[[nodiscard]] inline StageSpec stage(StageId id, std::vector<ParticipantId> participants,
                                     TopologyMode mode = TopologyMode::parallel) {
  StageSpec value;
  value.id = id;
  value.label = "stage-" + id.to_string();
  value.mode = mode;
  value.participants = std::move(participants);
  return value;
}

}  // namespace examples
