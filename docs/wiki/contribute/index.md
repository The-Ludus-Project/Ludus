# Contribute to Ludus

Start with a concrete user problem or a bounded engine change. Find the owning
module, read its design/evidence and tests, and preserve its dependency direction.
Use a pull request to make the change and its validation reviewable.

## Required engineering rules

The repository's [AGENTS.md](https://github.com/The-Ludus-Project/Ludus/blob/main/AGENTS.md)
and [.kiro steering](https://github.com/The-Ludus-Project/Ludus/tree/main/.kiro/steering)
are authoritative. Read them before editing code. In particular:

- Use C++23 and the pinned reference tools; engine/app code has no C++ exceptions.
- Use Ludus fixed-width aliases and explicit error results.
- Keep heavy facilities and private headers outside public include boundaries.
- Credit actually consulted technical sources near the affected implementation.
- Build warning-clean and run appropriate unit, sanitizer, formatting and tidy gates.

## Prioritize roadmap work

Use [Ludus Kanban](https://github.com/orgs/The-Ludus-Project/projects/1) to
compare work across engine milestones. When adding a backlog item, look for an
existing issue, link the owning roadmap and acceptance criteria, and use
GitHub's **Blocked by** relationship for actual prerequisites.

The board answers two separate questions. **Priority** says how much an
outcome matters. **Blocked by** says what must finish first. Do not use
Priority to express order inside a dependency chain.

Set **Priority** per milestone outcome: every phase of a milestone (for
example Mobile M0 to M6) carries the milestone's priority, and **Blocked by**
orders the phases. Standalone follow-ups take their own priority. Set a
one-sentence **Priority rationale** that says why the outcome matters now,
such as a failed integration job, a committed milestone, or a named consumer.
The prerequisites are already recorded by **Blocked by**, so do not restate
them. Apply the same rule to new issues and reused cards.

| Priority | Use when |
| --- | --- |
| P0 | A verified failure blocks integration, promotion, or a committed release. Link the failing gate or regression. |
| P1 | Work is on the next milestone's critical path, unblocks multiple active workstreams, or closes a high-impact correctness or qualification gap. Name the affected milestone, consumers, or check. |
| P2 | A scoped roadmap deliverable has acceptance criteria and can wait for its milestone or prerequisites. |
| P3 | Work is conditional on a measured benefit, named consumer, or platform capability. Record the condition for reconsideration. |

Keep P1 for the current focus, at most a few milestones at once. When most
of the backlog shares one priority, Priority no longer distinguishes work.

Two derived fields show what can start. The
[Kanban dependency fields](https://github.com/The-Ludus-Project/Ludus/blob/develop/.github/workflows/kanban-dependencies.yml)
workflow recomputes them hourly and when an issue closes or reopens. Do not
edit them by hand.

| Field | Meaning |
| --- | --- |
| **Actionable** | *Actionable*: no open blocker. *Waiting on validation*: every open blocker is on the board in **In Validation**, so the work can usually start. *Blocked*: another open blocker remains. Closed issues are cleared. |
| **Unlocks** | Distinct open issues that transitively wait on this one, counted once each. |

The **Next up** view lists **Backlog** cards with **Actionable** set to
*Actionable* or *Waiting on validation*, sorted by **Priority** and then
**Unlocks**. Compare candidates within a priority by the next committed
outcome, **Unlocks**, and rough effort when known.

Treat **Ready** as a short, deliberately ordered queue of about ten cards,
drawn from **Next up**. Drag cards to order them; the top card is the most
important work to start. Move a card back to **Backlog** when it becomes
*Blocked*. Keep using **Status** for execution state, including
**In Validation** while required evidence is pending.

To see the same ranking locally, or after editing dependencies, run:

```bash
python3 scripts/python/kanban_dependencies.py sync --dry-run
```

Omit `--dry-run` to write the fields. The tool uses `LUDUS_PROJECT_TOKEN`,
`GH_TOKEN`, or the signed-in `gh` account, which needs the `project` scope.

If the evidence is incomplete, record the missing input in the rationale and
leave **Priority** empty so the card appears in **Priority triage**. Resolve
triage before moving it to **Ready**. Reassess priority when release goals,
blockers, or validation results change.

Use the [weekly performance review](../../development/runtime-profiling.md#weekly-performance-review)
to turn engine and Editor benchmark evidence into scoped Kanban work and
verify improvements after integration.

## Turn a reference or tip into a Ludus change

Start with a link, PDF, paper, book excerpt, slides, talk, practical tip, coding
convention or anecdote. Include the Ludus problem if you have one. Otherwise,
look for a current use or an upcoming Ludus decision the source could inform.

1. **Read and assess the input.** For a publication or talk, record the author,
   title, version and the relevant pages, sections, slides or timestamps. Use
   the primary material and check figures, equations or demos when they carry the
   argument. State what you could access and distinguish the source's claims,
   evidence and assumptions from your interpretation. For an informal tip,
   capture the problem, proposed practice and conditions where it might help.
   Use anecdotes to form hypotheses and check them against Ludus.
2. **Compare current behavior and planned uses.** Inspect the owning module,
   architecture decisions, implementation and tests. Identify a real gap or an existing
   equivalent. Check the tradeoffs that matter for that subsystem, such as
   compatibility, performance, determinism, portability or maintenance cost.
   For a convention, check existing guidance and representative call sites.
   Consider the engine, editor workflows and apps built with Ludus separately.
   For each, look for a milestone, design or consumer whose requirements could
   change the conclusion. Cite that evidence and label inferred uses as hypotheses.
   If the value lies ahead, record when the choice first matters and how to test
   it. A finding of no relevant planned use is also a valid outcome.
3. **Decide and act.** Adopt or adapt an idea when it has a clear benefit and
   fits the engine. Use a bounded prototype with a baseline and a success
   criterion when an important uncertainty can be tested. Defer with a concrete
   prerequisite when evidence or support is missing. Close the review with the
   reason when the idea is already covered or unsuitable. Carry a supported
   decision through implementation in the owning module using the normal
   worktree and pull request workflow. If the source exposes a larger change,
   implement the smallest useful, verifiable slice and record what remains.
   For an adopted convention, update the owning guidance and enforce it where
   the rule is mechanically checkable and enforcement is warranted.
4. **Validate and record the result.** Match the check to the claim. Use a
   regression for behavior, a representative baseline comparison for
   performance, and the existing platform or parity checks where relevant.
   Record the decision, its rationale, significant departures and evidence in
   the owning design, contributor guidance or pull request. Follow Ludus's
   attribution rule beside materially influenced code, and check license terms
   before incorporating third-party code or assets. Keep supplied files and
   private anecdotes out of published documentation.

A short review can live in the pull request or task response. Give a substantial
review its own page only when it needs a durable evidence or design record. The
[reference reading paths](../learn/references.md) link to examples. If the
request is review-only, stop at the assessment and decision.

## Standard native validation

Prepare tests with initialization, then use the pinned tools:

```bash
./scripts/build linux-clang-development
./scripts/test linux-clang-development
./scripts/check linux-clang-development --all
./scripts/build linux-clang-asan-ubsan
./scripts/test linux-clang-asan-ubsan
./scripts/install-sdk linux-clang-development
```

Report any skipped platform/manual acceptance explicitly. A successful offscreen
editor test is useful logic evidence, but it does not prove native high-DPI,
window decoration or device interaction.

## Keep the documentation honest

Follow the [one-source publication rule](../../../AGENTS.md#one-documentation-source-online-and-offline):
search for an existing owner before creating a page, keep one canonical public
source in `docs/`, and verify its wiki publication, index and search. Reading
paths link to contracts; they do not maintain shortened copies. New pages need
a distinct reader task, contract, decision or evidence purpose.

Update a task guide when a workflow changes. Separate current behavior from
architecture proposals, and state the environment needed by a command. Do not
turn a catalogued title into a claim that its chapter was consulted or adopted.

For documentation-only work, see [Improve this wiki](../../development/wiki.md). Include the built
site/link checks and browser evidence relevant to the change.

## Propose a change

[Open a pull request](https://github.com/The-Ludus-Project/Ludus/pulls) or
[report a reproducible issue](https://github.com/The-Ludus-Project/Ludus/issues).
Explain the resulting behavior, why it helps, what was validated and any remaining
acceptance limits. Keep secrets and machine-specific settings out of commits.
