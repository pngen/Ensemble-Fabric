# Ensemble Fabric

Ensemble Fabric is an open-source, vendor-neutral C++20 reference runtime for
orchestrating multiple model executions as one governed ensemble operation.

Several models produced outputs. That is not an ensemble. Ensemble Fabric makes
the second statement mechanically enforceable:

> This ensemble, under this generation, with these authoritative participants and
> this current evidence, validly selected and committed this result.

## The systems question

Given multiple candidate models, specialists, judges, execution branches, and
results, which ensemble plan may execute, what evidence each participant
contributes, how disagreement and failure are resolved, and which final result
is authoritative?

An ensemble is not a vector of model calls. Parallel execution alone is not an
ensemble. Majority vote alone is not an ensemble. Calling a second model after a
failure alone is not an ensemble. A final answer is authoritative only when it
was produced by a valid ensemble specification, executed through current
participant authority, evaluated under explicit aggregation and arbitration
policy, and committed under the current ensemble generation.

## Boundary

Ensemble Fabric owns ensemble identity and generations; ensemble specifications;
participant roles, requirements, and authority; execution topology; candidate and
evaluation lifecycle; specialist, judge, verifier, and fallback assignment;
quorum, consensus, and disagreement representation; evaluation evidence and
provenance; aggregation, arbitration, and tie resolution; abstention and
partial-failure semantics; cancellation; retry authority; fallback sequencing;
exactly-once logical result commit; stale-state fencing; deterministic
explanations; persistence and recovery of durable state; and the distinction
between current and historical ensemble authority.

It deliberately does not own generic model discovery, global model routing,
model hosting, inference serving, model loading or residency, token generation,
KV-cache management, GPU scheduling, workload placement, generic agent
lifecycle, generic tool execution, generic dependency graphs, model training,
benchmark suites, prompt management, generic policy engines, or a general-purpose
message bus.

| Adjacent system | Owns | Relationship |
| --- | --- | --- |
| Model Router | Which model or model class should receive work, by capability, policy, cost, availability | Ensemble Fabric governs what happens when work intentionally involves several model executions |
| Agent Runtime | Long-running autonomous-agent lifecycle | Ensemble Fabric governs bounded multi-model result production and adjudication |
| Critic Fabric | Reusable specialized critique and verification workers | Ensemble Fabric models a VERIFIER participant role and does not absorb a critic runtime |
| Experiment Fabric | Autonomous experiments and hypothesis branching | Ensemble Fabric owns bounded multi-model result production, not experiment search |

## Architecture

    controller / client            ensemble_client, examples, tests
            |
            v
    coordinator                    ensemble_coordinator
            |   one EnsembleFabric, one event loop, one durable state file
            v
    loopback TCP, framed, versioned, checksummed
            |
            v
    participant workers            ensemble_worker + a ParticipantBackend

```
include/ensemble_fabric/     public headers
src/                         core library implementation
src/distributed/             coordinator, worker, client transport
tools/                       coordinator, worker, client, inspection CLI
examples/                    runnable examples
tests/                       contract, property, race, adversarial, multiprocess tests
benchmarks/                  completed-work benchmarks
cuda/                        optional accelerator authority-gating proof
consumer/                    independent find_package consumer
```

The core library is usable with no network and no CUDA:
`LocalEnsembleDriver` executes the same action contract inside one process
against in-process backends.

## Key semantics

These distinctions are enforced in code and exercised by tests:

- participant availability is not participant authority
- a request being accepted is not a participant having executed
- model output is not a valid candidate
- a candidate is not an accepted result
- a score is not authority
- a majority is not correctness
- a judge opinion is not an automatic commit
- reaching quorum is not satisfying policy
- attempting a fallback is not authorizing a fallback
- completion is not current completion
- a historical result is not the current result
- ensemble execution is not final ensemble commit

UNKNOWN never silently becomes VALID, AVAILABLE, PASS, CONSENSUS,
QUORUM_REACHED, or AUTHORITATIVE. Hard-invalid candidates never survive because
of favorable scores, and aggregation policy never silently overrides hard
eligibility.

## Authority model

An action is legal only when every component of its authority matches current
state: ensemble identity and generation, participant identity and generation,
worker identity and boot identity, coordinator epoch, execution and attempt
generation, candidate identity and generation, and evaluation identity and
generation.

- WorkerBootId is generated fresh by each process incarnation; a restarted
  worker never inherits authority.
