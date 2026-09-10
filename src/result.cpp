#include "ensemble_fabric/result.hpp"

#include "detail/text.hpp"

namespace ensemble_fabric {

std::string_view to_string(EnsembleDecision decision) noexcept {
  switch (decision) {
    case EnsembleDecision::pending: return "PENDING";
    case EnsembleDecision::committed: return "COMMITTED";
    case EnsembleDecision::cancelled: return "CANCELLED";
    case EnsembleDecision::superseded: return "SUPERSEDED";
    case EnsembleDecision::failed: return "FAILED";
    case EnsembleDecision::revalidation_required: return "REVALIDATION_REQUIRED";
    case EnsembleDecision::no_eligible_candidate: return "NO_ELIGIBLE_CANDIDATE";
    case EnsembleDecision::fallback_required: return "FALLBACK_REQUIRED";
  }
  return "UNKNOWN_DECISION";
}

std::uint64_t commit_fingerprint(const EnsembleResult& result) noexcept {
  std::uint64_t hash = 1469598103934665603ull;
  hash = detail::fnv1a64_extend(hash, result.ensemble.value());
  hash = detail::fnv1a64_extend(hash, result.ensemble_generation.value());
  hash = detail::fnv1a64_extend(hash, result.execution.value());
  hash = detail::fnv1a64_extend(hash, result.attempt_generation.value());
  hash = detail::fnv1a64_extend(hash, result.coordinator_epoch.value());
  hash = detail::fnv1a64_extend(hash, static_cast<std::uint64_t>(result.decision));
  hash = detail::fnv1a64_extend(hash, result.has_selected ? 1ull : 0ull);
  hash = detail::fnv1a64_extend(hash, result.selected_candidate.value());
  hash = detail::fnv1a64_extend(hash, result.selected_candidate_generation.value());
  hash = detail::fnv1a64_extend(hash, result.selected_participant.value());
  hash = detail::fnv1a64_extend(hash, result.payload_digest);
  hash = detail::fnv1a64_extend(hash, result.fallback_used ? 1ull : 0ull);
  hash = detail::fnv1a64_extend(hash, static_cast<std::uint64_t>(result.fallback_depth));
  hash = detail::fnv1a64_extend(hash, static_cast<std::uint64_t>(result.consensus.state));
  return hash;
}

std::string EnsembleResult::render() const {
  std::string out = "result " + id.to_string();
  out.append(" generation=").append(generation.to_string());
  out.append(" ensemble=").append(ensemble.to_string());
  out.append(" ensemble_generation=").append(ensemble_generation.to_string());
  out.append(" execution=").append(execution.to_string());
  out.append(" attempt=").append(attempt_generation.to_string());
  out.append(" epoch=").append(coordinator_epoch.to_string());
  out.append(" decision=").append(ensemble_fabric::to_string(decision));
  out.append(" selected=");
  out.append(has_selected ? selected_candidate.to_string() : std::string("none"));
  if (has_selected) {
    out.append(" selected_generation=").append(selected_candidate_generation.to_string());
    out.append(" selected_participant=").append(selected_participant.to_string());
    out.append(" role=").append(ensemble_fabric::to_string(selected_role));
  }
  out.append(" payload_bytes=").append(std::to_string(payload_bytes));
  out.append(" payload_digest=").append(std::to_string(payload_digest));
  out.append(" commit_sequence=").append(commit_sequence.to_string());
  out.append(" commit_fingerprint=").append(std::to_string(commit_fingerprint));
  if (fallback_used) {
    out.append(" fallback_depth=").append(std::to_string(fallback_depth));
  }
  out.push_back('\n');
  out.append("  ").append(quorum.render()).push_back('\n');
  out.append("  ").append(consensus.render()).push_back('\n');
  out.append("  ").append(arbitration.render());
  for (const ExplanationEntry& entry : explanation) {
    out.push_back('\n');
    out.append("  [").append(entry.subject).append("] ").append(entry.detail);
  }
  return out;
}

}  // namespace ensemble_fabric
