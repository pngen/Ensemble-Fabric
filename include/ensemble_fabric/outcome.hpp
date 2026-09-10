#pragma once

#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace ensemble_fabric {

/// Typed failure classes.  Materially distinct states are never reduced to a
/// generic boolean or an untyped string.
enum class EnsembleError : std::uint16_t {
  ok = 0,

  // Caller/argument problems.
  invalid_argument = 1,
  resource_limit_exceeded = 2,
  unsupported_version = 3,
  not_found = 4,

  // Unknown objects.
  unknown_ensemble = 10,
  unknown_execution = 11,
  unknown_participant = 12,
  unknown_candidate = 13,
  unknown_evaluation = 14,

  // Stale authority.  Every one of these fences a distinct authority domain.
  stale_ensemble_generation = 20,
  stale_execution_generation = 21,
  stale_participant_generation = 22,
  stale_worker_boot = 23,
  stale_coordinator_epoch = 24,
  stale_candidate_generation = 25,
  stale_evaluation_generation = 26,

  // Participant problems.
  participant_not_authoritative = 30,
  participant_unavailable = 31,
  participant_incompatible = 32,
  participant_role_mismatch = 33,
  participant_duplicate = 34,
  participant_revalidation_required = 35,
  participant_failed = 36,
  required_participant_failed = 37,

  // Content problems.
  malformed_candidate = 40,
  malformed_evaluation = 41,
  malformed_frame = 42,
  malformed_persistence = 43,
  checksum_mismatch = 44,
  trailing_data = 45,
  protocol_error = 46,
  invalid_enum_value = 47,
  invalid_identity_combination = 48,
  payload_too_large = 49,

  // Lifecycle problems.
  invalid_state_transition = 60,
  duplicate_completion = 61,
  duplicate_evaluation = 62,
  conflicting_evaluation = 63,
  duplicate_dispatch = 64,
  execution_not_open = 65,
  evaluation_target_mismatch = 66,

  // Aggregate policy outcomes.
  quorum_impossible = 70,
  quorum_not_reached = 71,
  insufficient_evidence = 72,
  all_candidates_invalid = 73,
  consensus_not_reached = 74,
  disagreement_unresolved = 75,
  tie_unresolved = 76,
  fallback_not_authorized = 77,
  fallback_exhausted = 78,

  // Commit problems.
  result_already_committed = 80,
  conflicting_result_commit = 81,
  no_eligible_candidate = 82,

  // Cancellation/supersession.
  ensemble_cancelled = 90,
  ensemble_superseded = 91,
  execution_cancelled = 92,
  revalidation_required = 93,

  // Persistence/transport.
  persistence_io_error = 100,
  persistence_corruption = 101,
  transport_error = 102,

  // Internal invariants.
  internal_error = 110,
};

[[nodiscard]] std::string_view to_string(EnsembleError error) noexcept;

/// True when the failure means "the caller was not authoritative".  These are
/// the failures that must never mutate current state.
[[nodiscard]] bool is_authority_failure(EnsembleError error) noexcept;

/// True when a participant failure of this class may be retried under an
/// explicit retry policy.  Retryability is typed, never inferred from strings.
[[nodiscard]] bool is_retryable_failure(EnsembleError error) noexcept;

/// True when the failure permanently invalidates the target of the operation.
[[nodiscard]] bool is_terminal_failure(EnsembleError error) noexcept;

struct Error {
  EnsembleError code{EnsembleError::ok};
  std::string message;

  Error() = default;
  Error(EnsembleError error_code, std::string text)
      : code(error_code), message(std::move(text)) {}

  [[nodiscard]] bool ok() const noexcept { return code == EnsembleError::ok; }
};

/// A value or a typed failure.  There is no "silently defaulted" state: an
/// Outcome either holds a value or holds a non-ok error.
template <typename T>
class Outcome {
 public:
  using value_type = T;

  Outcome(T value) : value_(std::in_place, std::move(value)) {}
  Outcome(Error error) : error_(std::move(error)) {
    if (error_.code == EnsembleError::ok) {
      error_.code = EnsembleError::internal_error;
      error_.message = "Outcome constructed with an ok error code";
    }
  }

  [[nodiscard]] bool ok() const noexcept { return value_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] EnsembleError code() const noexcept { return error_.code; }
  [[nodiscard]] const Error& error() const noexcept { return error_; }

  [[nodiscard]] T& value() & { return value_.value(); }
  [[nodiscard]] const T& value() const& { return value_.value(); }
  [[nodiscard]] T&& value() && { return std::move(value_.value()); }

  [[nodiscard]] T* operator->() noexcept { return &value_.value(); }
  [[nodiscard]] const T* operator->() const noexcept { return &value_.value(); }
  [[nodiscard]] T& operator*() noexcept { return value_.value(); }
  [[nodiscard]] const T& operator*() const noexcept { return value_.value(); }

  [[nodiscard]] T value_or(T fallback) const {
    return value_.has_value() ? value_.value() : std::move(fallback);
  }

 private:
  std::optional<T> value_;
  Error error_{};
};

template <>
class Outcome<void> {
 public:
  using value_type = void;

  Outcome() noexcept = default;
  Outcome(Error error) : error_(std::move(error)) {
    if (error_.code == EnsembleError::ok) {
      error_.code = EnsembleError::internal_error;
      error_.message = "Outcome constructed with an ok error code";
    }
  }

  [[nodiscard]] static Outcome success() noexcept { return Outcome(); }

  [[nodiscard]] bool ok() const noexcept { return error_.code == EnsembleError::ok; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] EnsembleError code() const noexcept { return error_.code; }
  [[nodiscard]] const Error& error() const noexcept { return error_; }

 private:
  Error error_{};
};

using Status = Outcome<void>;

[[nodiscard]] inline Error make_error(EnsembleError code, std::string message) {
  return Error{code, std::move(message)};
}

template <typename T>
[[nodiscard]] Outcome<T> fail(EnsembleError code, std::string message) {
  return Outcome<T>(Error{code, std::move(message)});
}

[[nodiscard]] inline Status fail(EnsembleError code, std::string message) {
  return Status(Error{code, std::move(message)});
}

[[nodiscard]] inline Status ok_status() noexcept { return Status(); }

}  // namespace ensemble_fabric
