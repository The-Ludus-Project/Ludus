---
name: ludus-reference-review
description: Evaluate a publication, talk, PDF, link, tip, coding convention, or anecdote for a concrete Ludus improvement. Use when the user asks how an idea applies to Ludus or wants to carry it through implementation, validation, and follow-up tracking.
---

# Review a reference for Ludus

Follow the repository's [reference review workflow](../../../docs/wiki/contribute/index.md#turn-a-reference-or-tip-into-a-ludus-change)
and [contributor rules](../../../AGENTS.md).

When a user shares an input as part of this workflow, take it through the
decision and any supported, in-scope implementation in the same task. Honor
an explicit review-only request. If the source is partly accessible, work
from the portions you can verify. Ask for the missing material only when
it would change the decision and you cannot resolve the gap another way.

## Assess the engine, editor, and apps

Check the source against all three parts of Ludus:

- **Engine.** Inspect the owning subsystem, current behavior, tests, and
  roadmap. Identify supported capabilities and concrete technical gaps.
- **Editor.** Trace the relevant authoring and preview workflow. Check where
  users get feedback and where the workflow still requires manual work.
- **Apps built with Ludus.** Check what a game or app built with the SDK or
  authored in the editor can use. Where relevant, trace the feature through
  Play, build, and packaging on a named target. Check the app's behavior and
  fallbacks independently of the editor preview.

Record a decision and its evidence for each area. Implement supported work
within the current scope and record its validation. Put concrete remaining
work in the backlog. For deferred work, name the prerequisite or evidence
that would trigger reconsideration. When an idea is already covered or has
no relevant use, record the existing owner or the reason.

## Track follow-up work

As part of the review, track concrete remaining implementation, validation,
or prerequisite work in [Ludus Kanban](https://github.com/orgs/The-Ludus-Project/projects/1).
Honor review-only requests and any limits the user sets on GitHub changes.

1. Search the board and repository issues for the same deliverable. Reuse a
   matching issue or create one, then add it to the board if needed. Give new
   cards **Backlog** status and preserve the status of existing cards unless
   their work has advanced.
2. Describe the Ludus gap, the deliverable, and acceptance criteria that tell a
   reviewer what evidence will establish completion. Link the relevant source
   passage, the owning roadmap or design, and the implementation PR when
   applicable. Use GitHub's **Blocked by** relationship for actual prerequisites.
3. Set **Priority** and a one-sentence **Priority rationale** using the
   [roadmap priority rules](../../../docs/wiki/contribute/index.md#prioritize-roadmap-work).
   For conditional work, name the trigger. If the evidence is insufficient,
   leave **Priority** blank and record what is missing in the rationale for triage.
   Verify the card and its fields on the board.

Report the source and the decisions for all three areas, including what
changed and the validation results or remaining gaps. Include the pull
request and worktree for implemented changes, and link any follow-up cards
you added or updated with their priorities. If a board update is blocked,
report what remains and why.
