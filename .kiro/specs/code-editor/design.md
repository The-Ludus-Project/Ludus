# Code editor implementation design

Status: proposed; implement through [tasks.md](tasks.md). Requirement IDs are in
[requirements.md](requirements.md). Product rationale, UX, budgets, decisions and
references are owned by the
[architecture](../../../docs/architecture/code-editor.md); this file maps them
onto the repository and does not restate them. The implemented foundation's
contracts are owned by
[Source editing direction](../../../docs/architecture/editor-gui-systems.md#source-editing-direction).
Names below are proposals: follow the checked-out conventions where they differ,
and record the deviation in the decision log.

## 1. Repository fit

All code stays in `apps/editor` (private target, never installed). The editor
does not yet adopt the naming-tool scope; new files still follow every naming
rule in `AGENTS.md` and are reviewed for it.

| Area | Existing files | New or changed files (proposed) |
| --- | --- | --- |
| Seam values | `internal/code_document.h` | Add `CodeEditSet`, `CodeSelectionSet`, `CodeTransactionId`; keep `ApplyEdit` as a one-edit wrapper over `ApplyEdits` |
| Hosts | `internal/render_graph_code_document.h` | `internal/file_code_document.h/.cpp`, `internal/script_code_document.h/.cpp` |
| Registry and tabs | `main_window_project_files.cpp`, `external_code_editor.*` | `internal/code_document_registry.h/.cpp`, `internal/code_document_tab.h/.cpp`, `internal/code_editor_group.h/.cpp` |
| Controller | `code_document_controller.*`, `code_edit_scope.h` | Extend with the selection set and `EditSet` building; `internal/code_multi_cursor.h/.cpp` for cursor painting and input distribution |
| View | `code_editor.*`, `code_editor_panel.*`, `code_text.*` | `internal/code_decorations.h/.cpp` (lanes, inline layers, overview ruler); `internal/code_find_overlay.h/.cpp` replaces the bottom panel; `internal/code_fold_model.h/.cpp` |
| Languages | `CodeLanguage` enum in `code_editor.h` | `internal/code_language_registry.h/.cpp`; the enum becomes a registry index |
| Syntax | `code_syntax_highlighter.*` | `internal/syntax_service.h/.cpp` (worker), `internal/syntax_tokens.h/.cpp` (packed tokens and delta shifting); the highlighter becomes the token applier |
| Language services | `slang_language_service.*`, `code_assistance.*`, `render_graph_shader_assistance.*`, `code_navigation*` | `internal/language_client.h/.cpp` (generic LSP), `internal/language_profiles.h/.cpp`, `internal/code_assistance_presenter.h/.cpp`, `internal/workspace_edit.h/.cpp`, `internal/script_context.h/.cpp`, `internal/native_context.h/.cpp` |
| Formatting | `code_formatter.*` | Profiles for LSP, clang-format, StyLua and JSON; whole-text results go through the sequence diff |
| Search | — | `internal/code_search_service.h/.cpp`, `internal/search_results_window.h/.cpp` |
| Debugging | `script_workspace.*` (S5 stops), `play_process.*` | `internal/debug_client.h/.cpp` (DAP), `internal/luau_debug_adapter.h/.cpp`, `internal/debug_windows.h/.cpp` |
| Change markers | `tool_process.*` | `internal/change_markers.h/.cpp` |
| Sequence diff | — | A Ludus-owned Myers diff. It is a general facility: M0 decides its owner (recommended: a FoundationContainers algorithm beside `sort.hpp`, with its own tests), not a private editor copy |
| Theme | `config/design-tokens.json`, `editor_style.*` | Syntax roles and contrast pairs |
| Tools | `config/tool_versions.json`, `init.sh`, `scripts/python/ludus_tools` | Pins and per-platform hashes for `luau-lsp` and StyLua; resolvers for clangd, clang-format and `lldb-dap` from the pinned LLVM |
| Third party | `conanfile.py` | `tree-sitter`, `tree-sitter-c`, `tree-sitter-cpp` from ConanCenter; Ludus-owned recipes for the Slang, Luau, JSON, CMake and Markdown grammars; query files installed beside the editor; notices updated |
| Tests | `apps/editor/tests/code_*_tests.cpp`, `formatter_fixture.py` | Scripted fake LSP and DAP servers as Python fixtures in the style of `formatter_fixture.py`; new `[code-editor]` tests; extended `[.benchmark]` corpus |
| Docs | `docs/development/render-graph-authoring.md`, `docs/development/editor-workspace.md` | A user guide `docs/development/code-editor.md` (setup, keymaps, languages, limits), registered in the docs index and wiki |

Tree-sitter is C. Compile it and its grammars as C in a private static target
linked only by the editor, with the project warning set where the sources allow;
any suppression is limited to that target and justified in the PR. No exceptions
cross the boundary because there are none.

## 2. Seam changes (CE02, CE05, CE11)

```cpp
/// An ordered set of non-overlapping replacements applied as one revision.
struct CodeEditSet final
{
    foundation::BorrowedView<const CodeTextEdit> Edits; ///< Ascending StartBytes, no overlap.
    CodeSelectionSet Before;
    CodeSelectionSet After;
    CodeTransactionId Transaction; ///< Zero, or shared by every file of one workspace edit.
};

/// The primary cursor first, then secondary cursors in document order.
struct CodeSelectionSet final
{
    foundation::BorrowedView<const CodeTextSelection> Selections;
};

class CodeDocumentHost
{
public:
    // ...existing members...
    [[nodiscard]] virtual CodeEditStatus ApplyEdits(const CodeDocumentIdentity& expected,
                                                    const CodeEditSet& edits,
                                                    bool shouldMerge) = 0;
    /// Called after a successful ApplyEdits so other views can mirror the delta.
    virtual void SetEditObserver(CodeEditObserver* observerOrNull) noexcept = 0;
};
```

- Validate all edits before changing anything: order, overlap, UTF-8 scalar
  boundaries, capacity after the whole set. Either all apply or none do.
- The host copies inserted bytes before returning, as it does today.
- The registry is the single observer. It fans out applied deltas to every view
  of the document except the one that made the edit, and to the syntax service.
- Selections are stored in UTF-8 bytes. Mapping to and from Qt UTF-16 positions
  stays in `CodeDocumentController`, as the existing seam already requires.
- Undo and redo restore the whole selection set. Selection hints stay out of
  content equality and serialization, as they do today.

## 3. File document host (CE03, CE04, CE22)

- The load happens on the background pool: read, check UTF-8, detect the BOM,
  line endings and indentation, and build the line-start index. The host is
  created on the UI thread only after the load succeeds. A failed or invalid
  load produces a read-only document with an explanation.
- Storage is a contiguous UTF-8 buffer. `ApplyEdits` rebuilds in one pass when
  the set has more than one edit, and moves bytes in place for a single edit.
- One owner constant per threshold. Proposed names, with values from the
  architecture: `CODE_FILE_NORMAL_LIMIT_BYTES`, `CODE_FILE_NORMAL_LIMIT_LINES`,
  `CODE_FILE_LARGE_LIMIT_BYTES`, `CODE_FILE_READ_ONLY_LIMIT_BYTES`,
  `CODE_LONG_LINE_UTF16_UNITS`. Every read, admission and degradation decision
  uses these constants.
- Saving uses `QSaveFile`. A save that fails keeps the buffer dirty and reports
  the error. The external-change watcher is the existing `source_watch`;
  compare the size, modification time and a content hash before declaring a
  conflict, so the editor's own save does not trigger one.
- Recovery: M0 finds the existing recovery owner (render-graph recovery, the
  World draft recovery) and either generalizes it to code buffers or adds a
  bounded journal under the project's local state directory. Write snapshots on
  an idle debounce from the background pool. Never block the UI thread on disk.
- EditorConfig: a small Ludus-owned parser for the subset that applies
  (`indent_style`, `indent_size`, `tab_width`, `end_of_line`, `charset`,
  `insert_final_newline`, `trim_trailing_whitespace`), with section glob
  matching. Use the bounded Ludus text-to-number conversion, not `std::sto*`.
  `trim_trailing_whitespace` and `insert_final_newline` apply as one edit set
  just before saving.

## 4. Views, tabs and groups (CE01, CE02)

- `CodeDocumentTab` holds one `CodeEditor`, its controller binding and its view
  state: folds, cursors, scroll position and wrap. A tab is a document in the
  existing document well, so Save, Undo and Close commands route the way they
  already do for World and Render Graph.
- `CodeEditorGroup` arranges tabs. Two groups are enough for M1; the design
  must not rule out more.
- Each view's `QTextDocument` is filled from the host snapshot and updated from
  observed deltas inside one `QTextCursor` edit block. The view's
  `contentsChange` caused by mirroring is marked so that it is not sent back as
  a user edit.
- Preview-tab state is a flag on the tab. Project-local settings store the open
  tabs, the active tab, cursors and scroll positions per project.
- Opening a file that the render-graph package already owns opens the Render
  Graph document's Source pane, not a second host for the same text.

## 5. Multiple cursors (CE06)

- The selection set lives in the controller. The view draws secondary carets
  and selections in its paint pass (the primary uses Qt's own cursor), so Qt's
  accessibility and IME see one cursor.
- Key handling turns each input into one `CodeEditSet`. Edits are computed for
  each selection from the end of the document backwards, so earlier offsets stay
  valid, and are then sorted ascending for the host.
- Merge selections that touch after any edit or movement.
- Movement commands (arrows, Home/End, word moves) apply to every cursor using
  Qt's own movement logic on temporary `QTextCursor`s, so platform behavior is
  preserved.
- Paste: when the clipboard's line count equals the cursor count, each cursor
  gets one line; otherwise each gets the whole text.

## 6. Syntax service (CE08)

- One `QThread` owns every tree-sitter parser, tree and query cursor. Work items
  carry the document identity, revision, the accumulated `TSInputEdit`s and a
  copied UTF-8 snapshot. Results are posted back with a queued signal.
- The UI thread drops any result whose revision is older than the document's
  last delivered revision. Tokens from older revisions are moved by
  `syntax_tokens` using the edit deltas until a newer result arrives.
- Token storage is one array of packed `uint32` pairs per block with a block
  revision, kept in `QTextBlockUserData`, which already holds bracket data.
- `CodeSyntaxHighlighter::highlightBlock` applies cached tokens when the block
  revision matches, and otherwise runs the existing lexer. After a result,
  `rehighlightBlock` runs for visible blocks first and the rest in slices of at
  most 2 ms (`CODE_UI_SLICE_MICROSECONDS`).
- Queries come from `config/code-languages/<language>/*.scm`, installed with
  the editor. M0 confirms the grammar revisions and their query sets.
- Capture names map to design-token syntax roles in one table in the registry.
- Folds, outline, breadcrumb scopes and sticky headers come from the same pass,
  bounded by `CODE_MAX_FOLDS` and `CODE_MAX_OUTLINE_ITEMS`.

## 7. Language client (CE09, CE10, CE11, CE20)

- Move framing, process supervision and decoding from `SlangLanguageService`
  into `LanguageClient` without changing behavior, then add features. Keep
  every existing bound as a profile default and the existing stale-result
  identity.
- A request table maps a feature to its LSP method, required capability,
  priority, debounce and result decoder. A command whose capability is absent
  is disabled with its reason.
- Request queue: one in-flight request per interactive kind (completion,
  signature, hover). Background kinds (semantic tokens, inlay hints, symbols,
  diagnostic pulls) are coalesced per document. Send `$/cancelRequest` for any
  superseded request.
- `textDocument/publishDiagnostics` and pull diagnostics feed a decoration set
  whose source is "Analysis". The Error List shows Analysis and Build groups.
- Restart policy: exponential backoff starting at 1 s. Stop after three crashes
  within `CODE_SERVER_CRASH_WINDOW_SECONDS` = 60, then show Retry.
- Workspace edits: validate every target URI against the context inventory,
  reject generated or read-only spans, open the affected files through the
  registry (bounded by `CODE_MAX_WORKSPACE_EDIT_FILES` = 256), and apply one
  `CodeEditSet` per file with a shared `CodeTransactionId`. If any file fails,
  apply none: validate them all first.
- Profiles:
  - Slang: the existing `slangd` resolution and shader context.
  - Luau: `luau-lsp lsp --definitions=<contract.d.luau>`, with the definitions
    path supplied by `script_context` from the active behavior package.
  - C/C++: clangd with `--compile-commands-dir=<active preset build dir>` and
    `--background-index`. Never pass `--query-driver`.
- Completion aggregation merges server items, snippets, words from open buffers
  and project symbols, then ranks them with the command palette's existing
  fuzzy scorer. Resolve documentation lazily for the selected item only.

## 8. Search (CE07)

- Enumerate files on the search worker from the project's authored roots,
  skipping build directories (read from the project's preset layout), `.git`,
  files with a NUL in the first 8 KiB, and files above `CODE_SEARCH_FILE_LIMIT_BYTES`.
- Dirty buffers are passed in as copied snapshots when the search starts.
- Results are streamed in batches of at most 256 matches through queued
  signals, with a total of `CODE_SEARCH_MAX_MATCHES` = 10,000.
- Regular expressions use `QRegularExpression` with
  `QRegularExpression::UseUnicodePropertiesOption` off by default. Reject
  patterns that fail to compile with the error shown inline.
- Replace in Files produces a workspace edit (section 7) and shows a preview
  tree before anything is applied.

## 9. Debugging (CE16)

- `DebugClient` speaks DAP over the existing tool process supervisor, with the
  same framing limits as the language client.
- `LuauDebugAdapter` runs in the editor process and translates DAP into the
  versioned GameHost debug capability. The
  [scripting debugging contract](../../../docs/architecture/scripting.md#debugging-contract)
  owns the semantics; if a DAP request has no matching capability, report it as
  unsupported. Do not invent behavior.
- The breakpoint table belongs to the debug client and is keyed by canonical
  path. Breakpoint positions are anchored like diagnostics so they move with
  edits, and are re-sent on save.
- `lldb-dap` is resolved from the pinned LLVM. Launch and attach configurations
  come from the project's existing play/launch description, never from a
  project-provided command line.

## 10. Testing strategy

- Production-path offscreen tests for every requirement: the host, edit sets,
  multiple cursors, find, the registry and tabs, recovery, EditorConfig, token
  shifting and stale-result rejection.
- Fake LSP and DAP servers as Python fixtures that replay scripted sessions,
  including crashes, timeouts, oversize messages, wrong position encodings and
  stale replies. They run under the pinned host Python.
- Real-server tests behind tags that skip when the pinned tool is not installed
  (`[slangd]`, `[luau-lsp]`, `[clangd]`, `[lldb-dap]`). Report skipped tests
  honestly.
- `[.benchmark]` corpus and latency probe for CE21. Keep raw results in ignored
  output and record the summary in tasks.md.
- Shell shortcut-collision tests for both keymap profiles.
- Native interactive runs for IME, accessibility and multiple cursors, recorded
  in the ledger. Offscreen tests do not qualify these.
