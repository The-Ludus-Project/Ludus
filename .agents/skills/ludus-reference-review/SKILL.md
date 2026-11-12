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

Report what you reviewed, the decision and why, what changed, and the
validation results or remaining gap. Include the pull request and worktree
for implemented changes, and link any follow-up cards you added or updated
with their priorities. If a board update is blocked, report what remains and why.
