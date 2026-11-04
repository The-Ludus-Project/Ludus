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
