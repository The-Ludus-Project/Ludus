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

- [ ] Read `AGENTS.md`, `.kiro/steering/`, ADRs 0003-0005 and 0007, the
  architecture, and the existing editor files listed in design.md section 1.
- [ ] Compare the checkout with the architecture's baseline table. Record the
  actual revision and any newer editor work in
  `docs/architecture/code-editor-decision-log.md`. Reuse compatible code; do not
  overwrite another implementation.
- [ ] Add the keypress-to-paint latency probe through the existing editor
  profiling scopes, and the `[.benchmark]` corpus (real shaders, 100,000-line
  C++, 8 MiB JSON, a 1 MiB single line, typing during diagnostics, 100 cursors).
  Record baseline numbers for the current editor.
- [ ] Write the tree-sitter dependency ADR under `docs/decisions/`: library and
  grammar revisions, licenses, recipes, the Wasm build, warning handling and
  update policy. Verify each grammar's license and query set at the pinned
  revision.
- [ ] Decide the owner of the sequence diff (recommended: FoundationContainers)
  and of the code-buffer recovery journal.
- [ ] Add the language registry skeleton with Slang and plain text, replacing
  the `CodeLanguage` switch statements without changing behavior.
- [ ] Copy the decided shortcut resolution (architecture, "Settings, keymaps
  and EditorConfig", decided 2026-11-03) into the decision log, and list the
  existing commands and tests it affects.

Gate: the baseline benchmark results are recorded; the ADR is ready for review;
behavior is unchanged.

## M1 - Every text file is a document

Requirements: CE01-CE04, CE20, CE22, CE23. Prerequisite: M0.

- [ ] Implement the file document host (design section 3), including
  thresholds, read-only invalid UTF-8, atomic save, external-change conflicts
  and EditorConfig.
- [ ] Implement the code document registry, preview tabs, two editor groups,
  view mirroring and restoring tabs per project.
- [ ] Route Project Explorer, Quick Open, Error List and navigation to code
  tabs. Files owned by a render-graph package open its Source pane. Keep Open
  Externally.
- [ ] Move the Scripts work area's text onto `CodeEditor` through a script host,
  keeping S5 draft, Save, reload and source-stop behavior.
- [ ] Add code-buffer recovery and the dirty-close prompts.
- [ ] Add the status bar items: position, selection count, indentation,
  encoding, line endings, language and tool state.
- [ ] Update the user guide: create `docs/development/code-editor.md`, register
  it, and update `editor-workspace.md`.

Gate: every listed file type opens, edits, saves, conflicts and recovers in
offscreen tests and a recorded native run; existing render-graph and Scripts
tests pass.

## M2 - Editing parity

Requirements: CE05-CE07, CE17. Prerequisite: M1.

- [ ] Extend the seam with `CodeEditSet` and the selection set (design
  section 2). Move formatting and Replace All onto it.
- [ ] Implement multiple cursors, column selection, add next and all
  occurrences, and cursors above and below (design section 5).
- [ ] Replace the bottom find panel with the find overlay: regex, whole word,
  in selection, match count and preserve-case replace.
- [ ] Implement the search service, the Search tool window and Replace in
  Files with preview (design section 8).
- [ ] Add auto-closing and surrounding pairs and indentation rules from the
  registry.
- [ ] Add the Ludus and VS Code keymap profiles, user overrides, conflict
  detection and the first-use choice. Apply the decided bindings: Record
  Profiling to Ctrl+Alt+F9, F9 Toggle Breakpoint in code editors, Ctrl+K palette
  in the Ludus profile, and Ctrl+Shift+P palette with Ctrl+K chords in the VS
  Code profile. Update the documents that name F9 for profiling
  (`docs/development/editor-workspace.md`, `docs/architecture/graphics-debugging.md`)
  in the same change. Add
  shortcut-collision tests for both profiles.
- [ ] Measure 1-cursor and 100-cursor typing against the budgets.

Gate: the CE05-CE07 and CE17 tests pass; the typing budgets are recorded.

## M3 - Syntax service

Requirements: CE08, CE19 (syntax roles), CE22. Prerequisite: M0 ADR accepted, M1.

- [ ] Add the tree-sitter recipes and the private C target; build them on every
  supported native and Wasm configuration.
- [ ] Implement the worker, snapshot posting at the debounce, token packing,
  delta shifting and the revision checks (design section 6).
- [ ] Turn `CodeSyntaxHighlighter` into the token applier with the lexical
  fallback and sliced reapplication.
- [ ] Add queries and role mappings for Slang, Luau, C, C++, JSON, CMake and
  Markdown. Add the syntax roles and contrast pairs to the design tokens.
- [ ] Add folding (syntax with indentation as the fallback), outline,
  breadcrumbs, sticky scroll and expand/shrink selection.
- [ ] Measure open, scroll and reparse budgets, and the large-file behavior.

Gate: highlighting is correct on the corpus; a stale result never lands at a
wrong offset under randomized edit tests; the budgets are recorded.

## M4 - Generic language client

Requirements: CE09-CE11, CE20. Prerequisite: M2, M3.

- [ ] Move `SlangLanguageService` into `LanguageClient` with no behavior change;
  all existing slangd tests must pass unchanged.
- [ ] Add capability dispatch, the request table, priorities, cancellation,
  incremental synchronization, position-encoding negotiation and the restart
  policy.
- [ ] Add hover, Analysis diagnostics, semantic tokens, inlay hints, document
  and workspace symbols, the remaining Go to commands, references window,
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

Gate: CE01-CE25 each have evidence or an accepted, recorded exception.

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