- CoordinatorEpoch advances on every coordinator restart; recovered dynamic
  evidence becomes REVALIDATION_REQUIRED.
- Participant generations advance on replacement, so a replacement never
  inherits its predecessor's votes or candidates.
- Candidate generations advance on retry, so late output from a superseded
  attempt is fenced.

## Participant roles

CANDIDATE, SPECIALIST, JUDGE, VERIFIER, FALLBACK. Roles are semantic, not
decorative: a specialist can be required for one domain, a judge may not become
the final answer unless the policy permits it, a verifier returns a typed state
without producing a replacement candidate, and a fallback participant is not
dispatched until a typed activation condition becomes true.

## Ensemble lifecycle

Define a generation-bound specification, open an execution, dispatch candidates
in parallel or staged order, ingest candidate output or typed failure, plan and
ingest evaluations, arbitrate, and commit exactly one authoritative result.
Cancellation, supersession, retry, fallback, and coordinator recovery are all
first-class transitions with their own identity and their own typing.

## Quorum and consensus

Quorum states its denominator explicitly (declared, authoritative, contributing,
required, or eligible participants), its role quotas, its evaluation and
candidate minimums, its consensus threshold, and whether abstentions, optional
participants, failed participants, and unavailable participants count. Membership
is generation-bound, and duplicate votes from one participant generation collapse
into a single vote.

Consensus outcomes are distinct: CONSENSUS, CONSENSUS_WITH_ABSTENTIONS,
PLURALITY_WITHOUT_THRESHOLD, QUORUM_NOT_REACHED, QUORUM_IMPOSSIBLE,
DISAGREEMENT, TIE, INSUFFICIENT_EVIDENCE, REQUIRED_PARTICIPANT_FAILED,
ALL_CANDIDATES_INVALID, NO_ELIGIBLE_CANDIDATE, FALLBACK_REQUIRED, CANCELLED,
SUPERSEDED, and REVALIDATION_REQUIRED. A configured threshold that is not met is
DISAGREEMENT, never a silent plurality win. A tie is reported as a tie unless the
ensemble explicitly authorizes a deterministic tie-break policy.

## Arbitration

Hard eligibility runs first and is not configurable. Then the configured ranking
factors, in order. Then the configured tie-break policy. No container iteration
order is ever consulted, so identical authoritative inputs produce the identical
decision and the identical explanation. Every result carries the considered
candidates, the eliminated candidates, the reason each was eliminated, the
current evidence, the quorum and consensus state, the ranking factors, the
tie resolution, and the authoritative generations.

## Stale-state fencing

Late output from a cancelled, replaced, failed, stale-generation, or dead-worker
participant cannot mutate current state. Each rejection carries its own typed
code: stale ensemble generation, stale execution generation, stale participant
generation, stale worker boot, stale coordinator epoch, stale candidate
generation, stale evaluation generation, ensemble cancelled, or ensemble
superseded.

## Persistence and recovery

The durable container carries a magic word, an explicit format version, bounded
lengths, a header checksum, a payload checksum, a content digest, and a record
count. Writes are atomic: a unique temporary file is renamed over the
destination. Corrupt, truncated, over-declared, and trailing-byte inputs are each
rejected with a distinct code, and a state file written by a future format
version is rejected explicitly rather than misread.

A recovered coordinator advances its epoch, preserves committed results exactly,
and marks every recovered participant and in-flight execution as requiring
revalidation. Recovered dynamic evidence is never silently current.

## Distributed reference topology

    ensemble_coordinator --port 0 --state state.bin --port-file port.txt
    ensemble_worker --coordinator 127.0.0.1:<port> --participant 1 --roles CANDIDATE --script "..."
    ensemble_client --coordinator 127.0.0.1:<port> --demo-parallel 3 --judges 1

The protocol is framed and bounded: magic, version, header size, message type,
flags, bounded payload length, correlation identity, coordinator epoch, worker
boot identity, sequence, and a CRC32C integrity check. Frames are decoded before
any payload allocation, and malformed, truncated, oversized, unknown-type,
bad-checksum, and non-zero-reserved-word inputs are rejected with distinct codes.

## Build

Requirements: CMake 3.20 or newer, a C++20 compiler, and (on Windows) the
Visual Studio toolchain.

    cmake -S . -B build-rel -G "Visual Studio 17 2022" -A x64
    cmake --build build-rel --config Release --parallel

On other generators and platforms:

    cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
    cmake --build build-rel --parallel

