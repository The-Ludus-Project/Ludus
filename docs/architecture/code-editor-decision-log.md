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
