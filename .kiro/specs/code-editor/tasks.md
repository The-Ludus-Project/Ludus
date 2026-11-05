# Code editor implementation milestones

All boxes start unchecked: nothing in this handoff has been implemented or
validated. Use [requirements.md](requirements.md) and [design.md](design.md) as
the contract and the
[architecture](../../../docs/architecture/code-editor.md) for rationale and
budgets. After each batch, update this file with the changed paths, commands,
results and remaining gates. Preserve working code and unrelated specs. Each
milestone ends with the [standard gates](#standard-gates).

## M0 - Revalidate and prepare

Requirements: CE21, CE25. Prerequisite: none.

- [x] Read `AGENTS.md`, `.kiro/steering/`, ADRs 0003-0005 and 0007, the
  architecture, and the existing editor files listed in design.md section 1.
- [x] Compare the checkout with the architecture's baseline table. Record the
  actual revision and any newer editor work in
  `docs/architecture/code-editor-decision-log.md`. Reuse compatible code; do not
  overwrite another implementation.
- [x] Add the keypress-to-paint latency probe through the existing editor
  profiling scopes, and the `[.benchmark]` corpus (real shaders, 100,000-line
  C++, 8 MiB JSON, a 1 MiB single line, typing during diagnostics, 100 cursors).
  Record baseline numbers for the current editor. *(Probe and corpus authored in
  the sandbox; baseline recorded on an Apple M5 on 2026-11-03, see
  [M0 baseline](#m0-baseline-2026-11-03).)*
- [x] Write the tree-sitter dependency ADR under `docs/decisions/`: library and
  grammar revisions, licenses, recipes, the Wasm build, warning handling and
  update policy. Verify each grammar's license and query set at the pinned
  revision. *(ADR 0026, opened as its own pull request; licenses verified MIT,
  exact tags/hashes recorded in M3's recipe change per the ADR.)*
- [x] Decide the owner of the sequence diff (recommended: FoundationContainers)
  and of the code-buffer recovery journal. *(Both recorded in the decision log.)*
- [x] Add the language registry skeleton with Slang and plain text, replacing
  the `CodeLanguage` switch statements without changing behavior.
- [x] Copy the decided shortcut resolution (architecture, "Settings, keymaps
  and EditorConfig", decided 2026-11-03) into the decision log, and list the
  existing commands and tests it affects.

Gate: the baseline benchmark results are recorded; the ADR is ready for review;
behavior is unchanged.

### M0 status (branch `codex/code-editor-m0`; ADR on `codex/code-editor-m0-tree-sitter-adr`)

Changed paths (M0 core branch):

- `docs/architecture/code-editor-decision-log.md` (new): baseline comparison,
  sandbox-toolchain deviation, sequence-diff and recovery-journal owners,
  shortcut resolution with affected commands/tests, registry-skeleton note.
- `apps/editor/src/internal/code_language_registry.h`,
  `apps/editor/src/code_language_registry.cpp` (new): Qt-free language registry
  skeleton (PlainText, Slang) replacing the hard-coded file-classification
  switch; `apps/editor/src/renderer_workspace.cpp` now classifies through it.
- `apps/editor/src/internal/code_latency_probe.h`,
  `apps/editor/src/code_latency_probe.cpp` (new): keypress-to-paint probe, idle
  until started; wired into `CodeEditor::keyPressEvent`/`paintEvent` with
  `LUDUS_PROFILE_SCOPE` zones (`CodeEditorKeyPress`, `CodeEditorPaint`).
- `apps/editor/tests/code_language_registry_tests.cpp`,
  `apps/editor/tests/code_latency_probe_tests.cpp` (new, `[code-editor]`):
  registry and probe regressions.
- `apps/editor/tests/code_editor_benchmark.cpp` (new, `[.benchmark]`): the six
  corpus workloads against the current editor.
- `apps/editor/CMakeLists.txt`: registers the new sources and tests; links
  `Ludus::FoundationProfiling` into `ludus_editor_code`.
- `docs/README.md`: indexes the decision log (ADR index line is on the ADR
  branch).

Changed paths (ADR branch): `docs/decisions/0026-tree-sitter-syntax-dependency.md`
(new), `docs/README.md` (ADR index line).

Gate status — **the standard gates and the baseline measurement could not be
run in this sandbox** (Amazon Linux 2023; system Clang 15 only, CMake 3.22 which
cannot parse the pinned `CMakePresets.json`, no Qt 6, no Conan, no LLVM 23). See
the decision log's "Sandbox toolchain cannot run the gates locally" entry. All
of `./scripts/build`, `./scripts/test`, `./scripts/check --all`, the sanitizer
lanes, `./scripts/check-build-budget`, `scripts/check-docs` and
`scripts/check-wiki` are **deferred to batch CI on the pinned toolchain**.
Local verification was limited to close reading against the C++23, naming,
standard-library (ADR 0003) and Qt-free-seam rules. The `[.benchmark]` corpus
records numbers only when built and run on a reference machine; those numbers
are **not yet recorded** and remain outstanding for M0's performance intent
(CE21), to be captured on a toolchain-capable host. Behavior is unchanged: the
registry reproduces the existing `.slang`-to-Slang mapping (with the documented
`.slangh` micro-deviation), and the probe is inert unless explicitly started.

### M0 baseline (2026-11-03)

Measured against `origin/develop` `a7239cb9` (before any M1 change) on branch
`codex/code-editor-m0-baseline`. This was also the first build of the M0 code.
Host: Apple M5 (10 cores, 16 GiB), macOS 26.6, Homebrew Clang 23.1.3,
Homebrew Qt 6.12.0, preset `macos-clang-development` (RelWithDebInfo),
`QT_QPA_PLATFORM=offscreen`. Concurrent agent builds kept the load average at
3.4-4.7, so this is not a quiet reference machine. The M9 qualification still
uses the native probe on quiet reference machines.

Commands:

```bash
./init.sh --cli --with-tests --with-editor --preset-only --skip-checks --skip-sanitizers --skip-sdk macos-clang-development
cmake --build --preset macos-clang-development --target ludus_code_editor_tests
QT_QPA_PLATFORM=offscreen out/build/macos-clang-development/apps/editor/ludus_code_editor_tests
QT_QPA_PLATFORM=offscreen out/build/macos-clang-development/apps/editor/ludus_code_editor_tests "[.benchmark][code-editor]" -r compact
```

Results:

- The M0 code compiled warning-clean.
- `ludus_code_editor_tests`: 57 test cases passed, 2 skipped. The skipped cases
  need `LUDUS_TEST_SLANGD`, which was not set.
- The `[.benchmark][code-editor]` corpus ran three times.

Each benchmark sample is one `x` key event sent through `keyPressEvent`, then
an undo, with 100 samples per workload. The p95 is nearest-rank. The table gives
the range of each figure across the three runs, in microseconds unless stated.
"Handling" covers key press and release, the document change, relayout and the
offscreen paint. "Key-to-paint" is the view's `CodeLatencyProbe`, measured from
the key's arrival to the paint that shows the edit.

| Workload | Handling p50 | Handling p95 | Key-to-paint p50 | Key-to-paint p95 | Budget check |
| --- | --- | --- | --- | --- | --- |
| Slang shader, 1,001 lines, lexical highlighter | 185-249 | 203-299 | 52-70 | 62-92 | Within 4 ms |
| 100,000-line C++ as plain text (6.7 M chars) | 1,302-1,554 | 1,359-1,671 | 807-1,001 | 838-1,077 | Within 4 ms |
| 8 MiB JSON as plain text (134,190 lines) | 133-154 | 148-175 | 117-135 | 129-153 | Within 4 ms |
| Single 1 MiB line | 917-934 ms | 959-1,111 ms | 657-673 ms | 693-794 ms | Fails by about 250x |
| Slang, 2,001 lines, 200 build markers | 1,428-1,597 | 1,484-1,977 | 1,015-1,129 | 1,055-1,366 | Within 4 ms |
| 100 inserts in one edit block (pre-M2) | 793-901 | 855-2,475 | n/a | n/a | Within the 8 ms 100-cursor budget |

Observations for later milestones:

- The single long line is the known Qt whole-block layout cost. CE22's
  20,000-unit long-line threshold (M1 file host, M3 formats) has to keep such
  files out of this path. This measurement is the evidence the escape hatch in
  the architecture's alternatives would be judged against.
- 200 static build markers cost about 7x the plain shader per keystroke. Live
  analysis diagnostics (M4) add more markers, so M4 should re-measure this case.
- The 100,000-line plain-text file costs about 10x the 8 MiB JSON file, which
  has more lines and more characters. Neither has a highlighter, so the cause is
  unexplained. M3 should profile it before attributing any change to
  tree-sitter.
- The 100-position case showed one run with a 6 ms maximum under load. This is
  the pre-M2 Qt path, not the edit-set path.

## M1 - Every text file is a document

Requirements: CE01-CE04, CE20, CE22, CE23. Prerequisite: M0.

- [x] Implement the file document host (design section 3), including
  thresholds, read-only invalid UTF-8, atomic save, external-change conflicts
  and EditorConfig.
- [x] Implement the code document registry, preview tabs, two editor groups,
  view mirroring and restoring tabs per project.
- [x] Route Project Explorer, Quick Open, Error List and navigation to code
  tabs. Files owned by a render-graph package open its Source pane. Keep Open
  Externally.
- [x] Move the Scripts work area's text onto `CodeEditor` through a script host,
  keeping S5 draft, Save, reload and source-stop behavior.
- [x] Add code-buffer recovery and the dirty-close prompts.
- [x] Add the status bar items: position, selection count, indentation,
  encoding, line endings, language and tool state.
- [x] Update the user guide: create `docs/development/code-editor.md`, register
  it, and update `editor-workspace.md`.

Gate: every listed file type opens, edits, saves, conflicts and recovers in
offscreen tests and a recorded native run; existing render-graph and Scripts
tests pass. See the [M1 gate report](../../../docs/architecture/code-editor-decision-log.md#m1-gate-report-2026-11-04);
every native item is **pending host verification** because the PR CI skips the
editor build and tests and this sandbox has no Qt6/pinned toolchain.

## M2 - Editing parity

Requirements: CE05-CE07, CE17. Prerequisite: M1.

- [ ] Extend the seam with `CodeEditSet` and the selection set (design
  section 2). Move formatting and Replace All onto it.
- [x] Implement multiple cursors, column selection, add next and all
  occurrences, and cursors above and below (design section 5). *(Batch 2;
  `internal/code_multi_cursor.h/.cpp`, `CodeDocumentController::ApplyMultiCursorEdit`,
  `AdvanceCodeCursorPosition`; one `CodeEditSet` per operation mapped in one pass;
  authored, not run — see the
  [M2 batch 2 decision-log entry](../../../docs/architecture/code-editor-decision-log.md#m2-batch-2-multiple-cursors-ce06-2026-11-05).)*
- [x] Replace the bottom find panel with the find overlay: regex, whole word,
  in selection, match count and preserve-case replace. *(Batch 3;
  `internal/code_find_overlay.h/.cpp` replaces the panel's find rows, one regex
  engine for find/count/replace, Replace All as one linear `CodeEditSet`;
  authored, not run — see the
  [M2 batch 3 decision-log entry](../../../docs/architecture/code-editor-decision-log.md#m2-batch-3-the-find-overlay-ce07-2026-11-05).)*
- [x] Implement the search service, the Search tool window and Replace in
  Files with preview (design section 8). *(Batch 4; `internal/code_search_service.h/.cpp`
  (worker + one-pass `FindMatchesInText`), `internal/search_tool_window.h/.cpp`,
  `CodeDocumentRegistry::ApplyWorkspaceEdit` minting one `CodeTransactionId`;
  authored, not run — see the
  [M2 batch 4 decision-log entry](../../../docs/architecture/code-editor-decision-log.md#m2-batch-4-search-and-replace-in-files-ce07-ce11-2026-11-05).)*
- [ ] Add auto-closing and surrounding pairs and indentation rules from the
  registry.
- [ ] Add the Ludus and VS Code keymap profiles, user overrides, conflict
  detection and the first-use choice. Apply the decided bindings: Record
  Profiling to Ctrl+Alt+F9, F9 Toggle Breakpoint in code editors, Ctrl+K palette
  in the Ludus profile, and Ctrl+Shift+P palette with Ctrl+K chords in the VS
  Code profile. Update the documents that name F9 for profiling
  (`docs/development/editor-workspace.md`, `docs/architecture/graphics-debugging.md`)
  in the same change. Reserve the folding bindings from the architecture in
  both profiles, so the collision tests cover them before M3 adds the commands.
  Add shortcut-collision tests for both profiles.
- [ ] Measure 1-cursor and 100-cursor typing against the budgets.

Gate: the CE05-CE07 and CE17 tests pass; the typing budgets are recorded.

## M3 - Syntax service

Requirements: CE08, CE19 (syntax roles), CE22, CE26. Prerequisite: M0 ADR accepted, M1, M2 (folding bindings).

- [ ] Add the tree-sitter recipes and the private C target; build them on every
  supported native and Wasm configuration.
- [ ] Implement the worker, snapshot posting at the debounce, token packing,
  delta shifting and the revision checks (design section 6).
- [ ] Turn `CodeSyntaxHighlighter` into the token applier with the lexical
  fallback and sliced reapplication.
- [ ] Add queries and role mappings for Slang, Luau, C, C++, JSON, CMake and
  Markdown. Add the syntax roles and contrast pairs to the design tokens.
- [ ] Add folding (design section 6, "Folding"): syntax, region-marker,
  indentation and manual ranges; placeholders and gutter controls; caret
  skipping and `RevealRange` at every reveal site; selections, the clipboard
  and line commands over hidden lines; reconciliation after edits; per-view
  state restored with the tab; the fold commands with their bindings in both
  profiles; announcements. Add the randomized invariant tests and measure Fold
  All and Unfold All on the 100,000-line corpus.
- [ ] Add outline, breadcrumbs, sticky scroll and expand/shrink selection.
- [ ] Measure open, scroll and reparse budgets, and the large-file behavior.

Gate: highlighting is correct on the corpus; a stale result never lands at a
wrong offset under randomized edit tests; the CE26 tests pass and no
randomized edit leaves a hidden line without a visible control; the budgets
are recorded.

## M4 - Generic language client

Requirements: CE09-CE11, CE20. Prerequisite: M2, M3.

- [ ] Move `SlangLanguageService` into `LanguageClient` with no behavior change;
  all existing slangd tests must pass unchanged.
- [ ] Add capability dispatch, the request table, priorities, cancellation,
  incremental synchronization, position-encoding negotiation and the restart
  policy.
- [ ] Add hover, Analysis diagnostics, semantic tokens, inlay hints, folding
  ranges (merged by kind per CE26), document and workspace symbols, the remaining Go to commands, references window,
  completion resolve and snippets.
- [ ] Add rename with preview, code actions with the light bulb, and workspace
  edits with transaction undo.
- [ ] Add the fake LSP fixture and its failure-path tests.

Gate: the CE09-CE11 tests pass against the fake server, and the `[slangd]` tests
pass against the pinned server.

## M5 - Luau provider

Requirements: CE12. Prerequisite: M4.

- [ ] Pin `luau-lsp` and StyLua with per-platform hashes; install them from
  `init.sh` only when scripting is enabled; add their resolvers and
  Choose Executable.
- [ ] Implement the script context: the active package's `contract.d.luau` and
  target profile.
- [ ] Show the script cook's diagnostics as Build diagnostics in the editor.
- [ ] Add project-symbol completion and validation for asset and entity
  references.
- [ ] Add StyLua formatting through the whole-text diff path.

Gate: the `[luau-lsp]` tests pass; a recorded run edits, completes, formats and
cooks a behavior.

## M6 - C/C++ provider

Requirements: CE13. Prerequisite: M4. Can run in parallel with M5.

- [ ] Resolve clangd and clang-format from the pinned LLVM.
- [ ] Implement the native context: the active preset's compile database, and
  the Configure banner when it is missing or stale.
- [ ] Open engine SDK headers read-only from navigation.

Gate: the `[clangd]` tests pass; a recorded run navigates from a game module
into an SDK header.

## M7 - Engine context

Requirements: CE14, CE15, CE12 (references). Prerequisite: M4; M5 for Luau references.

- [ ] Add the shader context picker to the status bar.
- [ ] Add Live Preview with the existing render-graph build. Keep the last
  drawable view until Ready; add pending, failure and success regression tests.
- [ ] Add the target code view with two-way line mapping.
- [ ] Add color swatches with the workbench picker, and numeric scrubbing as
  one undo group.
- [ ] Add asset reference hover previews, Open, and Used By.

Gate: the CE14 and CE15 tests pass; a recorded run tweaks a shader with Live
Preview.

## M8 - Debugging

Requirements: CE16. Prerequisite: M5, M6.

- [ ] Implement the DAP client and the fake DAP fixture.
- [ ] Implement the Luau adapter over the GameHost debug capability.
- [ ] Add `lldb-dap` launch and attach from the project's play description.
- [ ] Add the breakpoint lane (conditions, hit counts, logpoints), the current
  statement, Call Stack, Variables, Watch, inline values and Run to Cursor.

Gate: the `[lldb-dap]` tests and the Luau adapter tests pass; a recorded run
stops at a Luau breakpoint and at a native breakpoint.

## M9 - Polish and qualification

Requirements: CE18, CE19, CE21, CE24, CE25. Prerequisite: M1-M8.

- [ ] Add Git change markers through the sequence diff, with the original-lines
  view and Revert Change.
- [ ] Finish the overview ruler and theme roles.
- [ ] Carry out the accessibility audit with VoiceOver, Narrator and Orca, and
  record the results.
- [ ] Check browser editor parity and the unavailable-capability reporting.
- [ ] Qualify every budget on the reference machines and record the results or
  accepted exceptions.
- [ ] Update the architecture status and baseline table, and the user guide.

Gate: CE01-CE26 each have evidence or an accepted, recorded exception.

## Standard gates

Run these at the end of every milestone, with the pinned toolchain, and report
anything unavailable, skipped or failed accurately:

```bash
./scripts/build   macos-clang-debug          # or the host's debug preset
./scripts/test    macos-clang-debug
./scripts/build   linux-clang-asan-ubsan     # where available; otherwise state it
./scripts/test    linux-clang-asan-ubsan
./scripts/check   macos-clang-development --all
python3 scripts/python/source_policy.py --base <merge-base> --clang-format <pinned clang-format>
./scripts/check-build-budget
python scripts/check-docs
python scripts/check-wiki --documentation-only
```

Editor tests need `QT_QPA_PLATFORM=offscreen` when run directly. Do not raise
budgets, weaken warnings or enable exceptions to pass a gate.
