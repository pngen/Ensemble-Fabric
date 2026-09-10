#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace ensemble_fabric {

// ---------------------------------------------------------------------------
// Distinct identity domains.
//
// Ensemble Fabric refuses to collapse identities that represent materially
// different authority domains.  Each tag below is a separate compile-time
// type even when the underlying representation is identical, so that a
// ParticipantId can never be passed where a CandidateId is required.
// ---------------------------------------------------------------------------
struct EnsembleIdTag;
struct EnsembleGenerationTag;
struct EnsembleExecutionIdTag;
struct EnsembleAttemptGenerationTag;
struct ParticipantIdTag;
struct ParticipantGenerationTag;
struct ParticipantRoleIdTag;
struct CandidateIdTag;
struct CandidateGenerationTag;
struct EvaluationIdTag;
struct EvaluationGenerationTag;
struct ResultIdTag;
struct ResultGenerationTag;
struct WorkerIdTag;
struct WorkerBootIdTag;
struct CoordinatorEpochTag;
struct RequestIdTag;
struct PlanIdTag;
struct PlanGenerationTag;
struct StageIdTag;
struct SequenceTag;

/// A strongly typed 64-bit identity.
///
/// The zero value is reserved and means "no identity"; it is never valid for a
/// live object.  Generation types use exactly the same representation, but live
/// in their own tag so that a candidate identity cannot be used as a generation.
template <typename Tag, typename Rep = std::uint64_t>
class StrongId {
 public:
  using tag_type = Tag;
  using rep_type = Rep;

  constexpr StrongId() noexcept = default;
  constexpr explicit StrongId(Rep value) noexcept : value_(value) {}

  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != Rep{0}; }

  friend constexpr bool operator==(StrongId lhs, StrongId rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend constexpr bool operator!=(StrongId lhs, StrongId rhs) noexcept {
    return lhs.value_ != rhs.value_;
  }
  friend constexpr bool operator<(StrongId lhs, StrongId rhs) noexcept {
    return lhs.value_ < rhs.value_;
  }
  friend constexpr bool operator>(StrongId lhs, StrongId rhs) noexcept {
    return lhs.value_ > rhs.value_;
  }
  friend constexpr bool operator<=(StrongId lhs, StrongId rhs) noexcept {
    return lhs.value_ <= rhs.value_;
  }
  friend constexpr bool operator>=(StrongId lhs, StrongId rhs) noexcept {
    return lhs.value_ >= rhs.value_;
  }

  /// Canonical decimal rendering.  Never produces leading zeroes.
  [[nodiscard]] std::string to_string() const {
    if constexpr (std::is_signed_v<Rep>) {
      return std::to_string(static_cast<long long>(value_));
    } else {
      return std::to_string(static_cast<unsigned long long>(value_));
    }
  }

  /// Strict decimal parse: rejects empty input, signs, whitespace, trailing
  /// characters, and values that do not fit the representation.  The zero value
  /// is rejected because it denotes "no identity".
  [[nodiscard]] static std::optional<StrongId> parse(std::string_view text) noexcept {
    if (text.empty() || text.size() > 20u) {
      return std::nullopt;
    }
    Rep acc = Rep{0};
    for (const char ch : text) {
      if (ch < '0' || ch > '9') {
        return std::nullopt;
      }
      const Rep digit = static_cast<Rep>(ch - '0');
      if (acc > (static_cast<Rep>(-1) - digit) / Rep{10}) {
        return std::nullopt;
      }
      acc = static_cast<Rep>(acc * Rep{10} + digit);
    }
    if (acc == Rep{0}) {
      return std::nullopt;
    }
    return StrongId(acc);
  }

 private:
  Rep value_{0};
};

template <typename Tag, typename Rep = std::uint64_t>
struct StrongIdHash {
  [[nodiscard]] std::size_t operator()(StrongId<Tag, Rep> id) const noexcept {
    return std::hash<Rep>{}(id.value());
  }
};

template <typename Tag, typename Rep = std::uint64_t>
struct StrongIdLess {
  [[nodiscard]] constexpr bool operator()(StrongId<Tag, Rep> lhs,
                                          StrongId<Tag, Rep> rhs) const noexcept {
    return lhs.value() < rhs.value();
  }
};

using EnsembleId = StrongId<EnsembleIdTag>;
using EnsembleGeneration = StrongId<EnsembleGenerationTag>;
using EnsembleExecutionId = StrongId<EnsembleExecutionIdTag>;
using EnsembleAttemptGeneration = StrongId<EnsembleAttemptGenerationTag>;
using ParticipantId = StrongId<ParticipantIdTag>;
using ParticipantGeneration = StrongId<ParticipantGenerationTag>;
using ParticipantRoleId = StrongId<ParticipantRoleIdTag>;
using CandidateId = StrongId<CandidateIdTag>;
using CandidateGeneration = StrongId<CandidateGenerationTag>;
using EvaluationId = StrongId<EvaluationIdTag>;
using EvaluationGeneration = StrongId<EvaluationGenerationTag>;
using ResultId = StrongId<ResultIdTag>;
using ResultGeneration = StrongId<ResultGenerationTag>;
using WorkerId = StrongId<WorkerIdTag>;
using WorkerBootId = StrongId<WorkerBootIdTag>;
using CoordinatorEpoch = StrongId<CoordinatorEpochTag>;
using RequestId = StrongId<RequestIdTag>;
using PlanId = StrongId<PlanIdTag>;
using PlanGeneration = StrongId<PlanGenerationTag>;
using StageId = StrongId<StageIdTag>;
using Sequence = StrongId<SequenceTag>;

/// Advances a generation by exactly one step.  Returns false when the
/// generation space is exhausted rather than wrapping silently, because a
/// wrapped generation would resurrect historical authority.
template <typename Tag, typename Rep>
[[nodiscard]] bool advance_generation(StrongId<Tag, Rep>& generation) noexcept {
  const Rep current = generation.value();
  if (current == static_cast<Rep>(-1)) {
    return false;
  }
  generation = StrongId<Tag, Rep>(static_cast<Rep>(current + Rep{1}));
  return true;
}

/// Monotonic allocator for one identity domain.  Allocation is deterministic:
/// the first identity handed out by a fresh allocator is always 1.
template <typename Tag, typename Rep = std::uint64_t>
class IdAllocator {
 public:
  [[nodiscard]] std::optional<StrongId<Tag, Rep>> allocate() noexcept {
    if (next_ == static_cast<Rep>(-1)) {
      return std::nullopt;
    }
    const StrongId<Tag, Rep> id(next_);
    next_ = static_cast<Rep>(next_ + Rep{1});
    return id;
  }

  /// Restores the high-water mark after recovery so that recovered state can
  /// never collide with newly allocated identities.
  void observe(StrongId<Tag, Rep> id) noexcept {
    if (id.value() >= next_ && id.value() != static_cast<Rep>(-1)) {
      next_ = static_cast<Rep>(id.value() + Rep{1});
    }
  }

  [[nodiscard]] Rep next_value() const noexcept { return next_; }

 private:
  Rep next_{1};
};

}  // namespace ensemble_fabric
