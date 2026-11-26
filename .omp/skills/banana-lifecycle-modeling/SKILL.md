---
name: banana-lifecycle-modeling
description: Use to model BananaOS component lifecycles as state machines, derive invariants, design deterministic event traces, fault injection, and reproducible regressions. Not for implementing lifecycle behavior (use the component skill, e.g. banana-retro-lifecycle), wake/power policy, or hardware qualification.
---

# Banana lifecycle modeling

1. **Boundary.** Produce models, trace specs, and fixture designs. The component skill owns contracts, code paths, and gates; link to it and do not restate its rules. Modeling is read-only unless edits and checks are approved. Roadmap scope is not approval.

2. **State inventory.** One machine per owner (for example launcher, UI, runtime, bananad, saves/resume). Record:
   - states;
   - variables outside the state enum (epoch, session, holds, attempt count, sync flags);
   - events and peers, with their authenticated role.
   Anchor every transition to `file:line`. Label each as observed, documented, or inferred. Model cross-component edges as messages carrying the peer and epoch.

3. **Transition table.** Columns: state, event, guard, action, next state, visible effect, durable effect. List illegal and unhandled pairs with an explicit outcome: reject, ignore, replay, or abort. Include terminal and recovery states.

4. **Invariants.** Give each an ID, an observable predicate, a check point, and a source anchor. Take them from AGENTS.md and component contracts; do not invent rules. Kinds:
   - safety (never X);
   - monotonic, scoped to the record or session identity that owns the counter, never global;
   - conservation (every acquired hold is released on every exit);
   - ordering, keeping **atomic visibility** (a reader sees the old or new record, never a torn one) separate from **durability** (the data and its directory entry survive power loss only after the required sync). A rename is visible without being durable. Model both as distinct events and predicates. Separate *expected durability after a successful sync* (a model predicate) from *proven power-loss survival* (needs device evidence); never label the former as the latter.

5. **Deterministic sequences.**
   - Script events with logical time, an injected clock, and injected IDs. No sleeps or wall-clock assertions.
   - The oracle is per-step state plus invariant results, not just the final state.
   - Explore small event sets exhaustively, with a stated bound on events, depth, and total runs. Use a seeded generator beyond that and record the seed.
   - Report what the bound left unexplored.
   - Replay and duplicate cases must respect the implementation's **bounded IPC replay history** (per role, ring-evicted). Banana Retro stores only successful replies. After authorization, an ID found in history replays the stored reply only if type and payload match; otherwise the request gets `INVALID`. An uncached request (never seen, evicted, or failed) still obeys the ID/session guards: the session ID must match, and the ID must exceed the role's last ID or the request gets `STALE_SESSION`. A failed request still advances that last ID, so retrying it with the same ID is stale; a retry needs a new, higher ID. Unauthorized requests are rejected before either guard and do not advance it. Model eviction, and test inside and beyond the window for both outcomes.

6. **Fault injection.**
   - Catalog the injection points: write, fsync, rename, directory sync, and unlink failures; short I/O; ENOSPC; torn or corrupt records; peer loss; dropped or delayed replies; timeouts; crashes between named steps.
   - Run a single-fault matrix first, then chosen pairs.
   - Use counted deterministic hooks, such as failing the Nth call. Keep crash-point cases separate from error-return cases.
   - State the expected recovery for each fault: last-good generation, rollback, fallback eligibility, or a debt kept.

7. **Reproducible regressions.**
   - **Prefer production helpers, parsers, and test hooks** over model-only reimplementations. Extend the existing `check-*` targets and shell tests; do not add a second harness convention. A model-only fixture is acceptable only where no production seam exists. Label it as such and name the missing seam.
   - Each finding gets a fixture that fails before the fix and passes after. Record the invariant ID, the initial state, the script or fault, the expected trace, the seed, the exit status, and the failing text. Preserve the failing logs before fixing.
   - Prefer the shortest counterexample, shrunk by removing events and faults while the failure still reproduces. Set no hard step limit; keep every step needed.

8. **Reporting.** Give a matrix of transitions, invariants, and fault points exercised against the total, and name the gaps. Label each claim by evidence: model only, host fixture, production binary, or hardware. Host or model results never establish exFAT durability or device behavior. Raise contract ambiguities as questions to the owner.

9. **Anti-patterns.** Sleep-based timing; final-state-only oracles; global counters across identities; treating visibility as durability; unbounded replay assumptions; unseeded random faults; unanchored models that drift from the code; fixtures that restate implementation text.
