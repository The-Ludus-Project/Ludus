# Kiro handoff: build the code editor

This package is ready to push as Markdown. Start Kiro from a checkout that
contains these files:

- `.kiro/specs/code-editor/requirements.md`
- `.kiro/specs/code-editor/design.md`
- `.kiro/specs/code-editor/tasks.md`
- `docs/architecture/code-editor.md`
- `docs/architecture/code-editor-kiro-handoff.md`

The [architecture](code-editor.md) turns the existing shader Source editor into
one code editor for every project text file. It reuses tree-sitter for syntax,
LSP servers for semantics and DAP adapters for debugging, and adds the engine
context that VS Code lacks. The existing foundation (document seam, slangd
assistance, formatting, compiler diagnostics) is implemented; all the work in
the ten milestones is outstanding.

## Prompt 1 - Implement the complete milestone sequence

Copy this into Kiro after pushing the files:

```text
Build the Ludus code editor using the committed specification. Read AGENTS.md
and .kiro/steering/ first, then:
- .kiro/specs/code-editor/requirements.md
- .kiro/specs/code-editor/design.md
- .kiro/specs/code-editor/tasks.md
- docs/architecture/code-editor.md
- docs/architecture/editor-gui-systems.md, section "Source editing direction"

Execute M0 through M9 in dependency order and keep the task ledger in tasks.md
current. This authorizes implementation; do not stop after drafting another
plan or wait for approval between ordinary milestones. M0's tree-sitter ADR and
the F9/Ctrl+K keymap decision are the only points that need maintainer review:
open them as their own pull requests and carry on with M1 and M2, which do not
depend on them. Make each batch small and reviewable. Revalidate the checkout
instead of assuming the baseline still matches. Reuse the existing CodeEditor,
CodeDocumentHost, CodeDocumentController, SlangLanguageService, CodeAssistance,
CodeFormatter and CodeNavigation code; refactor them in structural moves with
regression coverage, separate from behavior changes. Preserve unrelated work.
Record material deviations with evidence in
docs/architecture/code-editor-decision-log.md.

Principles to keep throughout: typing never waits for parsing, servers, search,
diffing or formatting; the document host is the single authority for text,
history and save, and every other state is a revision-tagged projection; every
edit is one ordered non-overlapping edit set applied whole or not at all; every
asynchronous result is checked against the project epoch, document identity,
revision, context and request before it is used; every queue, result set, cache
and file size has one named owner constant; a missing or crashed tool keeps
editing usable and offers its fix.

Pay particular attention to: UTF-8/UTF-16/LSP position mapping; multi-cursor
edits computed back to front then sorted; IME preedit with multiple cursors;
mirroring deltas into other views without echoing them back as edits; tokens
from an older revision moved by edit deltas, never applied at stale offsets;
QSyntaxHighlighter whole-document rehighlighting on large files; Qt's
whole-block layout cost on long lines; dirty buffers versus external changes;
workspace edits validated in full before any file changes; Live Preview keeping
the last drawable view until Ready; server crash loops; debugger pauses never
blocking the UI thread.

Follow C++23, the Ludus aliases and naming rules, noexcept and explicit status
errors, no exceptions, the standard library policy (ADR 0003), lightweight
headers, and keep everything private to apps/editor with Qt-free seam values.
Never raise budgets, weaken warnings or enable exceptions to pass a check. Do
not add a plugin API, terminal, Vim layer, minimap, Markdown preview, other
encodings, a Git client beyond change markers, or a cross-document undo history.
Never let a project setting name an executable, and never pass clangd
--query-driver.

Finish every milestone with its gate and the standard gates in tasks.md, plus
native interactive runs where the gate asks for them. Report every unavailable,
skipped or failed check accurately; do not mark a milestone complete without its
gate. Return changed paths, verification evidence, measured budgets and
remaining limitations. Open pull requests against develop; do not merge,
enqueue or push to develop or main.
```

## Prompt 2 - Continue one milestone at a time

Use this instead of Prompt 1 for smaller runs. Repeat it to advance through the
milestones without resetting the specification:

```text
Continue the Ludus code editor from .kiro/specs/code-editor/tasks.md. Read
AGENTS.md, .kiro/steering/, requirements.md, design.md and
docs/architecture/code-editor.md again. Revalidate the checkout and the ledger,
then complete the first milestone whose prerequisites are met and whose gate is
not yet recorded. Keep the batch reviewable, follow every principle and
constraint in the specification, run the milestone gate and the standard gates,
update the ledger with changed paths, commands, results and remaining gaps, and
open a pull request against develop. Report skipped or failed checks
accurately. Stop after that milestone.
```

## Prompt 3 - Review a milestone

```text
Review the latest code editor milestone pull request against
.kiro/specs/code-editor/requirements.md and design.md, AGENTS.md and the
architecture in docs/architecture/code-editor.md. Check edit-set atomicity,
stale-result rejection, position mapping, bounded capacities with single
owners, UI-thread work against the budgets, tool failure paths, and that
recorded evidence matches the gate. List concrete defects with file and line,
and the missing evidence. Do not rewrite the implementation.
```
