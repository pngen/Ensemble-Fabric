# Ensemble Fabric - Architecture

Ensemble Fabric governs one systems question:

> Given multiple candidate models, specialists, judges, execution branches, and
> results, which ensemble plan may execute, what evidence each participant
> contributes, how disagreement and failure are resolved, and which final result
> is authoritative?

## What the runtime owns

Ensemble identity and generations; ensemble specifications; participant roles,
requirements, and authority; execution topology (parallel and staged);
candidate-attempt identity and lifecycle; specialist, judge, verifier, and
fallback assignment; quorum, consensus, and disagreement representation;
evaluation evidence and provenance; aggregation, arbitration, and tie
resolution; abstention and partial-failure semantics; cancellation; fallback
sequencing; retry authority; exactly-once logical result commit; stale-state
fencing; deterministic explanations; persistence and recovery of durable state;
and the distinction between current and historical ensemble authority.

## What the runtime deliberately does not own

Generic model discovery, global model routing, model hosting, inference serving,
model loading and residency, token generation, KV-cache management, GPU
scheduling, workload placement, generic agent lifecycle, generic tool execution,
generic dependency graphs, model training, benchmark suites, prompt management,
generic policy engines, and general-purpose messaging.

Adjacent boundaries stay adjacent:

| Adjacent system | Owns | Ensemble Fabric's relationship |
| --- | --- | --- |
| Model Router | Which model or model class should receive work, by capability, policy, cost, availability | Ensemble Fabric governs what happens when work intentionally involves several model executions |
| Agent Runtime | Long-running autonomous-agent lifecycle | Ensemble Fabric governs bounded multi-model result production and adjudication |
| Critic Fabric | Reusable specialized critique and verification workers | Ensemble Fabric models a VERIFIER participant role; it does not absorb a critic runtime |
| Experiment Fabric | Autonomous experiments and hypothesis branching | Ensemble Fabric owns bounded multi-model result production, not experiment search |

## Layers

    controller (client)          examples, ensemble_client, tests
            |
            v
    coordinator                  ensemble_coordinator
            |   owns exactly one EnsembleFabric and one event loop
            v
    loopback TCP, framed, versioned, checksummed
            |
            v
    participant workers          ensemble_worker + a ParticipantBackend

The core library is usable with no network and no CUDA at all: LocalEnsembleDriver
executes the same action contract inside one process against in-process backends.

## Authority model

An action is legal only when every component of its authority matches current state:

    EnsembleId + EnsembleGeneration
    ParticipantId + ParticipantGeneration
    WorkerId + WorkerBootId
    CoordinatorEpoch
    EnsembleExecutionId + EnsembleAttemptGeneration
    CandidateId + CandidateGeneration
    EvaluationId + EvaluationGeneration

- Availability is not authority. A reachable worker whose boot identity or epoch is
  stale may not mutate current state.
- WorkerBootId is generated fresh by each process incarnation. A restarted worker
  never inherits its predecessor's authority.
- CoordinatorEpoch advances on every coordinator restart. Recovered dynamic evidence
  is marked REVALIDATION_REQUIRED; nothing recovered from disk becomes current on its own.
- Participant generations advance on replacement. A replacement never inherits the
  predecessor's votes or candidates.
- Candidate generations advance on retry. Late output from a superseded attempt is fenced.

## Candidate lifecycle

    DECLARED -> DISPATCHED -> OUTPUT_RECEIVED -> VALID -> ELIGIBLE -> SELECTED
                       |            |             |          \-> REJECTED
                       |            |             \-> SUPERSEDED on a new generation
                       |            \-> INVALID
                       \-> FAILED -> DECLARED (retry, new candidate generation)

    CANCELLED, SUPERSEDED, ABSTAINED, and RETIRED are terminal for the current
    execution generation.

Every transition outside this table is a typed rejection, never a silent no-op.

## Quorum

Quorum is stated with an explicit denominator and explicit inclusion rules:
absolute participants, role quotas, evaluation minimums, candidate minimums,
consensus threshold, abstention counting, optional-participant counting,
failed-participant counting, and unavailable-participant counting. Membership is
generation-bound, and duplicate votes from one participant generation collapse
into a single vote, which makes vote inflation structurally impossible.

## Consensus

CONSENSUS, CONSENSUS_WITH_ABSTENTIONS, PLURALITY_WITHOUT_THRESHOLD,
QUORUM_NOT_REACHED, QUORUM_IMPOSSIBLE, DISAGREEMENT, TIE, INSUFFICIENT_EVIDENCE,
REQUIRED_PARTICIPANT_FAILED, ALL_CANDIDATES_INVALID, NO_ELIGIBLE_CANDIDATE,
FALLBACK_REQUIRED, CANCELLED, SUPERSEDED, and REVALIDATION_REQUIRED are distinct
outcomes. A threshold that is configured but not met is DISAGREEMENT, never a
silent win for the plurality.

## Arbitration

1. Hard eligibility runs first and is not configurable: current generation,
   authoritative participant, compatible participant, selectable role, content
   held, mandatory predicates satisfied, verifier gating, minimum evidence,
   minimum aggregate score.
2. Then the configured ranking factors, in the configured order.
3. Then the configured tie-break policy. REPORT_TIE reports a tie rather than
   picking silently; the deterministic policies always end at stable candidate
   identity.

No container iteration order is ever consulted, so identical authoritative inputs
always produce the identical decision and the identical explanation.

## Exactly-once logical commit

At most one authoritative logical result is committed for one ensemble execution
generation. A repeated identical commit is idempotent; a conflicting commit for the
same generation is rejected; a stale generation, a stale epoch, a cancelled
execution, and a superseded generation are each rejected with their own typed code.
Cancellation recorded before the commit gate wins; a commit that linearizes first is
preserved and a later cancellation does not revoke it.

## Persistence

The durable container carries a magic word, an explicit format version, bounded
lengths, a header checksum, a payload checksum, a content digest, and a record count.
Writes go to a unique temporary file that is atomically renamed over the destination.
Recovered live authority is never treated as current. Corrupt, truncated,
over-declared, and trailing-byte inputs are each rejected with a distinct code.

## Concurrency

- One shared mutex guards runtime state; no callback, network operation, or file
  write ever runs while it is held.
- The runtime returns FabricAction values; the caller transports them, so the
  distributed coordinator never performs network I/O under the state lock.
- Concurrent sends on one connection are serialized by a per-connection write mutex.
- The coordinator mutates state on exactly one thread (the event loop); reader
  threads only decode and enqueue.
- Every scope is bounded by configuration, not by luck.

## Deterministic race semantics

Where two actions genuinely race, both legal outcomes are defined and the loser
produces a typed rejection:

| Race | Legal outcomes |
| --- | --- |
| completion vs completion (different slots) | both admitted |
| duplicate completion (same slot) | first admitted, later ones duplicate_completion |
| commit vs commit | exactly one commit record; repeats are idempotent |
| commit vs cancellation | commit linearizes first, or cancellation does |
| commit vs supersession | commit wins, or ensemble_superseded |
| stale worker vs replacement | replacement advances the generation; stale output is fenced |
