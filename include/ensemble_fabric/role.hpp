#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ensemble_fabric/ids.hpp"

namespace ensemble_fabric {

/// Participant roles are semantic, not decorative.  A role decides which work a
/// participant may be dispatched, which evidence it may publish, and how its
/// evidence enters quorum and arbitration.
enum class ParticipantRole : std::uint8_t {
  candidate = 1,
  specialist = 2,
  judge = 3,
  verifier = 4,
  fallback = 5,
};

[[nodiscard]] std::string_view to_string(ParticipantRole role) noexcept;
[[nodiscard]] std::optional<ParticipantRole> parse_role(std::string_view text) noexcept;

/// Roles whose participants produce candidate payloads.
[[nodiscard]] bool role_produces_candidates(ParticipantRole role) noexcept;
/// Roles whose participants produce evaluations of other participants' work.
[[nodiscard]] bool role_evaluates(ParticipantRole role) noexcept;
/// Roles that may become the authoritative final answer when the policy allows.
[[nodiscard]] bool role_can_be_selected(ParticipantRole role) noexcept;
/// Roles that may only be activated by an explicit, generation-bound fallback
/// condition.
[[nodiscard]] bool role_requires_activation(ParticipantRole role) noexcept;

inline constexpr std::uint8_t participant_role_count = 5;

/// A small ordered set of roles.  Backed by a bitmask so that persistence and
/// wire formats stay stable when new roles are appended.
class RoleSet {
 public:
  constexpr RoleSet() noexcept = default;
  constexpr explicit RoleSet(std::uint32_t mask) noexcept : mask_(mask) {}

  [[nodiscard]] static constexpr RoleSet none() noexcept { return RoleSet(0u); }
  [[nodiscard]] static constexpr RoleSet of(ParticipantRole role) noexcept {
    return RoleSet(role_bit(role));
  }

  constexpr void insert(ParticipantRole role) noexcept { mask_ |= role_bit(role); }
  constexpr void erase(ParticipantRole role) noexcept { mask_ &= ~role_bit(role); }
  [[nodiscard]] constexpr bool contains(ParticipantRole role) const noexcept {
    return (mask_ & role_bit(role)) != 0u;
  }
  [[nodiscard]] constexpr bool empty() const noexcept { return mask_ == 0u; }
  [[nodiscard]] constexpr std::uint32_t mask() const noexcept { return mask_; }
  [[nodiscard]] std::uint32_t size() const noexcept;
  [[nodiscard]] bool contains_all(const RoleSet& other) const noexcept {
    return (mask_ & other.mask_) == other.mask_;
  }
  [[nodiscard]] bool contains_any(const RoleSet& other) const noexcept {
    return (mask_ & other.mask_) != 0u;
  }
  [[nodiscard]] RoleSet intersect(const RoleSet& other) const noexcept {
    return RoleSet(mask_ & other.mask_);
  }
  [[nodiscard]] RoleSet merge(const RoleSet& other) const noexcept {
    return RoleSet(mask_ | other.mask_);
  }

  friend constexpr bool operator==(RoleSet lhs, RoleSet rhs) noexcept {
    return lhs.mask_ == rhs.mask_;
  }
  friend constexpr bool operator!=(RoleSet lhs, RoleSet rhs) noexcept {
    return lhs.mask_ != rhs.mask_;
  }

  /// Canonical rendering: roles in enum order, "|" separated, "none" when empty.
  [[nodiscard]] std::string to_string() const;

 private:
  [[nodiscard]] static constexpr std::uint32_t role_bit(ParticipantRole role) noexcept {
    return static_cast<std::uint32_t>(1u) << (static_cast<std::uint32_t>(role) - 1u);
  }

  std::uint32_t mask_{0};
};

/// Verifier/verification outcome.  UNKNOWN is a first-class state and never
/// silently becomes PASS.
enum class VerificationState : std::uint8_t {
  pass = 1,
  fail = 2,
  abstain = 3,
  unknown = 4,
};

[[nodiscard]] std::string_view to_string(VerificationState state) noexcept;
[[nodiscard]] std::optional<VerificationState> parse_verification_state(
    std::string_view text) noexcept;

/// Categorical judgement produced alongside a score.  A score without a
/// categorical judgement is not evidence.
enum class CategoricalJudgment : std::uint8_t {
  accept = 1,
  reject = 2,
  abstain = 3,
  unknown = 4,
};

[[nodiscard]] std::string_view to_string(CategoricalJudgment judgment) noexcept;
[[nodiscard]] std::optional<CategoricalJudgment> parse_categorical_judgment(
    std::string_view text) noexcept;

/// Distinguishes a mandatory validation predicate from a ranking-only signal.
/// Hard predicates gate eligibility; ranking signals only order eligible
/// candidates.
enum class CriterionKind : std::uint8_t {
  hard_predicate = 1,
  ranking_signal = 2,
};

[[nodiscard]] std::string_view to_string(CriterionKind kind) noexcept;
[[nodiscard]] std::optional<CriterionKind> parse_criterion_kind(std::string_view text) noexcept;

/// Stable identity of an evaluation criterion together with its version.  A
/// criterion version change invalidates prior evaluations that used it.
struct CriterionRef {
  std::uint32_t id{0};
  std::uint32_t version{0};

  friend constexpr bool operator==(const CriterionRef& lhs, const CriterionRef& rhs) noexcept {
    return lhs.id == rhs.id && lhs.version == rhs.version;
  }
  friend constexpr bool operator!=(const CriterionRef& lhs, const CriterionRef& rhs) noexcept {
    return !(lhs == rhs);
  }
  friend constexpr bool operator<(const CriterionRef& lhs, const CriterionRef& rhs) noexcept {
    return lhs.id != rhs.id ? lhs.id < rhs.id : lhs.version < rhs.version;
  }
  [[nodiscard]] bool valid() const noexcept { return id != 0u; }
};

}  // namespace ensemble_fabric
