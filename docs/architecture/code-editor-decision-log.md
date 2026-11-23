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
(8 ms) budgets. The run is recorded in
[M2 typing budgets recorded](#m2-typing-budgets-recorded-ce21-2026-11-22).

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
- **Shell regressions outside the code editor suite.** The Search tool window
  joined the bottom tab group but not the diagnostics group that starts closed,
  so it kept the bottom area open and shrank the World viewport below the
  layout tests' limits; it now starts closed and collapses with that group and
  is listed in the Window menu. The find overlay's margin now comes from the
  spacing tokens. The profiling shell test still pressed F9, which the keymap
  decision moved to Ctrl+Alt+F9.

Kiro's sandbox cannot compile, so the "authored, not run" label covered
compiling as well as running. `AGENTS.md` and `.kiro/steering` now require a
warnings-as-errors build before a pull request, or a draft titled `[unbuilt]`.

### M2 typing budgets recorded (CE21, 2026-11-22)

The batch 7 workloads were first run on an Apple M5 (10 cores, 16 GiB), macOS
26.6, Homebrew Clang 23.1.3 and Qt 6.12.0, with preset `macos-clang-development`
(RelWithDebInfo), `LUDUS_WARNINGS_AS_ERRORS=ON` and `QT_QPA_PLATFORM=offscreen`
([issue 1261](https://github.com/The-Ludus-Project/Ludus/issues/1261)).
Concurrent agent builds kept the load average high, so this is not the quiet
reference machine that M9 qualifies with the native probe.

```bash
./init.sh --cli --with-tests --with-editor --preset-only --skip-checks --skip-sanitizers --skip-sdk macos-clang-development
cmake --preset macos-clang-development -DLUDUS_WARNINGS_AS_ERRORS=ON
cmake --build out/build/macos-clang-development --target ludus_code_editor_tests
QT_QPA_PLATFORM=offscreen out/build/macos-clang-development/apps/editor/ludus_code_editor_tests \
  "corpus: 1-cursor typing through the edit-set path","corpus: 100-cursor typing through the edit-set path" -r compact
```

At a load average of 16-21 the 100-cursor case missed its budget: handling p95
was 17-55 ms and key-to-paint p95 16-52 ms. A `sample` profile found two costs
that grow with the distance between the cursors, not with the edit:

- **Span replacement.** After the host applied the edit set,
  `CodeDocumentController::Synchronize` diffed the view against the host text
  by common prefix and suffix and replaced everything between them in one
  `insertText`. With carets spread over 400 lines, every keystroke deleted and
  recreated every line between the first and last caret, with a layout per
  recreated line. `ApplyMultiCursorEdit` now applies each accepted region to
  the view in place, last region first, in one Qt edit block, so each line
  keeps its `QTextBlock`, layout cache, syntax state and fold data. The
  following `Synchronize` still compares the view with the host text and
  repairs any difference.
- **Off-screen carets.** `CodeMultiCursor::Paint` located every secondary
  caret with `cursorRect`, which lays out the caret's line, even outside the
  viewport. Paint now receives the visible position range and skips carets
  outside it.

Two regression tests cover these: `Multi-cursor typing edits each line in place`
keeps a marked line between two carets, and `Painting secondary carets lays out
only visible lines` repaints synchronously and checks that an off-screen caret's
line stays unlaid out. Each fails without its fix.

Results with both fixes, alternating the builds before and after the change in
one session (load average about 9). Each run has 100 samples with a nearest-rank
p95; figures are the range over four runs, in microseconds:

| Workload | Build | Handling p50 | Handling p95 | Key-to-paint p50 | Key-to-paint p95 | Budget | Result |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 cursor (72 chars, 5 lines) | Before | 63-70 | 74-81 | 51-55 | 60-67 | ≤ 4 ms handling p95; ≤ 16 ms key-to-paint p95 | Within |
| 1 cursor | After | 63-70 | 71-108 | 50-55 | 57-87 | ≤ 4 ms handling p95; ≤ 16 ms key-to-paint p95 | Within |
| 100 cursors (7,200 chars, 401 lines) | Before | 2,219-2,246 | 2,306-3,627 | 2,132-2,150 | 2,214-3,495 | ≤ 8 ms handling p95; ≤ 16 ms key-to-paint p95 | Within |
| 100 cursors | After | 861-929 | 936-1,001 | 776-839 | 843-897 | ≤ 8 ms handling p95; ≤ 16 ms key-to-paint p95 | Within |

The earlier failure was mostly load: at a load average of about 9 the build
before the change met its budget too. The change still makes the 100-cursor
keystroke about 2.4 times cheaper. At a load average of about 7, alternating
runs gave a 100-cursor handling p95 of 7.4-11.7 ms before the change and
3.4-5.0 ms after it, so it also restores headroom on a busy machine. Both M2
typing budgets are now recorded and met offscreen. The native keypress-to-paint
qualification on quiet reference machines remains M9 work.

## M4 - Generic language client

### M4 batch 1: the generic client move (CE09, 2026-11-22)

`SlangLanguageService` is now `LanguageClient`
(`apps/editor/src/internal/language_client.h`), and its Slang specifics moved
into a `LanguageServerProfile` (`internal/language_profiles.h`), as the first
M4 task requires ([issue 1263](https://github.com/The-Ludus-Project/Ludus/issues/1263)).
This is a structural move with no behavior change:

- The client keeps the framing, bounds, off-thread decoding, deadline,
  incremental synchronization and stale-generation rules unchanged.
- The profile carries what was hard-coded for Slang: the `languageId`, the
  capabilities that initialization requires (completion and signature help),
  the `workspace/configuration` answers, and the vocabulary of the failure
  messages. The Slang profile reproduces every earlier message exactly.
- A profile never names an executable or adds arguments. Program resolution
  stays with the context provider (`RenderGraphShaderAssistance`), so a project
  file still cannot replace a server program.
- LUSL sources keep using the Slang profile, as they did before. `luslc lsp`
  advertises no signature help, so it fails the required-capability check; the
  capability dispatch in the next batch makes signature help optional and adds
  a LUSL profile.

`language_client_tests.cpp` pins the Slang profile's configuration answers and
the exact missing-server and unsupported-server messages, and drives a
completion through the client against `slang_server_fixture.py`. The existing
assistance and navigation cases run unchanged.

### M4 batch 2: capability dispatch and the request table (CE09, CE20, 2026-11-22)

The second M4 task adds the client's protocol machinery
([issue 1263](https://github.com/The-Ludus-Project/Ludus/issues/1263)). Behavior
changes, each with a test against the new scripted fixture
`apps/editor/tests/language_server_fixture.py`:

- **Request table** (`internal/language_requests.h`). One row per request
  kind: method, the server capability (and option, such as
  `completionProvider.resolveProvider`) that enables it, its priority and
  whether the client adds `textDocument`. Later batches add features by using
  their rows; no feature names a method string.
- **Capability dispatch.** The profile no longer lists required capabilities.
  A kind the ready server does not advertise is never sent: `CanRequest` and
  `GetUnavailableReason` disable its command with the reason in its tooltip
  (completion and signature help in the view; Go to Symbol, Go to Definition
  and Find References in navigation). Typing triggers completion and signature
  help only from the server's `triggerCharacters` (and signature
  `retriggerCharacters`); the earlier `.`, `(` and `,` remain the defaults until
  a server reports its options. This fixes LUSL assistance: `luslc lsp`
  advertises no signature help, which the batch 1 Slang requirements rejected.
  LUSL sources now use their own `GetLuslServerProfile`.
- **Cancellation and priority.** At most one request per kind is in flight; a
  newer one sends `$/cancelRequest` for the older, whose late reply is
  discarded. Each sent request has its own ten-second deadline. Interactive
  requests flush pending text and go at once; background requests (document
  symbols, semantic tokens, inlay hints, folding ranges) wait for the 30 ms
  synchronization debounce, then go before workspace symbols.
- **Synchronization.** `didChange` is coalesced to the debounce, sent at once
  before an interactive request, and uses full text when the server asks for
  `TextDocumentSyncKind.Full`. A server with no synchronization is rejected.
- **Positions.** The client offers `utf-8` then `utf-16` and accepts either;
  `internal/language_positions.h` maps UTF-16 editor offsets both ways, and
  completion edits, navigation results and `didChange` ranges use the
  negotiated encoding. The pinned `slangd` and `luslc` choose UTF-16.
- **Errors and crashes.** An error reply is `RequestFailed`: the status names
  the request's repair and the server keeps running (it used to be stopped and
  restarted on the next request). An unexpected exit restarts the same server
  after 1 s, then 2 s, with a status saying editing is unaffected and the
  pending intent re-sent when it is still current. A third exit within 60 s
  stops it with Retry assistance as the fix. The retained standard error
  (bounded by the existing 8 KiB budget) is available as `GetServerLog` for the
  status-bar Show Log action, which lands with the status-bar language-server
  item in the diagnostics batch.

The fixture replaces neither `slang_server_fixture.py` nor the `[slang-native]`
case: they keep covering the shader context. The remaining fixture failure paths
(oversize messages, stale replies across documents, workspace-edit rejections)
belong to the batches that add those features.

### M4 batch 3a: hover and Analysis diagnostics (CE10, CE20, 2026-11-23)

The first part of the third M4 task
([issue 1263](https://github.com/The-Ludus-Project/Ludus/issues/1263)):

- **Analysis diagnostics.** The client emits `textDocument/publishDiagnostics`
  (bounded to 1,024 per report) and advertises version support. Once a context
  is prepared, `CodeAssistance` re-synchronizes the draft 100 ms after an edit.
  A report is applied only when it names the context's document, carries the
  version the server last received (or none), and that text is still the
  current draft; otherwise the next report replaces it. Positions use the
  negotiated encoding, and ranges in the generated prefix or suffix are
  dropped. Hints are dropped; information shows as a warning.
- **Separate layer.** `CodeEditor::SetAnalysisDiagnostics` keeps its own
  markers, drawn with a dotted underline beneath the build's wave squiggles
  and described as `Analysis (<server>) error/warning`. Build markers, F8 and
  the gutter keep their current meaning. The Error List's Analysis and Build
  groups follow in a later batch.
- **Hover.** After 300 ms of pointer rest (VS Code's default), or **Show
  Hover** in the context menu, the editor asks for a hover. Hover never
  prepares a context, so a resting pointer cannot start tools: before a context
  exists it shows local diagnostics only. A reply applies only to the document
  revision and context it was asked for. Contents are shown as plain text, with
  code fences and Slang's generated `Defined in` provenance removed, followed by
  that position's build and analysis messages. Show Hover has no default
  shortcut: VS Code's Ctrl+K Ctrl+I needs the Ctrl+K prefix that the Ludus
  profile keeps for the command palette.
- **Show Log** in the Source pane's shader-assistance row shows the server's
  retained standard error (CE20).

Tests drive `language_server_fixture.py`, which can now publish a diagnostic
for every occurrence of a word, optionally with a stale version. The context
prefix contains the word too, so generated text must not produce a marker.

### M4 batch 3b: Analysis rows and the remaining Go to commands (CE10, 2026-11-23)

- **Error List.** A new `ErrorListProducer::Analysis` holds one row per analysis
  marker in every open package-shader Code tab (since
  [PR 2208](https://github.com/The-Ludus-Project/Ludus/pull/2208) shaders open
  there; the Source pane serves when a workspace has no Code area), with the
  origin `Analysis (<server>)`, the package file and the marker's current line
  and column. A closed tab's rows leave with it. The producer replaces only its own
  rows like the others, but never emits `ProblemsReported`: analysis changes as
  the author types, and raising the list on each report would take focus from
  the text. Activating a row brings that file's tab forward and selects the
  marker's range, expanding folds, or reports that the row was replaced. The architecture's "Analysis" and "Build"
  groups are these origins; the list is not split into two tables.
- **Go to commands.** Go to Declaration, Go to Type Definition and Go to
  Implementation (Ctrl+F12, VS Code's binding) use the request table's rows and
  the same result reader as Go to Definition, jumping straight to a single
  location. Declaration and Type Definition have no default binding, as in VS
  Code. Each is disabled with its reason when the server lacks the provider.
- **Go to Symbol in Workspace** (Ctrl+T) sends `workspace/symbol` with the word
  at the caret and lists the results in authored sources of the captured
  context; generated text is excluded, as for Go to Symbol. Re-querying as the
  filter changes is not implemented: the chooser filters the returned list.

The references tool window moves to the next batch with semantic tokens: it
needs its own dock, and the existing chooser keeps serving Find References.

### M4 batch 3c: semantic tokens and server folding ranges (CE10, CE26, 2026-11-23)

Two background layers, owned by a new `CodeSemanticLayers`
(`apps/editor/src/internal/code_semantic_layers.h`) rather than added to
`CodeAssistance`, which only forwards replies and says when to refresh:

- **Requests.** After each analysis synchronization (100 ms after an edit), and
  when a context is set or the server becomes ready, the layers ask for
  `textDocument/semanticTokens/full` and `textDocument/foldingRange` when the
  ready server advertises them. They never ask for an unadvertised kind, which
  would report the command unavailable. A reply applies only to the document
  revision and context it was asked for; the request table's per-kind
  cancellation drops older replies. The client now advertises every standard
  token type and modifier, the `relative` format without overlapping or
  multi-line tokens, and line-only folding ranges with `collapsedText`.
- **Semantic tokens** are decoded through the server's legend into the same
  `CodeSyntaxToken` records as tree-sitter, mapped to the design-token syntax
  roles by one table (`ReadSemanticTokenRole`; a `readonly` variable is a
  constant), and drawn by `CodeSyntaxHighlighter` over the syntax tokens. Tokens
  in the generated prefix or suffix, spanning lines, overlapping an earlier
  token or of a type with no role are dropped; a malformed entry ends the
  decode, since later tokens are relative to it. At most 131,072 tokens are
  kept. Between replies, edits move later tokens and drop the ones they touch,
  so colors do not flicker while typing. The table lives beside the decoder,
  not in the language registry, because the capture-to-role mapping it mirrors
  is in the syntax service.
- **Folding ranges** follow CE26: a server range is offered only for a kind the
  language's grammar does not produce (a grammar produces Syntax, Comment and
  Region ranges; without one, indentation), and never where a syntax range
  already starts. With the current grammars that admits `imports`, a new
  `CodeFoldKind` appended so saved fold state keeps its values; a range with no
  or an unknown kind is a Syntax range. Ranges are anchored to text cursors, so
  a collapsed server range moves with edits and stays collapsed through the
  next syntax result. At most 4,096 are kept.

The pinned `slangd` advertises semantic tokens (`type`, `enumMember`,
`variable`, `parameter`, `function`, `property`, `namespace`, `keyword`, `macro`
and `string`) and inlay hints, but no folding ranges; `luslc lsp` advertises
neither layer. The folding path is therefore exercised by
`language_server_fixture.py`, which can now answer semantic tokens for every
occurrence of a word (`--tokens`) and return given folding ranges
(`--folding-ranges`). Inlay hints and the references tool window follow in the
next batch.

### M4 batch 3d: inlay hints (CE10, 2026-11-23)

- **Requests.** `CodeSemanticLayers` adds `textDocument/inlayHint` to its
  refresh, for the whole authored source rather than the visible lines: hints
  are few, and edits move them without a request when the view scrolls. The
  client advertises `inlayHint` without resolve. The Slang profile now answers
  `slang.inlayHints.deducedTypes` and `slang.inlayHints.parameterNames` with
  `true`; the pinned `slangd` sends no hints otherwise. Whether hints are drawn
  is the view's setting, so changing it never restarts or reconfigures the server.
- **Refresh.** `slangd` answers the first request with no hints, reads its
  configuration, then sends `workspace/inlayHint/refresh`. The client now
  advertises `refreshSupport` for inlay hints and semantic tokens, answers both
  refresh requests and emits `RefreshRequested`; the layers ask again after the
  same 100 ms pause as an edit. Before this, the shader editor never showed a
  hint from the real server.
- **Decoding.** A label is a string or its joined parts, shortened with an
  ellipsis past 64 UTF-16 units. Hints outside the authored source, with an
  empty label or spanning lines are dropped; at most 4,096 are kept, ordered by
  position with Foundation's `StableSort`.
- **Drawing.** The architecture requires inline text that never enters the
  projection. `QPlainTextEdit` has no inline objects, so the highlighter reserves
  each hint's measured width as absolute letter spacing on the character before
  it, and the view paints the label into that gap. The document, its revision,
  the clipboard and the history are untouched, and the caret, selection and hit
  testing stay on real characters. A consequence is that a hint at a line start,
  with no character before it, is not shown; `slangd`'s hints follow a name or an
  opening parenthesis, so none is lost in practice. Edits move later hints and
  drop the ones at the edit until the next reply.
- **Setting.** **Show Inlay Hints** is a checkable editor command, persisted as
  `codeEditor/inlayHints` (default on). Holding Ctrl+Alt shows hidden hints until
  either key is released or the editor loses focus, as the architecture asks.

The references tool window follows in its own batch.

### M4 batch 3e: the References tool window (CE10, 2026-11-23)

- **Routing.** `CodeNavigationViewHost` gains `ShowReferences`, which returns
  false by default. `CodeAssistance` hands Find References results, with the
  symbol at the caret, to `CodeNavigation::ShowReferences`; a host that has a
  References window takes them, and any other host (plain Code tabs, tests)
  keeps the chooser beside the editor. `RendererWorkspace` takes them for
  package shaders once the main window attaches its window.
- **Window.** `CodeReferencesWindow` (`internal/code_references_window.h`) is a
  bottom tool window tabbed with the Error List and Search, closed until a
  result arrives, and listed in **View** and **Window**. It groups rows by file
  (the current source, other package files, and read-only shader context) and
  shows each use's line, column and line text from the captured snapshot.
- **Staleness.** Rows hold copied targets, not anchors, so they stay correct
  only while the finding editor's text is unchanged. The window records that
  document's revision: after an edit, activating a row reports that the list
  describes earlier text and offers **Find Again** rather than jumping to a
  shifted position. Find Again restores the request caret, which is anchored in
  the document and so moved with the edits, and runs Find References there. A
  closed editor disables Find Again and says how to start over. Activating a
  row first brings the finding editor's tab forward through its host, so a row
  works after the author has switched tabs.

### M4 batch 3f: completion resolve and snippets (CE10, 2026-11-23)

- **Resolve.** When the server advertises `completionProvider.resolveProvider`,
  highlighting a suggestion sends `completionItem/resolve` with the item as the
  server sent it, once per suggestion. The reply's fields are merged into the
  item, so its documentation, detail and additional edits apply, while the edit
  range taken from the list defaults stays unless the reply replaces it. A reply
  applies only while the same request is current. The client now advertises
  `resolveSupport` for `documentation`, `detail` and `additionalTextEdits`.
- **Documentation.** The completion list shows the highlighted suggestion's
  documentation in a panel beside it (plain text; the earlier tooltip remains),
  on the side with room.
- **Snippets.** The client advertises `snippetSupport`. `ParseCodeSnippet`
  (`internal/code_snippet.h`) expands the LSP 3.17 grammar: tab stops, nested
  placeholders, choices (the first is inserted), variables and escapes. A
  construct that does not parse is literal text, as in VS Code. A transform is
  refused with a message rather than inserted wrongly, as is a snippet beyond
  128 stops or 16 nesting levels. A bare `$N` after `${N:text}` repeats that
  text. The editor resolves no variables: the `TM_` ones are empty and keep
  their default, and others insert their name, as the protocol asks.
- **Tab stops.** The expanded text is applied as one undoable edit like any
  completion, then `CodeSnippetSession` selects the first stop. Tab and
  Shift+Tab move between stops; a stop's other occurrences become secondary
  cursors, so the existing multi-cursor editing keeps mirrors in step. Stops are
  anchored in the document and drawn with a faint background. `$0`, or the end
  of the snippet without one, ends the session, as do Escape and moving the
  caret outside the inserted text. Choices are inserted without a picker.

Snippets as a separate completion source (the architecture's merged snippet
list), rename, code actions and workspace edits follow.

### M4 batch 4a: rename and workspace edits (CE10, CE11, 2026-11-23)

The first part of the fourth M4 task
([issue 1263](https://github.com/The-Ludus-Project/Ludus/issues/1263)). Code
actions with the light bulb follow, and reuse these workspace edits.

- **Rename Symbol** (F2, VS Code's binding) prepares the shader context, asks
  `textDocument/prepareRename` when the server offers it (otherwise the word at
  the caret is renamed), and opens an inline box over the symbol, the one
  overlay besides Find that takes focus. Enter sends `textDocument/rename`;
  Shift+Enter, or a rename that reaches other files, shows a preview dialog
  listing every file and change with Apply and Cancel. A rename applies only to
  the draft revision and context it was asked for. The command is disabled with
  its reason for a server without `renameProvider`.
- **Workspace edits.** `ReadCodeWorkspaceEdit` (`internal/code_workspace_edit.h`)
  reads `changes` and `documentChanges` and validates the whole edit before
  anything applies, as the design asks. It refuses: edits in the generated
  prefix or suffix; read-only captured sources; URIs outside the context's
  inventory; other files when the host cannot edit them; a versioned edit for an
  older document; resource operations (create, rename, delete); invalid or
  overlapping ranges; and more than 256 files (`CODE_MAX_WORKSPACE_EDIT_FILES`)
  or 65,536 edits. Each refusal names its repair and says nothing changed.
- **One transaction.** Shader sources live in the render-graph package, whose
  history already snapshots every package file. `RenderGraphShaderAssistance`
  implements `CodeWorkspaceEditHost`: it checks that every change's text is
  still its file's draft, rebuilds each file's bytes (keeping CRLF endings), and
  commits them through `RenderGraphDocument::ReplaceTexts` as a single undo step.
  This is stronger than CE11's "Undo offers to undo in all files": one Undo in
  any of the files reverts them all, because they share one history. The files
  stay unsaved drafts until Save, and every open view refreshes as after Undo.
  Without a host (a plain buffer), a rename is limited to the edited source and
  applies through its controller as one edit.

Neither pinned server advertises `renameProvider` (nor `codeActionProvider`),
so the path is exercised by `language_server_fixture.py`'s new `--rename` and
`--stale-rename` options and by a package test that applies a two-file edit.

### M4 batch 4b: code actions and the light bulb (CE10, CE11, 2026-11-23)

- **Requests.** A new `CodeActions` component (`internal/code_actions.h`), kept
  out of `CodeAssistance`, asks `textDocument/codeAction` 250 ms after the caret
  rests in a prepared context, and at once for **Show Code Actions** (Ctrl+.,
  VS Code's binding). The range is the selection or caret; the context carries
  the analysis diagnostics there exactly as the server published them, while
  they still describe the draft, and the trigger kind (automatic or invoked).
  An automatic request never replaces a pending invoked one. Replies apply only
  to the draft they were asked for, and an edit clears the light bulb until the
  next reply.
- **Light bulb and list.** While actions exist on the caret's line, the gutter's
  marker column shows a light bulb, a shape distinct from the diagnostic dot it
  can share the column with; clicking it or Ctrl+. opens the list. Preferred
  actions come first, in bold. Disabled actions (LSP `disabled`) and actions
  that only run a server command are listed disabled with their reason: the
  client does not implement `workspace/executeCommand` or the server-initiated
  `workspace/applyEdit` it implies. At most 64 actions are kept.
- **Apply.** An action's `edit` goes through `ReadCodeWorkspaceEdit` and the new
  shared `ApplyCodeWorkspaceChanges`, which rename now uses too, so it is
  validated whole and applied as one package undo step. An action without an
  edit is resolved with `codeAction/resolve` (a new request-table row) when the
  server advertises `resolveProvider`. The client now advertises code action
  literals with `isPreferred`, `disabled`, `data` and edit resolution, workspace
  edit `documentChanges` with no resource operations and abort-on-failure, and
  rename's `prepareSupport`, which batch 4a omitted.

Not done: the hover's Quick Fix link, and running server commands. With this
batch the fourth M4 task is complete; the fixture failure paths and the M4 gate
remain.