Options: `ENSEMBLE_FABRIC_BUILD_TESTS`, `ENSEMBLE_FABRIC_BUILD_EXAMPLES`,
`ENSEMBLE_FABRIC_BUILD_TOOLS`, `ENSEMBLE_FABRIC_BUILD_BENCHMARKS`,
`ENSEMBLE_FABRIC_BUILD_CUDA_PROOF`, `ENSEMBLE_FABRIC_ENABLE_ASAN`,
`ENSEMBLE_FABRIC_ENABLE_SHARED`.

Everything is built with strict warnings, and the library carries no warnings of
its own: on MSVC `/W4 /WX /permissive-`, elsewhere `-Wall -Wextra -Wpedantic
-Werror` and friends. Warnings are never suppressed.

## Test

    ctest --test-dir build-rel -C Release --output-on-failure

Tests run to completion naturally; there are no test timeouts, no artificial
delays, and no sleeps used as synchronization. Races are synchronized with
barriers and explicit orderings.

The suite covers unit contracts, the candidate state machine, quorum and
consensus algebra, arbitration and determinism, exactly-once commit, retry,
fallback, cancellation, persistence corruption and recovery, protocol
malformation, loopback transport, seeded randomized property runs, deterministic
races, an adversarial phase, and real multiprocess deployment.

## Install and consume

    cmake --install build-rel --config Release --prefix <prefix>

An independent project then consumes the installed package:

    find_package(ensemble_fabric 1.0 REQUIRED CONFIG)
    target_link_libraries(app PRIVATE ensemble_fabric::ensemble_fabric)

`consumer/` is exactly such a project: it constructs an ensemble, ingests
deterministic participant results, arbitrates them, verifies the selected output,
and re-checks idempotent commit.

## Examples

    example_parallel_best_of_n      independent candidates, hard filters, one commit
    example_candidate_judge         candidate evidence plus judge arbitration
    example_weighted_quorum         explicit denominator, threshold, abstentions
    example_specialist_judge        sequential staged specialists
    example_verifier_gated          a mandatory predicate beats a better score
    example_fallback                typed activation, fenced late primary result
    example_disagreement            disagreement reported, never fabricated
    example_cancellation            a cancelled ensemble never publishes success
    example_retry_replacement       retry advances the candidate generation
    example_multiprocess            coordinator plus real participant processes

## Inspection CLI

    ensemble_inspect --file state.bin --ensemble 1
    ensemble_inspect --coordinator 127.0.0.1:<port> --ensemble 1
    ensemble_inspect --file state.bin --list

The inspection surface reports ensemble identity, generation, execution status,
participants with roles, boots and authority, candidate states, evaluations with
their exact target generations, quorum, consensus, arbitration, commits,
cancellation and supersession, and recovery state. The CLI is an inspection
surface over the runtime, never a source of truth.

## Benchmarks

    ensemble_benchmarks --scale 1

Benchmarks measure completed operations, not async submission: an operation is
counted only after its ensemble has committed an authoritative result. Measured
on this machine, Release, MSVC 19.44:

    ensemble definition                          3.33 us/op
    candidate ingest and arbitration (4)        54.57 us/op
    ensemble completion (32 candidates, 4 judges)   2.39 ms/op
    ensemble completion (48 candidates, 8 judges)  10.37 ms/op
    state query (inspection snapshot)            1.49 us/op
    specification encode/decode                  5.91 us/op
    state serialization                         9.20 us/op
    protocol frame encode/decode                 0.28 us/op

## Verified status

REAL, executed during closure on this machine:

- Windows x64, MSVC 19.44, Visual Studio 2022 Build Tools
- Release and Debug configurations, full test suite, 100 percent passing
- AddressSanitizer build of the CPU, transport, persistence, and engine suites
- real multiprocess deployment over loopback TCP with OS process death,
  fresh-boot revalidation, and coordinator restart
- NVIDIA GeForce RTX 5090 (compute capability 12.0), CUDA 12.9: real device
  execution with host-to-device copy, kernel, device synchronize,
  device-to-host copy, CPU reference parity, and device cleanup

SYNTHETIC: the deterministic reference backend emulates model participants with
scripted, replayable behavior so that ensemble semantics can be tested without a
proprietary model API. It is evidence about the runtime, never evidence about
model quality. No claim in this repository is based on synthetic output as if it
were real inference.

UNSUPPORTED, and therefore not claimed: multiple physical GPUs, multi-node
clusters, NVLink, RDMA, InfiniBand, hardware partitioning, and integration with
any external model provider. None of these were exercised here.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
