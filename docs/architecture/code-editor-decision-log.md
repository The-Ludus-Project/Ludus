# Code editor decision log

This log records material decisions and deviations taken while implementing the
[code editor](code-editor.md) through the
[specification](../../.kiro/specs/code-editor/tasks.md). Each entry names the
milestone, the decision or deviation, the evidence, and the consequence. The
architecture and the task ledger remain authoritative for the contract; this
file explains where the implementation departed from the proposed names or
assumptions and why. New entries are appended under their milestone.

## M0 - Revalidate and prepare

### Checkout baseline versus the architecture baseline

The architecture and specification were written against `develop` at
`eebc2f20` (the [baseline table](code-editor.md#baseline)). The checkout used
for this implementation is `develop` at `cb588721`, which is 20 commits ahead of
`eebc2f20`. The newer work that touches or neighbours the editor is:

- **PR #920 (`codex/code-editor-architecture`).** Adds the specification and the
  architecture pages themselves (`.kiro/specs/code-editor/*`,
  `docs/architecture/code-editor.md`, the shortcut-decision commit
  `eac150bc`). This is the handoff being implemented, not competing code.
- **PR #913 (`codex/curves-surfaces-g5`).** Adds editor-private curve and
  surface authoring adapters under `apps/editor` and a `Ludus::GeometryContent`
  pipeline. None of these touch the `code_*` editor files listed in
  [design.md section 1](../../.kiro/specs/code-editor/design.md#1-repository-fit);
  they are preserved unchanged.
- **PRs #918 and #919 (`codex/object-handles-stage-4-*`).** Content-side graph
  duplication as one transaction and a two-pass World restore. They do not touch
  the code-editor seam or views.

No competing implementation of the code-editor milestones exists on `develop` or
on any remote branch (`git branch -r | grep -i code-editor` returns nothing new
beyond the architecture branch). The existing editor source matches the
baseline table's "Implemented" column: `CodeEditor` over `QPlainTextEdit`;
`CodeDocumentHost` with the single-edit `ApplyEdit` seam
(`apps/editor/src/internal/code_document.h`); the render-graph package host;
the single-line Slang lexer in `CodeSyntaxHighlighter`; lazy `slangd`
assistance; explicit Format Document; and anchored compiler diagnostics.
Consequence: the implementation proceeds from `cb588721`; proposed names from
the design are used unless an existing convention differs, in which case the
deviation is recorded here.

### Sandbox toolchain cannot run the gates locally (deviation)

The implementation sandbox is Amazon Linux 2023 with only system Clang 15.0.7,
CMake 3.22.2, Ninja 1.10.2 and no Qt 6, no Conan, and no access to the pinned
LLVM 23 suite (no apt, no dnf LLVM-23 packages). This has three hard
consequences, re-confirmed at the start of M0:

- CMake 3.22 cannot parse `CMakePresets.json` (it needs the project's pinned
  CMake 3.29.6; `cmake --preset linux-clang-debug -N` fails with
  `Unrecognized "version" field`). No preset build or test runs locally.
- Clang 15 accepts only `-std=c++2b`, not `-std=c++23`, and the Foundation
  headers reach generated headers produced by CMake configure
  (`ludus/foundation/base/assert_config.hpp`), so even standalone
  `-fsyntax-only` checks of editor translation units fail before configure.
- `scripts/check --format` / `--tidy` require clang-format 23 / clang-tidy 23
  and (for tidy) a configured build with the C++-modules object map; none are
  available.

Therefore the [standard gates](../../.kiro/specs/code-editor/tasks.md#standard-gates)
(`./scripts/build`, `./scripts/test`, `./scripts/check --all`, the sanitizer
lanes, `./scripts/check-build-budget`, `scripts/check-docs`,
`scripts/check-wiki`) and the native interactive runs **cannot be executed in
this sandbox** and are deferred to batch CI on the pinned toolchain. This
matches the deferral recorded for the curves-surfaces phases (G2-G5) and the
Clang-23 toolchain pin ADR 0025, whose own "Consequences" note that Linux
builds, the Qt editor and the SDK consumer are first qualified by CI. Local
verification is limited to close reading against the C++23 contracts and the
naming, standard-library and seam rules in `AGENTS.md`. Every milestone's gate
report states which checks ran and which were deferred, and no milestone box is
ticked on the strength of a check that could not run here.

### Sequence diff owner

M0 must decide the owner of the Myers sequence diff used by the change-marker
feature (M9) and by whole-text formatter results (M4/M5). Decision: it is a
general FoundationContainers algorithm beside `sort.hpp`
(`modules/foundation/containers/include/ludus/foundation/containers/`), with its
own `[foundation]` tests, as design.md recommends. Rationale: the diff operates
on borrowed sequences of comparable tokens (bytes, hashed lines) and has no Qt
or editor dependency; placing it in FoundationContainers keeps it reusable and
exception-free, and keeps the editor free of a private copy. The concrete header
and tests are added in the milestone that first needs it (M4 for formatter
results, or M9 for change markers), not in M0, so the owner is fixed now and the
code lands with its first consumer and regression coverage.

### Code-buffer recovery journal owner

M0 must decide where unsaved code buffers are journaled for crash recovery
(M1). Decision: add a bounded code-buffer recovery journal under the project's
machine-local state directory, owned by the code document registry, rather than
overloading the render-graph or World draft recovery owners. Rationale: those
owners serialize structured documents (graph packages, World drafts), whereas a
code buffer is UTF-8 text plus a small header (canonical path, encoding, line
endings, revision). A dedicated journal keeps the code path independent, is
written off the UI thread on an idle debounce, and is bounded by its own owner
constant. The journal format and its owner constant are introduced in M1 with
the file document host; M0 only fixes the ownership decision.

### Shortcut resolution (F9 / Ctrl+K), decided 2026-11-03

The architecture's
["Settings, keymaps and EditorConfig"](code-editor.md#settings-keymaps-and-editorconfig)
section records the maintainer-accepted resolution; it is copied here so the
implementation in M2 has one place to check against, together with the existing
commands and tests it affects.

- **Record Profiling** moves to **Ctrl+Alt+F9** in every keymap profile and in
  every focus context.
- **F9** toggles a breakpoint when focus is in a code editor; elsewhere it is
  unassigned.
- The **Ludus** profile keeps **Ctrl+K** for the command palette.
- The **VS Code** profile opens the palette with **Ctrl+Shift+P** and frees
  **Ctrl+K** for chords (for example Ctrl+K Ctrl+C).
- The shell's shortcut-collision tests enforce these bindings in both profiles.

Existing commands, bindings and tests this resolution affects, to be updated in
the same M2 change that applies it:

- The Record Profiling command and its current F9 binding in the editor command
  catalogue and menus.
- Documentation that currently names F9 for profiling:
  `docs/development/editor-workspace.md` and
  `docs/architecture/graphics-debugging.md`.
- The breakpoint command (new in M2's keymap work; its gutter action is wired in
  M8) must claim F9 only within a code-editor focus context.
- The command-palette binding, split per profile (Ctrl+K for Ludus,
  Ctrl+Shift+P for VS Code), and the new shortcut-collision shell tests for both
  profiles.

### Baseline corpus and latency probe corrections (2026-11-03)

The corpus and probe that M0 authored in the sandbox were first built on an
Apple M5 to record the baseline (see the
[task ledger](../../.kiro/specs/code-editor/tasks.md#m0-baseline-2026-11-03)).
Four defects were fixed before any numbers were recorded:

- **No output.** Catch2's `INFO` messages were scoped to the reporting helper
  and only printed on failure, so a passing run printed nothing. Each workload
  now prints one `WARN` summary line.
- **Wrong p95.** The reported p95 was the maximum of 20 samples. The corpus now
  takes 100 samples and reports the nearest-rank p50 and p95 and the maximum
  separately.
- **Key handling skipped.** Edits were inserted programmatically, which bypassed
  `CodeEditor::keyPressEvent`, where the probe and the editing behavior live.
  Typing workloads now send real key events. The 100-position case stays
  programmatic until M2 adds multiple cursors, and its output says so.
- **Probe started after handling.** The architecture says the probe timestamps
  the key "when it arrives", but `MarkKeyHandled()` started the timer only after
  handling. `CodeLatencyProbe::MarkKeyArrived()` now records the arrival at the
  top of `keyPressEvent`, and a sample includes handling. Regression tests cover
  the arrival timing and keys that change no text.

The probe also replaced `std::sort` (`<algorithm>`, disallowed in `apps/` by
ADR 0003) with `foundation::Sort` from FoundationContainers.

### Language registry skeleton

M0 introduces the language registry skeleton with Slang and plain text only,
replacing the `CodeLanguage` enum's role as a hard-coded switch selector without
changing behavior. The enum becomes an index into a compiled-in registry table,
as design.md section 1 proposes. The skeleton holds, per language, only what the
current lexical editor already needs (identifier, display name, file patterns,
comment tokens, bracket and auto-closing pairs, and the lexical-profile flag the
view and highlighter already branch on). Server, formatter, debugger, grammar
and query fields are added by their owning milestones (M3 grammars, M4 servers,
M5/M6 providers, M8 debuggers). The existing `CodeLanguage::PlainText` and
`CodeLanguage::Slang` values keep their numeric identity so persisted settings
and the render-graph Source pane behave exactly as before; this is a structural
move covered by the existing `code_editor` and `code_document` tests, separate
from any behavior change.

One intentional micro-deviation: the Slang record declares both `.slang` and
`.slangh` suffixes (the architecture's
[language table](code-editor.md#language-registry)), whereas the render-graph
Source pane's previous inline test matched only `.slang`. The Source pane is
only ever shown files already in the render-graph package (all `.slang`), so no
observable behavior changes there; the registry carries the correct contract for
the ordinary-file host added in M1. This is recorded as evidence rather than
left implicit.

## M1 - Every text file is a document

### M1 gate report (2026-11-04)

M1 makes every project text file open as a code tab (CE01-CE04, CE20, CE22,
CE23). It landed in batches, each as its own pull request into `develop`:

- Tasks 1-4 - the file document host, the code document registry with preview
  tabs, two editor groups, view mirroring and per-project tab restore, the
  navigation routing (Project Explorer, Quick Open, Error List, render-graph
  Source pane, Open Externally), and the Scripts work area moved onto
  `CodeEditor` through a script host - merged as #927-#930. The maintainer's
  macOS build of that merged stack found and fixed four issues, recorded here so
  they are not reintroduced: a missing test include, four ignored
  `[[nodiscard]]` results, a tab-teardown crash (the `CodeDocumentController`
  must be destroyed before its `CodeEditor`, in `CodeDocumentTab` and
  `ScriptWorkspace`), and `ScriptCodeDocument::SetBaseline` clearing history when
  Save should keep it (Save now uses `MarkSaved`; the external-change test no
  longer depends on the clock). Fixed by #936 and #938.
- Task 5 - the code-buffer recovery journal and the dirty-close prompts. The
  journal is owned by `CodeDocumentRegistry`, lives under the project's
  machine-local state directory with its own per-entry owner constant
  (`CodeRecoveryJournal::MAX_RECOVERY_ENTRY_BYTES`), and is written off the UI
  thread on an idle debounce by a background `QThread`; unsaved buffers restore
  on reopen, re-anchored against the current on-disk baseline. Closing a dirty
  tab asks Save/Don't Save/Cancel (Cancel default); closing the project or
  quitting lists every dirty code buffer.
- Task 6 - the code status bar: cursor position, selection, indentation,
  encoding, line endings, language and the language-server placeholder, with the
  position, language and line-ending items clickable where M1 supports the
  change. The language-server item is a placeholder until M4.
- Task 7 - this report, the `development/code-editor.md` user guide (registered
  in MkDocs and the documentation index), and the `editor-workspace.md` update.

**Gate status - pending host verification.** Every native item below is
**pending host verification**: the pull-request CI skips the editor build and
tests, so a green PR check does not mean the editor code compiled, and this
sandbox has no Qt6 and no pinned toolchain (see
[Sandbox toolchain cannot run the gates locally](#sandbox-toolchain-cannot-run-the-gates-locally-deviation)).
The M1 code for tasks 5-7 was authored and read closely against the C++23,
naming, standard-library (ADR 0003) and Qt-free-seam rules, but was **not
compiled or run** here; it is marked "authored, not run" in its pull requests
for the maintainer to build and test on macOS. Specifically pending on a
toolchain-capable host:

- `./scripts/build` and `./scripts/test` for the debug preset, including the new
  `ludus_code_editor_tests` cases (crash-style restore, a journal for a deleted
  file, a journal over its limit, a regression per dirty-close answer, and the
  status-bar items and change requests).
- The ASan/UBSan lane, `./scripts/check --all` (clang-format and clang-tidy at
  the pinned version), `./scripts/check-build-budget`, `python scripts/check-docs`
  and `python scripts/check-wiki --documentation-only`.
- The gate's recorded native run: every listed file type opening, editing,
  saving, conflicting and recovering, with a forced-quit recovery pass, and the
  existing render-graph and Scripts tests still passing.

Each batch started from freshly fetched `origin/develop` rather than stacking on
an already-merged branch, and each touched file includes the headers for the
types it uses, checks every `[[nodiscard]]` result, and makes Qt child
destruction order explicit wherever one child's destructor uses another (the
recovery writer is joined in `~CodeDocumentRegistry` before the base `QObject`
deletes it; the controller-before-editor teardown from #938 is left intact).

### M1 host verification (2026-11-04)

The maintainer built develop `d8a3e050` (all seven M1 batches) with
`macos-clang-development` on macOS. It compiled warning-clean, and the offscreen
`ludus_code_editor_tests` passed. Reading the code against the gate found M1
incomplete. These gaps are fixed in the follow-up pull request:

- **Save.** File > Save (Ctrl+S) ignored code tabs, so a code tab could only be
  saved by closing it. `CodeEditorArea::SaveTab`/`SaveActiveTab` now back the
  command, which reads **Save File** while a code tab is active.
- **External changes.** Nothing called `HasExternalChange`, and the conflict
  bar's buttons were not connected. `CodeExternalChangeWatch` now watches each
  open file and its directory. The area also re-checks on tab activation and
  application activation, which covers late FSEvents and the browser editor.
  A clean buffer reloads, and a dirty buffer shows Compare (read-only, side by
  side), Keep Mine and Reload. Save refuses to write over an undecided change.
- **Don't Save resurrected drafts.** Discarding on project close cleared the
  journal but left the drafts in memory. The close-time flush then wrote them
  back, and the next open recovered them. Don't Save now discards the drafts in
  memory too (`DiscardDraft`, `DiscardDirtyDrafts`).
- **Recovered drafts were dropped silently.** A draft whose file changed on disk
  after the crash was skipped. It is now restored with the conflict bar.
- **A second crash lost restored drafts.** The restore pass cleared the journal
  and nothing rewrote it until the next edit. The registry now treats every
  journal entry as journaled and starts a checkpoint right after the restore.
  Saving also starts an immediate checkpoint, so a saved buffer's entry is
  removed without waiting for the debounce. Switching projects and quitting both
  flush.
- **Split views.** Closing one of two views of a dirty file asked Save or Don't
  Save, and Don't Save reloaded the shared host under the other view. Only the
  last view of a dirty draft asks now.

Remaining differences, recorded rather than fixed here:

- The registry holds hosts through `std::shared_ptr`, which ADR 0003 does not
  allow. Migrating it is a separate change.
- The journal lives under the application's local data directory, scoped per
  project, rather than the project's machine-local state directory. It writes
  through a per-flush `QThread` rather than the shared background pool. Every
  checkpoint rewrites every dirty draft.
- A recovered draft whose file was deleted is not reopened.
- `HasExternalChange` reads the file on the UI thread when size or time differ.
- Compare shows both texts side by side without highlighting the differences.
- The recorded native run of the gate (every file type opening, editing,
  saving, conflicting and recovering, with a forced quit) is still pending.
- The full `ludus_editor_tests` run fails one render-graph test, "mouse Back and
  Forward buttons navigate graph history from the canvas and its toolbar". It
  passes alone, and it failed before M1 tasks 5-7 too, so an earlier test
  leaves state behind.

### M2 batch 1: edit sets (CE05, 2026-11-04)

The document seam gained `CodeEditSet` and `CodeSelectionSet` (design section 2):
an ordered, non-overlapping set of `CodeTextEdit`s with a `Before`/`After`
selection set and a `CodeTransactionId`. `CodeDocumentHost::ApplyEdits` is the
new primary edit entry point; `ApplyEdit` stays a virtual one-element wrapper
over it, so existing single-edit call sites (the controller's typing path) are
unchanged. The whole-set validation (ascending order, no overlap, UTF-8 scalar
boundaries, cumulative capacity, all or nothing) lives in one shared helper,
`ValidateAndApplyEditSet` in `code_edit_set.{h,cpp}`, which the file, script,
render-graph and test hosts all call, replacing the three near-identical private
`applyByteEdit` bodies.

Decisions and deviations recorded here:

- **One undo step restores text and selections.** `DocumentHistory` stores only
  the payload `T`, with no selections. Rather than rearchitect it, the file host
  now instantiates `DocumentHistory<CodeHistoryState>`, where `CodeHistoryState`
  carries the UTF-8 text and the selection set and compares **text only**, so a
  bare caret move still records no history. `CodeFileDocument::Undo`/`Redo` now
  surface the step's selection set; `CodeDocumentController::RestoreSelectionSet`
  restores it. The script host stays `DocumentHistory<QByteArray>` and keeps its
  single-cursor `Undo()`/`Redo()`: the Scripts work area is not multi-cursor.
- **RestoreSelectionSet restores the primary selection only for now.** The view
  still carries one Qt cursor; the secondary cursors are a batch-2 (CE06) layer.
  The selection set is already recorded per step, so batch 2 restores the rest
  without another history change.
- **Formatting and Replace All moved onto the seam.** The formatter publishes its
  clang-format replacement ranges as one `CodeEditSet` (they were already an
  ascending, validated replacement list) through the new
  `CodeDocumentController::ApplyEditSet`, instead of collapsing to a whole-text
  `ApplyServiceText`. `CodeEditorPanel` gained an optional bound controller so
  Replace All builds a byte-offset edit set and applies it whole; with no bound
  controller it falls back to the Qt edit-block path, which still lands as one
  undo step. `ApplyServiceText` is retained for completion/whole-text callers.
- **The render-graph host forwards one combined edit.** It owns no buffer, so
  `ApplyEdits` validates the whole set against the package draft and forwards a
  single `RenderGraphDocument::ApplyTextEdit`, keeping the package's one-step
  history. No package change was needed.
- **Status for a rejected set.** An unordered or overlapping set returns
  `Conflict` (not a new status), matching the single-edit identity-mismatch
  rejection; the buffer, history and selections are unchanged.

Authored, not run: the editor was not built or tested in this sandbox (no Qt6
and no pinned toolchain; PR CI skips the editor build and tests). The new
`code_edit_set_tests` (overlap/unordered/stale rejection with the buffer
unchanged, and one-undo-step text-and-selection restore) and the migrated
formatter/Replace All paths are pending host verification by the maintainer.

**Host verification (2026-11-04).** Built and tested on macOS: the batch
compiled and its tests passed. Replace All was quadratic in the file size. Each
match was mapped from the start of the text, and the validator re-validated the
UTF-8 prefix up to every edit and selection offset. On a 1.2 MB file, 65,536
matches took 78 s to map and 23 s to validate. The fix:

- Host drafts are validated UTF-8, so `ValidateAndApplyEditSet` checks
  boundaries in constant time (`IsCodeScalarBoundary`) and validates only the
  inserted bytes. Its cost is linear in the original plus the inserted bytes,
  for any number of edits or selections. This is a precondition on the function.
- `AdvanceCodeByteOffset` maps ascending positions by walking on from the
  previous one. Replace All, and later the multi-cursor and Replace in Files
  paths, map their positions in one pass.
- Replace All finds matches in one `QString::indexOf` pass over the decoded
  draft. `QTextDocument::find` cost about 100 µs per match.

Mapping and validating 65,536 edits now takes about 10 ms, and a 16,384-match
Replace All through the panel about 80 ms. Regressions in `code_edit_set_tests`
cover both, plus CRLF, case and non-ASCII matching. Later batches must keep
per-edit and per-cursor work constant-time or amortized over one pass.

### M2 batch 2: multiple cursors (CE06, 2026-11-05)

The view gained a secondary-cursor overlay, `CodeMultiCursor`
(`internal/code_multi_cursor.h/.cpp`), owned by `CodeEditor` as a plain value
member bound to the view. The primary cursor stays Qt's own `QTextCursor`, so
Qt's accessibility, IME and clipboard still see one cursor (design section 5);
the secondary cursors are the overlay's own UTF-16 anchor/caret regions, kept
sorted by start in document order and merged whenever they touch the primary or
each other. The overlay implements add next occurrence, add all occurrences,
cursors above and below, Escape-to-collapse, Alt+drag column (box) selection,
movement for every cursor through Qt's own `QTextCursor` operations, and input
distribution for typing, Backspace, Delete, Return, Tab and paste.

Decisions and deviations recorded here:

- **Every multi-cursor text operation is one `CodeEditSet`, mapped in one pass.**
  `CodeMultiCursor` builds an ordered region set in the view's UTF-16 coordinates
  and hands it to the new `CodeDocumentController::ApplyMultiCursorEdit`, which
  maps every region's start and end to byte offsets by walking forward from a
  single `CodePositionOffset` with `AdvanceCodeByteOffset` (never
  `GetCodeByteOffset` per cursor), encodes each region's inserted text once,
  computes every after-caret from a running byte delta without a second walk, and
  publishes the whole set through the existing `ApplyEdits` seam. A set over
  40,000 cursors is one undo step and one pass over the text. This follows the
  host-verification rule from batch 1 that per-edit and per-cursor work stays
  amortized over one pass; a timing regression in `code_multi_cursor_tests`
  guards it against a wall-clock ceiling at a realistic size.
- **Byte-to-position mapping gained its ascending mirror.** Restoring a
  multi-cursor selection set maps many ascending byte offsets back to Qt
  positions, which `GetCodeCursorPosition` did only from the start. A new
  `AdvanceCodeCursorPosition` (the mirror of `AdvanceCodeByteOffset` added in
  #983) walks forward from a previous pair, so
  `CodeDocumentController::RestoreSelectionSet` maps a whole selection set in one
  pass. `RestoreSelectionSet` now restores the primary as the single Qt cursor
  and the rest as the overlay's secondary cursors, completing the batch-1 note
  that one undo step restores the whole set.
- **Occurrence search is one `QString::indexOf` pass, not `QTextDocument::find`.**
  Add next and add all occurrences search the decoded text with
  `QString::indexOf`, matching the Replace All fix in #983, so adding a cursor to
  every match in a large file is linear.
- **The controller finds the overlay, not the reverse.** The controller sets
  itself on the editor's overlay in its constructor and clears it in its
  destructor (before `SetDocumentHistory(false)`), so an unbound view keeps the
  overlay inert and every multi-cursor text operation is a no-op. No Qt
  parent-chain lookup is used.
- **The status bar shows the cursor count.** `CodeStatusBar` now shows
  "N cursors" from `CursorCountChanged`, replacing the batch-1 placeholder note.
- **Key bindings are left to batch 6, except where they do not collide.** The
  add-cursor commands are public `CodeEditor` methods (`AddCursorAbove`,
  `AddCursorBelow`, `AddNextOccurrence`, `AddAllOccurrences`,
  `ClearSecondaryCursors`), reachable from the overlay, menus and the palette and
  exercised directly by the CE06 tests. Only the collision-free bindings are set
  here: Ctrl+Alt+Up/Down (cursor above/below), Ctrl+Shift+L (add all
  occurrences), Escape (collapse), and Alt+drag (column selection). Add next
  occurrence is deliberately NOT bound to Ctrl+D, which is already Duplicate
  Lines; the per-profile resolution of that conflict is batch 6's keymap work.
  Input distribution (typing, Backspace, Delete, Return, Tab, movement and
  paste) is behavior, not a binding, and runs whenever a secondary cursor exists.

Authored, not run: the editor was not built or tested in this sandbox (no Qt6
and no pinned toolchain; PR CI skips the editor build and tests). The new
`code_multi_cursor_tests` (typing/Backspace/selection-replace distribution as one
undo step, add next/all occurrences, cursor below, Escape collapse, paste
line-per-cursor and whole-text, CRLF and non-ASCII mapping, and the
40,000-cursor timing ceiling) and the `AdvanceCodeCursorPosition` and
`RestoreSelectionSet` changes are pending host verification by the maintainer on
macOS. Pending specifically: `./scripts/build` and `./scripts/test` for the debug
preset, `./scripts/check --all` (clang-format and clang-tidy at the pinned
version), the ASan/UBSan lane, and a recorded native run of multiple cursors
(typing, column selection, add occurrence and paste) including IME and
VoiceOver, which offscreen tests do not qualify.

### M2 batch 3: the find overlay (CE07, 2026-11-05)

The bottom find panel's find/replace rows are replaced by a floating overlay,
`CodeFindOverlay` (`internal/code_find_overlay.h/.cpp`), that sits over the top
of the editor's viewport (design section 4). It adds regex, whole word, in
selection, a live match count and preserve-case replace to the existing find,
Previous/Next, replace and Replace All. `CodeEditorPanel` keeps its editing
toolbar (text size, wrap, guides, position, diagnostics) and Go to Line, and now
hosts the overlay instead of the inline rows; the overlay is parented to and
owned by the editor so it tracks the viewport and takes no layout space.

Decisions and deviations recorded here:

- **The overlay keeps the panel's find object names and status strings.** The
  find field, replacement field, replace / Replace All buttons, the status label
  and the "Match case" check box keep `codeEditorFind`, `codeEditorReplacement`,
  `codeEditorReplace`, `codeEditorReplaceAll`, `codeEditorFindStatus` and their
  text, so the existing editor and edit-set tests (and the "%1 replacements" /
  "No matches" strings) are unchanged. The new options add
  `codeEditorRegex`, `codeEditorWholeWord`, `codeEditorInSelection` and
  `codeEditorPreserveCase`.
- **One regex engine for find, count and replace.** The query is compiled once
  into a `QRegularExpression` (literal queries are `QRegularExpression::escape`d;
  whole word wraps the source in `\b(?:...)\b`; case follows Match case). Find,
  the match count and Replace All all collect matches in one global pass over the
  decoded text, never `QTextDocument::find` in a loop. An invalid regex is
  reported inline and changes nothing.
- **Replace All stays linear, like #983.** It builds the ordered edits and maps
  every match's editor position to a byte offset by walking forward from one
  `CodePositionOffset` with `AdvanceCodeByteOffset`, then publishes one
  `CodeEditSet` through `CodeDocumentController::ApplyEditSet`. The common case (a
  literal find/replace with no regex or preserve case) reuses one replacement
  string and needs no per-match re-match, so a large Replace All is one pass. A
  timing regression in `code_find_overlay_tests` replaces 16,384 regex matches
  through the overlay against the same wall-clock ceiling as the edit-set tests.
  With no bound controller the Qt edit-block fallback applies the edits from the
  end as one undo step.
- **In selection freezes its range.** The target range is captured when In
  selection is turned on, so edits do not shrink it mid-operation, and matches
  that would cross the boundary are excluded.
- **Preserve case** matches an occurrence's all-upper, all-lower or capitalized
  pattern onto the replacement; a mixed-case occurrence is left as authored.

Authored, not run: the editor was not built or tested in this sandbox (no Qt6
and no pinned toolchain; PR CI skips the editor build and tests). The new
`code_find_overlay_tests` (regex/whole-word/in-selection/preserve-case Replace
All, match count with "N of M", invalid-regex reporting, find-next wrap and the
16,384-match timing ceiling) and the unchanged edit-set and editor find tests
are pending host verification by the maintainer on macOS: `./scripts/build`,
`./scripts/test`, `./scripts/check --all`, the ASan/UBSan lane, and a recorded
native run of the overlay including its floating placement and a screen reader.

### M2 batch 4: search and Replace in Files (CE07, CE11, 2026-11-05)

The editor gained project-wide search and Replace in Files (design sections 7
and 8): a background search service, a Search tool window, and a workspace-edit
path on the registry. New files: `internal/code_search_service.h/.cpp`,
`internal/search_tool_window.h/.cpp`, the owner constants in
`internal/code_file_limits.h`, and `CodeDocumentRegistry::ApplyWorkspaceEdit`.

Decisions and deviations recorded here:

- **Matching is one pure pass, shared by the worker and the tests.**
  `FindMatchesInText` compiles the query once into a `QRegularExpression`
  (literal queries escaped, whole word wrapped in `\b(?:...)\b`,
  UseUnicodePropertiesOption off per section 8) and walks the decoded text once,
  deriving each match's line and column from a running offset that only moves
  forward — never a per-match scan from the start, and never
  `QTextDocument::find`. It stops at the passed cap so the whole search is bounded
  by `CODE_SEARCH_MAX_MATCHES`.
- **The worker mirrors the recovery writer.** `CodeSearchService` owns a per-job
  `QThread` that deep-copies its inputs (roots, exclusions, query, and a snapshot
  of every dirty buffer), enumerates the authored roots, skips the excluded
  `out/` and `.git` prefixes, binary files (a NUL in the first
  `CODE_SEARCH_BINARY_PROBE_BYTES`) and files over `CODE_SEARCH_FILE_LIMIT_BYTES`,
  and streams results back in batches of at most `CODE_SEARCH_RESULT_BATCH`
  through queued signals. A dirty file is searched from its passed-in draft, not
  disk. Only one worker runs at a time; Start cancels and joins the previous one.
- **Replace in Files is one workspace-edit transaction (section 7).**
  `ApplyWorkspaceEdit` mints a non-zero `CodeTransactionId` (its first use;
  `CodeEditSet::Transaction` was threaded through but unused until now), acquires
  a host per file through the registry, maps each file's editor-coordinate
  replacements to byte edits in one `AdvanceCodeByteOffset` pass, and VALIDATES
  every file against its snapshot before changing anything. If any file fails, or
  the edit exceeds `CODE_MAX_WORKSPACE_EDIT_FILES`, nothing is applied (Rejected).
  On success each file applied its slice as one undo step carrying the shared
  transaction id and was saved; a save failure leaves the applied edit in a dirty
  host (`AppliedButSaveFailed`) so no work is lost. Every acquire is balanced by a
  release, so a host opened only to apply is dropped afterwards while a host a
  view already held keeps its undo history. A timing regression rewrites 64 files
  of 2,000 lines under a wall-clock ceiling.
- **The results tree is the preview.** The Search tool window shows every match
  grouped by file; Replace in Files turns exactly those shown matches into the
  workspace edit, so the user reviews before applying. Each match carries its
  absolute editor offsets, so Replace in Files needs no re-find. Activating a
  result emits `OpenRequested(path, line, column)`, which the shell routes to
  `openCodeFile` + `CodeEditor::GoToLine`.
- **The scope provider avoids a shell dependency in the window.**
  `CodeSearchScopeProvider` is a small Qt-free interface the window queries for
  roots, exclusions and dirty buffers at each run; `CodeAreaSearchScope`
  implements it over the code area's registry and a project root the shell
  updates. `CodeDocumentRegistry::GetDirtyDrafts` returns copied snapshots safe
  to hand to the worker.
- **Shell wiring.** The Search dock is a `QDockWidget` on `MainWindow`, tabbed
  with the Error List in the bottom area, listed in `getToolWindows()` with
  `LAYOUT_VERSION` bumped to 7 so saved layouts reset to include it, and given a
  plain View-menu entry. Its keymap binding (Find in Files) is deferred to batch
  6 with the keymap profiles, to avoid a premature shortcut collision.

Authored, not run: the editor was not built or tested in this sandbox (no Qt6
and no pinned toolchain; PR CI skips the editor build and tests). The new
`code_search_tests` (one-pass line/column mapping, regex/whole-word/case/cap,
root enumeration with exclusions, binary and dirty-buffer handling, the
all-or-nothing workspace edit, and the 64-file timing ceiling) and the shell
wiring are pending host verification by the maintainer on macOS: `./scripts/build`,
`./scripts/test`, `./scripts/check --all`, the ASan/UBSan lane, and a recorded
native run of a project search, result navigation and Replace in Files including
a screen reader. The worker runs real threads, so the offscreen tests pump the
event loop for its queued signals.

### M2 batch 5: registry-driven pairs and indentation (CE17, 2026-11-05)

Auto-closing, surrounding pairs and smart indentation now come from the language
registry instead of a hard-coded Slang set. `CodeLanguageDescriptor` gains
`AutoClosingPairs` (brackets plus the string quote) and `SurroundingPairs`
(brackets plus string and character quotes) beside the existing `BracketPairs`;
the Slang record fills all three and plain text leaves them empty.

Decisions and deviations recorded here:

- **The view reads the registry, not the `CodeLanguage::Slang` enum.** The
  auto-closing block in `CodeEditor::handleKeyPress` is gated on
  `languageHasStructuralEditing()` (the descriptor's `HasStructuralEditing`) and
  looks pairs up through `autoClosingCloseFor`, `isCloseOfAutoClosingPair`,
  `isOpenBracket` and `bracketCloseFor`, which query the current descriptor.
  Adding a language's pairs is now a registry record, not a view change. The
  Slang-only comment/string exclusion (via the highlighter's `CodeBlockData`
  excluded ranges) is preserved, so a bracket typed inside a comment or string
  still does not pair.
- **Surrounding pairs are new behavior.** Typing an opening bracket or quote
  with a selection wraps the selection in the pair and keeps it selected, rather
  than replacing it; previously any selection suppressed pairing entirely. The
  wrap is one undo step.
- **Smart indentation generalized from `{`.** Return after a line whose last
  non-space character opens any registry bracket pair indents the next line, and
  when that bracket's matching close is right after the caret the close drops
  onto its own dedented line. The old code hard-coded `{`/`}`.
- **The preference and the step-over behavior are unchanged.** The existing
  Auto-close Brackets preference (`mShouldPairBrackets`) still gates pairing, and
  typing a closing character that already sits next to the caret steps over it,
  as before; a quote whose open equals its close uses the same step-over so a
  closing quote is not duplicated. Bracket-match highlighting (a separate,
  matching-only feature) keeps its fixed set and is out of scope here.

Authored, not run: the editor was not built or tested in this sandbox (no Qt6
and no pinned toolchain; PR CI skips the editor build and tests). The new
`code_pairs_tests` (auto-close, quote close/step-over, Backspace-deletes-pair,
surround with bracket and quote, Return smart indent, and the plain-text opt-out)
and the unchanged bracket-pairing test are pending host verification by the
maintainer on macOS: `./scripts/build`, `./scripts/test`, `./scripts/check --all`,
the ASan/UBSan lane, and a recorded native run including IME, which offscreen
tests do not qualify.

### M2 batch 6: keymap profiles (CE07/CE17 shortcut resolution, 2026-11-05)

The editor gained two keymap profiles with user overrides, conflict detection
and a first-use choice, and applied the shortcut resolution decided 2026-11-03
(copied into this log under M0). New files: `internal/code_keymap.h/.cpp`.

Decisions and deviations recorded here:

- **`CodeKeymap` owns the profiles.** It holds the per-profile binding tables
  (Ludus and VS Code), the user's overrides and the active profile (persisted in
  `QSettings`), resolves the effective binding per command (override, else
  profile, else none), detects collisions, and applies the resolved shortcuts
  onto the shell's QActions by object name (the command id). It is unit testable
  without a window.
- **The decided bindings.** Record Profiling moved from F9 to Ctrl+Alt+F9 in both
  profiles and every focus context (the `PROFILING_RECORD_COMMAND` catalogue
  default changed too), freeing F9 to toggle a breakpoint when a code editor has
  focus. The Ludus profile keeps Ctrl+K for the command palette; the VS Code
  profile opens the palette with Ctrl+Shift+P and frees Ctrl+K for chords.
- **Folding bindings are reserved per profile, not shared.** Because the Ludus
  profile keeps Ctrl+K for the palette, it cannot also use Ctrl+K chords, so its
  fold bindings are single Ctrl+Shift keys; the VS Code profile uses the Ctrl+K
  chords it is known for. `DetectConflicts` was strengthened to treat a single
  key that opens another binding's chord as a collision (a complete Ctrl+K would
  swallow the first stroke of a Ctrl+K chord), so "both profiles are
  collision-free" is a real guarantee, not an artefact of chord-vs-single
  inequality. The fold commands carry no catalogue default; the keymap owns their
  binding and applies it to the code view when the fold commands land in M3.
- **The breakpoint command is registered now.** `editor.toggleBreakpoint` (F9 in
  both profiles) plus the three fold commands are registered on each `CodeEditor`
  with a WidgetWithChildren shortcut context, so they only fire while a code view
  has focus, and emit a request the owner will act on when the gutter (M8) and
  fold model (M3) land.
- **The first-use choice is interactive only.** On the first run with no stored
  profile the shell offers the Ludus / VS Code choice once (a modal in
  `showEvent`) and persists it; it defaults to Ludus. Offscreen (headless tests
  and CI) never shows the modal, so the default Ludus profile is used there,
  which keeps the existing command-palette shell test (Ctrl+K) green.
- **Documents updated.** `docs/development/editor-workspace.md` and
  `docs/architecture/graphics-debugging.md` now name Ctrl+Alt+F9 for Record
  Profiling and F9 for Toggle Breakpoint, and the workspace guide describes the
  two profiles and the first-use choice.

Authored, not run: the editor was not built or tested in this sandbox (no Qt6
and no pinned toolchain; PR CI skips the editor build and tests). The new
`code_keymap_tests` (both profiles collision-free, the decided bindings resolve,
the fresh-install default and persisted choice, and an override that takes
precedence and is detectable as a conflict) and the updated `profiling_tests`
assertion (Ctrl+Alt+F9) are pending host verification by the maintainer on
macOS: `./scripts/build`, `./scripts/test`, `./scripts/check --all`, the
ASan/UBSan lane, and a recorded native run of the first-use choice, both
profiles and an override, which offscreen tests do not qualify.

### M2 batch 7: typing budgets (CE21, 2026-11-05)

The `[.benchmark]` corpus gained two workloads that measure typing through the
real M2 multi-cursor edit-set path: `1-cursor typing (edit-set path)` and
`100-cursor typing (edit-set path)`. Both bind a `CodeFileDocument` host through
a `CodeDocumentController`, place the cursors (the primary plus 99 secondaries
for the 100-cursor case) through the `CodeMultiCursor` overlay, send a real key
event through `CodeEditor::keyPressEvent`, and record handling and key-to-paint
latency with the `CodeLatencyProbe`, undoing between samples. This replaces the
M0 note that the 100-position case stayed a programmatic edit block "until M2
adds multiple cursors": it now exercises the production path (one `CodeEditSet`
distributed across every cursor, then a view re-synchronize), which is what the
architecture's 8 ms 100-cursor budget governs. The single-cursor case uses the
same path so the two are directly comparable.

Authored, not run: the corpus records numbers only when built and run on a
reference machine (no Qt6 and no pinned toolchain in this sandbox; PR CI skips
the editor build and tests), exactly as the M0 baseline did (recorded on an
Apple M5). The two new workloads print one WARN summary each; the maintainer runs
`ludus_code_editor_tests "[.benchmark][code-editor]"` on a quiet reference
machine and records the summaries against the 1-cursor (4 ms) and 100-cursor
(8 ms) budgets. Until then the typing budgets for M2 are pending that run.

### M2 gate report (2026-11-05)

M2 (editing parity: CE05-CE07, CE17) landed in seven batches, each its own pull
request into `develop`, each started from a freshly fetched `origin/develop`:

- Batch 1 - the edit-set seam (`CodeEditSet`/`CodeSelectionSet`), formatting and
  Replace All moved onto it. Host-verified and made linear in #983.
- Batch 2 - multiple cursors (CE06): the secondary-cursor overlay, one
  `CodeEditSet` per operation mapped in one pass, `AdvanceCodeCursorPosition`.
- Batch 3 - the find overlay (CE07): regex, whole word, in selection, match
  count, preserve-case replace, one regex engine for find/count/replace.
- Batch 4 - search, the Search tool window and Replace in Files (CE07, CE11):
  the background search service, the workspace-edit transaction on the registry.
- Batch 5 - registry-driven auto-closing, surrounding pairs and smart indent
  (CE17).
- Batch 6 - the Ludus and VS Code keymap profiles (CE07/CE17 shortcut
  resolution), overrides, conflict detection and the first-use choice.
- Batch 7 - the 1-cursor and 100-cursor typing-budget workloads (CE21).

**Gate status - authored, not run.** Every batch was authored and read closely
against the C++23, naming (source_policy.py, validated locally), standard-library
(ADR 0003) and Qt-free-seam rules, and the per-edit/per-cursor one-pass rule the
#983 host verification set; each multi-edit path carries a wall-clock timing
regression. But this sandbox has no Qt6 and no pinned toolchain and the PR CI
skips the editor build and tests, so none of the editor code was compiled or run
here. Pending on a toolchain-capable host (macOS), for the merged stack:
`./scripts/build` and `./scripts/test` for the debug preset (the new
`code_multi_cursor_tests`, `code_find_overlay_tests`, `code_search_tests`,
`code_pairs_tests`, `code_keymap_tests` and the updated `code_edit_set_tests`),
`./scripts/check --all` (clang-format and clang-tidy at the pinned version), the
ASan/UBSan lane, the `[.benchmark]` 1-cursor and 100-cursor numbers against the
budgets, and the recorded native runs the individual batch entries list
(multiple cursors with IME and a screen reader, the find overlay, project search
and Replace in Files, surrounding pairs, and the keymap first-use choice and
both profiles).

### M2 host verification (2026-11-05)

Built on macOS with the pinned toolchain and `LUDUS_WARNINGS_AS_ERRORS=ON`, then
ran `ludus_code_editor_tests` and `ludus_render_graph_configuration_tests`
offscreen. The merged M2 stack did not compile, and five tests failed once it did:

- **Build.** `code_editor.cpp` iterated `BorrowedView` with range-for, which it
  does not support (index loops now). `CodeEditor` called `CreateEditorCommand`,
  defined only in the application target, so every test executable linking
  `ludus_editor_code` failed to link (`editor_commands.cpp` moved into
  `ludus_editor_code`). Three ignored `[[nodiscard]]` results in
  `code_search_tests.cpp`.
- **Quadratic regex matching.** The find overlay's match count, Replace All and
  the project search called `QRegularExpression::match` once per occurrence.
  Each call validates the whole subject as UTF-16, so 16,384 matches took
  6.5–8.5 s and ran again on every find-field keystroke. All three now walk one
  `globalMatch` iterator, which validates once. The search also finds each
  line's end once per line instead of once per match.
- **VS Code chords never fired.** The profile wrote chords as `Ctrl+K Ctrl+0`;
  `QKeySequence` separates strokes with `, `, so both folding chords parsed to
  an empty sequence and collided with each other. A test now requires every
  built-in shortcut to round-trip through `QKeySequence`.
- **Enter in the find field replaced the match.** `QLineEdit` ignores Return
  after `returnPressed`, so the key reached the editor beneath the overlay and
  replaced the selected match with a line break. The overlay accepts keys its
  fields leave unhandled.
- **A quote did not step over its own close.** The caret before a closing quote
  is inside the string, which the excluded ranges reject, so the second `"`
  inserted a third. Symmetric pairs now step over inside strings; brackets
  still need a position outside strings and comments.

Kiro's sandbox cannot compile, so the "authored, not run" label covered
compiling as well as running. `AGENTS.md` and `.kiro/steering` now require a
warnings-as-errors build before a pull request, or a draft titled `[unbuilt]`.
