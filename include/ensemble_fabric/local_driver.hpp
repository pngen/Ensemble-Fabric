#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "ensemble_fabric/authority.hpp"
#include "ensemble_fabric/fabric.hpp"
#include "ensemble_fabric/ids.hpp"
#include "ensemble_fabric/outcome.hpp"
#include "ensemble_fabric/participant.hpp"
#include "ensemble_fabric/reference_backend.hpp"
#include "ensemble_fabric/version.hpp"

namespace ensemble_fabric {

/// One participant served inside the current process.
struct LocalParticipant {
  ParticipantId id;
  ParticipantGeneration generation;
  WorkerId worker;
  WorkerBootId worker_boot;
  RoleSet roles;
  CompatibilityProfile profile;
  std::string domain;
  std::shared_ptr<ParticipantBackend> backend;
  bool registered{false};
  bool alive{true};

  [[nodiscard]] bool authoritative() const noexcept { return registered && alive; }
};

/// Executes the actions an EnsembleFabric produces against participants that
/// live in the same process.
///
/// This is the single-process counterpart of the distributed coordinator: it is
/// the same contract (the runtime decides what may happen; the driver only
/// transports), which makes it suitable for embedding, examples, tests, and
/// single-process deployments.  It owns no authority of its own.
class LocalEnsembleDriver {
 public:
  explicit LocalEnsembleDriver(EnsembleFabric& fabric) : fabric_(fabric) {}

  [[nodiscard]] Outcome<EnsembleGeneration> define(EnsembleSpec spec) {
    return fabric_.define_ensemble(std::move(spec));
  }

  [[nodiscard]] Outcome<EnsembleExecutionId> open(EnsembleId ensemble,
                                                  EnsembleGeneration generation) {
    return fabric_.open_execution(ensemble, generation);
  }

  /// Adds a participant and returns a reference that stays valid until the
  /// driver is destroyed.
  LocalParticipant& attach(EnsembleId ensemble, EnsembleGeneration ensemble_generation,
                           const ParticipantSpec& declaration,
                           std::shared_ptr<ParticipantBackend> backend, WorkerId worker,
                           WorkerBootId worker_boot, CompatibilityProfile profile = {}) {
    LocalParticipant participant;
    participant.id = declaration.id;
    participant.roles = declaration.roles;
    participant.domain = declaration.domain;
    participant.backend = std::move(backend);
    participant.worker = worker;
    participant.worker_boot = worker_boot;
    participant.profile = profile;
    if (participant.profile.protocol_version == 0u) {
      participant.profile.protocol_version = protocol_version;
    }
    participant.profile.task_class = declaration.compatibility.required_task_class;
    participants_.push_back(std::move(participant));
    ensemble_ = ensemble;
    ensemble_generation_ = ensemble_generation;
    return participants_.back();
  }

  [[nodiscard]] Status register_participant(LocalParticipant& participant) {
    ParticipantRegistration registration;
    registration.ensemble = ensemble_;
    registration.ensemble_generation = ensemble_generation_;
    registration.participant = participant.id;
    registration.worker = participant.worker;
    registration.worker_boot = participant.worker_boot;
    registration.coordinator_epoch = fabric_.epoch();
    registration.profile = participant.profile;
    registration.claimed_roles = participant.roles;
    Outcome<ParticipantGeneration> generation = fabric_.register_participant(registration);
    if (!generation.ok()) {
      return fail(generation.code(), generation.error().message);
    }
    participant.generation = generation.value();
    participant.registered = true;
    participant.alive = true;
    return ok_status();
  }

  [[nodiscard]] Status register_all() {
    for (LocalParticipant& participant : participants_) {
      const Status status = register_participant(participant);
      if (!status.ok()) {
        return status;
      }
    }
    return ok_status();
  }

  [[nodiscard]] ParticipantAuthority authority_of(const LocalParticipant& participant) const {
    ParticipantAuthority authority;
    authority.ensemble = ensemble_;
    authority.ensemble_generation = ensemble_generation_;
    authority.participant = participant.id;
    authority.participant_generation = participant.generation;
    authority.worker = participant.worker;
    authority.worker_boot = participant.worker_boot;
    authority.coordinator_epoch = fabric_.epoch();
    return authority;
  }

  [[nodiscard]] LocalParticipant* find(ParticipantId id) {
    for (LocalParticipant& participant : participants_) {
      if (participant.id == id) {
        return &participant;
      }
    }
    return nullptr;
  }

  /// Executes one round of actions.  Returns how many actions were performed.
  [[nodiscard]] Outcome<std::uint32_t> step();

  /// Runs until the runtime has no further actions to offer.
  [[nodiscard]] Status run(std::uint32_t max_rounds = 4096u);

  /// Simulates a real process death: the participant stops being able to serve
  /// work and its authority is reported to the runtime.
  [[nodiscard]] Status kill(LocalParticipant& participant, std::string detail = "process died");

  [[nodiscard]] EnsembleFabric& fabric() noexcept { return fabric_; }
  [[nodiscard]] std::deque<LocalParticipant>& participants() noexcept { return participants_; }

 private:
  [[nodiscard]] bool perform_dispatch(const DispatchAction& dispatch);
  [[nodiscard]] bool perform_evaluation(const EvaluationAction& action);

  EnsembleFabric& fabric_;
  EnsembleId ensemble_;
  EnsembleGeneration ensemble_generation_;
  /// A deque keeps every reference handed out by attach() valid for the
  /// lifetime of the driver.
  std::deque<LocalParticipant> participants_;
};

}  // namespace ensemble_fabric
