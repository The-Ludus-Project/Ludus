# Networking tasks

Architecture: [networking.md](../../../docs/architecture/networking.md).
Decision: [ADR 0021](../../../docs/decisions/0021-networking-core-and-transport-boundary.md).
The architecture is authoritative for contracts and exit gates.

- [x] N0: repository audit, initial design, primary-source check and Gems review.
- [x] N1: checked bit streams and versioned message envelope.
- [x] N1: 64-message wrap-safe duplicate window.
- [x] N1: bounded owned queues, complete-snapshot replacement, lane fairness.
- [x] N1: caller-clock seeded delay/jitter/loss/duplication simulator.
- [x] N1: public SDK target and integrated regression/installed-consumer coverage.
- [x] N2: audited/pinned native backend spike and explicit session lifecycle.
- [x] N2: compatibility/admission, connection generations, quotas and timeouts.
- [x] N2: real endpoints, loss/backpressure/reconnect/security tests.
- [ ] N3: explicit schema registry and network object generations.
- [ ] N3: reliable lifecycle metadata, relevance and baseline/keyframe state.
- [ ] N3: headless multi-client replication demo and fuzz/soak harness.
- [ ] N4: bounded prediction/input history, correction and remote interpolation.
- [ ] N4: game-specific bounded rewind and authoritative permission tests.
- [ ] N5: browser transport capability negotiation and server-side interop.
- [ ] N5: shared-codec inspector, trace/replay and defined load measurements.

Never mark the later slices implemented based on core loopback tests. The PR
records actual local/CI validation for N1 separately from these implementation tasks.

## Research backlog

The [networking reference review](../../../docs/architecture/networking-reference-review.md#conference-and-journal-research-backlog)
owns the venue list, source queue, reading status and experiment/decision records.
Research progress does not complete the implementation gates above.

Finish and validate the initial networking architecture through N2-N5 before
starting this broader improvement pass. Preserve its revision, workloads and
measurements as the baseline for later experiments. Required backend and
specification checks remain part of the initial implementation.

- [ ] Screen the recommended venue archives for N2-N5 topics and extend the source queue with attributed, prioritized articles and search dates/scopes.
- [ ] Fully review NR-01 through NR-05 and progressively read most articles retained in the relevant shortlists; record exact consulted sections and applicable assumptions.
- [ ] Trial promising ideas against a reproducible baseline, record improvements and regressions, and retain an adopt/defer/reject decision with implementation/test links.
