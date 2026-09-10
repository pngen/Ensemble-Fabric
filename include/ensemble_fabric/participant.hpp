#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ensemble_fabric/authority.hpp"
#include "ensemble_fabric/ids.hpp"
#include "ensemble_fabric/limits.hpp"
#include "ensemble_fabric/outcome.hpp"
#include "ensemble_fabric/role.hpp"

namespace ensemble_fabric {

/// Lifecycle of a logical participant.
///
/// `available` describes reachability; `authoritative` describes the right to
/// act.  Losing authority never implies losing availability and vice versa.
enum class ParticipantStatus : std::uint8_t {
  current = 1,
  unavailable = 2,
  failed = 3,
  fenced = 4,
  revalidation_required = 5,
  retired = 6,
};

[[nodiscard]] std::string_view to_string(ParticipantStatus status) noexcept;
[[nodiscard]] std::optional<ParticipantStatus> parse_participant_status(
    std::string_view text) noexcept;

/// Declared compatibility of one participant process.  Ensemble Fabric models
/// only the compatibility data that changes ensemble correctness; it is not a
/// universal model registry.
struct CompatibilityProfile {
  std::uint16_t protocol_version{1};
  std::uint32_t capability_mask{0};
  std::uint32_t task_class{0};
  std::uint32_t output_schema{0};
  std::uint32_t model_family{0};
  std::uint32_t backend_features{0};

  friend bool operator==(const CompatibilityProfile& lhs,
                         const CompatibilityProfile& rhs) noexcept {
    return lhs.protocol_version == rhs.protocol_version &&
           lhs.capability_mask == rhs.capability_mask && lhs.task_class == rhs.task_class &&
           lhs.output_schema == rhs.output_schema &&
           lhs.model_family == rhs.model_family &&
           lhs.backend_features == rhs.backend_features;
  }
};

/// Requirement expressed by an ensemble.  A zero field means "no constraint".
struct CompatibilityRequirement {
  std::uint16_t min_protocol_version{1};
  std::uint32_t required_capabilities{0};
  std::uint32_t required_task_class{0};
  std::uint32_t required_output_schema{0};
  std::uint32_t excluded_model_family{0};
  std::uint32_t required_backend_features{0};
  bool require_model_family_diversity{false};

  [[nodiscard]] bool constrains_task_class() const noexcept {
    return required_task_class != 0u;
  }
};

/// Returns a typed failure describing exactly which requirement was not met.
[[nodiscard]] Status check_compatibility(const CompatibilityRequirement& requirement,
                                         const CompatibilityProfile& profile);

/// Immutable declaration of one logical participant slot inside an ensemble
/// specification.
struct ParticipantSpec {
  ParticipantId id;
  std::string label;
  RoleSet roles;
  bool required{true};
  std::string domain;
  std::uint32_t priority{0};
  std::uint32_t execution_budget_units{0};
  std::optional<bool> selectable_override;
  CompatibilityRequirement compatibility;

  [[nodiscard]] bool selectable() const noexcept {
    if (selectable_override.has_value()) {
      return selectable_override.value();
    }
    return roles.contains(ParticipantRole::candidate) ||
           roles.contains(ParticipantRole::specialist) ||
           roles.contains(ParticipantRole::fallback);
  }
};

/// Registration of a physical worker process as one logical participant.
struct ParticipantRegistration {
  EnsembleId ensemble;
  EnsembleGeneration ensemble_generation;
  ParticipantId participant;
  WorkerId worker;
  WorkerBootId worker_boot;
  CoordinatorEpoch coordinator_epoch;
  CompatibilityProfile profile;
  /// Roles the worker claims.  A claim that is not granted by the ensemble
  /// specification is rejected: a worker may not promote itself into a role.
  RoleSet claimed_roles;
};

/// Point-in-time inspection view of a participant.  Returned by value: callers
/// never hold a reference into mutable runtime state.
struct ParticipantSnapshot {
  ParticipantId id;
  ParticipantGeneration generation;
  std::string label;
  RoleSet roles;
  bool required{true};
  std::string domain;
  std::uint32_t priority{0};
  ParticipantStatus status{ParticipantStatus::revalidation_required};
  WorkerId worker;
  WorkerBootId worker_boot;
  CoordinatorEpoch coordinator_epoch;
  CompatibilityProfile profile;
  std::uint32_t replacement_count{0};
  std::uint32_t candidates_completed{0};
  std::uint32_t candidates_failed{0};
  std::uint32_t candidates_abstained{0};
  std::uint32_t evaluations_submitted{0};
  std::uint32_t retries_consumed{0};
  bool fallback_activated{false};
  bool authoritative{false};
};

/// Why a logical participant stopped contributing.  Failures are typed so the
/// retry and fallback policies can act on exact causes.
enum class ParticipantFailureKind : std::uint8_t {
  unavailable = 1,
  transport_error = 2,
  malformed_output = 3,
  internal_error = 4,
  timeout_reported = 5,
  explicit_abort = 6,
};

[[nodiscard]] std::string_view to_string(ParticipantFailureKind kind) noexcept;
[[nodiscard]] std::optional<ParticipantFailureKind> parse_failure_kind(
    std::string_view text) noexcept;

/// Maps a typed participant failure onto the error class used for retry policy
/// decisions.  This mapping is total and explicit.
[[nodiscard]] EnsembleError failure_error(ParticipantFailureKind kind) noexcept;

/// A participant reference used by dispatch plans.
struct ParticipantRef {
  ParticipantId id;
  ParticipantGeneration generation;

  friend bool operator==(const ParticipantRef& lhs, const ParticipantRef& rhs) noexcept {
    return lhs.id == rhs.id && lhs.generation == rhs.generation;
  }
};

}  // namespace ensemble_fabric
