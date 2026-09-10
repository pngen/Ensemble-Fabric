#include "ensemble_fabric/evaluation.hpp"

#include "ensemble_fabric/persistence.hpp"

#include "detail/text.hpp"

namespace ensemble_fabric {

std::uint64_t fingerprint_evaluation(const EvaluationSubmission& submission) noexcept {
  std::uint64_t hash = 1469598103934665603ull;
  hash = detail::fnv1a64_extend(hash, submission.authority.evaluator.participant.value());
  hash = detail::fnv1a64_extend(hash, submission.authority.evaluator.participant_generation.value());
  hash = detail::fnv1a64_extend(hash, submission.authority.candidate.value());
  hash = detail::fnv1a64_extend(hash, submission.authority.candidate_generation.value());
  hash = detail::fnv1a64_extend(hash, static_cast<std::uint64_t>(submission.criterion.id));
  hash = detail::fnv1a64_extend(hash, static_cast<std::uint64_t>(submission.criterion.version));
  hash = detail::fnv1a64_extend(hash, static_cast<std::uint64_t>(submission.kind));
  hash = detail::fnv1a64_extend(hash, static_cast<std::uint64_t>(submission.verification));
  hash = detail::fnv1a64_extend(hash, static_cast<std::uint64_t>(submission.judgment));
  hash = detail::fnv1a64_extend(hash, submission.score_defined ? 1ull : 0ull);
  hash = detail::fnv1a64_extend(hash, submission.score);
  hash = detail::fnv1a64_extend(hash, submission.weight);
  hash = detail::fnv1a64_extend(
      hash, sha256_digest64(std::span<const std::byte>(submission.evidence.data(),
                                                       submission.evidence.size())));
  hash = detail::fnv1a64_extend(hash, submission.rationale);
  return hash;
}

}  // namespace ensemble_fabric
