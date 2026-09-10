#pragma once

/// Ensemble Fabric 1.0.0 - orchestrating multiple model executions as one
/// governed ensemble operation.
///
/// The runtime owns ensemble identity and generations, participant roles and
/// authority, execution topology, candidate and evaluation lifecycle, quorum,
/// consensus, arbitration, fallback, cancellation, persistence/recovery, and
/// exactly-once logical result commit.
///
/// It deliberately does not own model discovery, global routing, inference
/// serving, model residency, token generation, GPU scheduling, agent lifecycle,
/// tool execution, or general-purpose messaging.

#include "ensemble_fabric/arbitration.hpp"
#include "ensemble_fabric/authority.hpp"
#include "ensemble_fabric/candidate.hpp"
#include "ensemble_fabric/client.hpp"
#include "ensemble_fabric/consensus.hpp"
#include "ensemble_fabric/coordinator.hpp"
#include "ensemble_fabric/evaluation.hpp"
#include "ensemble_fabric/explanation.hpp"
#include "ensemble_fabric/fabric.hpp"
#include "ensemble_fabric/ids.hpp"
#include "ensemble_fabric/limits.hpp"
#include "ensemble_fabric/local_driver.hpp"
#include "ensemble_fabric/outcome.hpp"
#include "ensemble_fabric/participant.hpp"
#include "ensemble_fabric/persistence.hpp"
#include "ensemble_fabric/protocol.hpp"
#include "ensemble_fabric/quorum.hpp"
#include "ensemble_fabric/reference_backend.hpp"
#include "ensemble_fabric/result.hpp"
#include "ensemble_fabric/role.hpp"
#include "ensemble_fabric/spec.hpp"
#include "ensemble_fabric/transport.hpp"
#include "ensemble_fabric/version.hpp"
#include "ensemble_fabric/worker.hpp"
