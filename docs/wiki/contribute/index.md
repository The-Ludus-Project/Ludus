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

Set the card's **Priority** and a one-sentence **Priority rationale** when
adding it. State the evidence that drives the decision, such as a failed
integration job, a committed milestone, or the work that can start once this
issue closes. Apply the same rule to new issues and reused cards.

| Priority | Use when |
| --- | --- |
| P0 | A verified failure blocks integration, promotion, or a committed release. Link the failing gate or regression. |
| P1 | Work is on the next milestone's critical path, unblocks multiple active workstreams, or closes a high-impact correctness or qualification gap. Name the affected milestone, consumers, or check. |
| P2 | A scoped roadmap deliverable has acceptance criteria and can wait for its milestone or prerequisites. |
| P3 | Work is conditional on a measured benefit, named consumer, or platform capability. Record the condition for reconsideration. |

Compare work within a priority by the next committed outcome, the open tasks
that can start immediately after it closes, and rough effort when known. Count
distinct downstream issues once. Indirect dependencies help show the size of a
chain. Keep using **Status** for execution state, including **In Validation**
while required evidence is pending.

If the evidence is incomplete, record the missing input in the rationale and
leave **Priority** empty so the card appears in **Priority triage**. Resolve
triage before moving it to **Ready**. Reassess priority when release goals,
blockers, or validation results change.

## Turn a reference or tip into a Ludus change

Start with a link, PDF, paper, book excerpt, slides, talk, practical tip, coding
convention or anecdote. Include the Ludus problem if you have one. Otherwise,
identify a concrete use from the source and the current engine.

1. **Read and assess the input.** For a publication or talk, record the author,
   title, version and the relevant pages, sections, slides or timestamps. Use
   the primary material and check figures, equations or demos when they carry the
   argument. State what you could access and distinguish the source's claims,
   evidence and assumptions from your interpretation. For an informal tip,
   capture the problem, proposed practice and conditions where it might help.
   Use anecdotes to form hypotheses and check them against Ludus.
2. **Compare it with Ludus today.** Inspect the owning module, architecture
   decisions, implementation and tests. Identify a real gap or an existing
   equivalent. Check the tradeoffs that matter for that subsystem, such as
   compatibility, performance, determinism, portability or maintenance cost.
   For a convention, check existing guidance and representative call sites.
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
