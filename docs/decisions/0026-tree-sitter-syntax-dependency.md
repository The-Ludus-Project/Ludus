# ADR 0026: Adopt tree-sitter for code-editor syntax structure

## Status

Accepted, with the [LUSL grammar amendment](#amendment-the-lusl-grammar), when
the maintainer merges the acceptance change into `develop`. The merge itself
records the acceptance. The editor's syntax service milestone (M3 of the
[code editor](../architecture/code-editor.md#syntax-service)) and the LUSL
grammar may land from then on, each in its own change that records verified
pins in the manifest. M1 and M2 of the code editor never depended on this ADR.

## Context

The [code editor architecture](../architecture/code-editor.md) replaces the
single-line Slang lexer with accurate highlighting, folding, outline,
breadcrumbs, sticky scroll and expand/shrink selection for Slang, Luau, C, C++,
JSON, CMake and Markdown. The architecture's
[syntax-service decision](../architecture/code-editor.md#syntax-service) chose
tree-sitter for this, with the existing lexers kept as an instant fallback, and
required an ADR under the
[standard-library usage policy](0003-standard-library-usage-policy.md)'s review
model before code lands. This ADR records that review: the library and grammar
revisions, their licenses, the Conan and Ludus-owned recipes, the Wasm build,
warning handling and the update policy.

Tree-sitter is an incremental parsing library written in C. It recovers from
errors, uses no exceptions (which satisfies the engine's `-fno-exceptions`
boundary with no translation-unit wrapper), builds for WebAssembly, and has
maintained grammars for every first language. Neovim, Helix and Zed use it in
production. The [alternatives the architecture rejected](../architecture/code-editor.md#decisions-and-alternatives)
(TextMate/Oniguruma regex grammars, KSyntaxHighlighting, more hand-written
lexers, in-process type checkers) are not revisited here.

### Why these dependencies are not already covered

The engine's standard-library policy (ADR 0003) defaults new runtime facilities
to Ludus-owned implementations. Tree-sitter is an exception justified exactly as
HarfBuzz was for text shaping: writing and maintaining incremental,
error-recovering parsers for seven languages is far more code and risk than
adopting a proven, narrowly-scoped C library behind a thin seam. The grammars
are data (generated parser tables plus a small C scanner), not general runtime
facilities, and the editor is the only consumer; nothing enters the installed
SDK.

## Decision

Adopt the tree-sitter C runtime and the seven first grammars as editor-private
third-party dependencies, pinned by exact tagged revision and verified source
hash, compiled as C into one private static target linked only by the editor.

### Library and grammars

| Component | Upstream | License | Recipe |
| --- | --- | --- | --- |
| tree-sitter runtime | `tree-sitter/tree-sitter` (the `lib/` C runtime only) | MIT | Ludus-owned `third_party/tree-sitter` |
| tree-sitter-c | `tree-sitter/tree-sitter-c` | MIT | ConanCenter if the pinned version is published, else Ludus-owned |
| tree-sitter-cpp | `tree-sitter/tree-sitter-cpp` | MIT | ConanCenter if the pinned version is published, else Ludus-owned |
| tree-sitter-slang | `tree-sitter-grammars/tree-sitter-slang` | MIT | Ludus-owned |
| tree-sitter-luau | `polychromatist/tree-sitter-luau` | MIT | Ludus-owned |
| tree-sitter-json | `tree-sitter/tree-sitter-json` | MIT | Ludus-owned |
| tree-sitter-cmake | `uyha/tree-sitter-cmake` | MIT | Ludus-owned |
| tree-sitter-markdown | `tree-sitter-grammars/tree-sitter-markdown` | MIT | Ludus-owned |

All eight are MIT, which is compatible with Ludus's license and attribution
rules. Every grammar used here emits a generated `parser.c` (and some a
hand-written `scanner.c`); the editor compiles those as C and links the runtime,
rather than vendoring a prebuilt binary.

Only the runtime's `lib/` C sources and headers are taken from the
`tree-sitter/tree-sitter` repository; its Rust CLI, bindings and web playground
are not. The CLI is not needed at build time: the pinned grammars ship their
pre-generated `parser.c`, so the editor build never regenerates a grammar.

### Pinning discipline

> **Amended in M3 (2026-11-07): CMake `FetchContent` instead of Conan.** The
> browser editor build does not run Conan; that is why HarfBuzz still has no
> web build. A Conan recipe would therefore fail this ADR's
> [WebAssembly requirement](#webassembly-build) that native and browser builds
> compile the same C sources.
>
> Each component instead commits its pin to `third_party/<name>/SOURCE-LOCK.json`
> (version, tag, commit, archive URL, SHA-256) and its license text to
> `third_party/<name>/`. The editor's CMake fetches the archive with
> `FetchContent`, which fails on a hash mismatch, and checks the license against
> the committed copy. That is the mechanism the editor already uses for its
> Crashpad pin.
>
> The rest of this section still applies: exact tags, verified hashes, the
> manifest and no floating versions. The one exception is
> `polychromatist/tree-sitter-luau`, which publishes no tags. It is pinned to
> an exact commit and that commit's archive hash, and the
> [manifest](../architecture/code-editor-evidence/tree-sitter-manifest.md#grammars)
> records why. The text below keeps the original recipe wording for the record.

Each dependency gets a Ludus-owned Conan recipe under `third_party/<name>/`
(following `third_party/harfbuzz`) that downloads the exact upstream tagged
archive with a verified SHA-256, or ConanCenter for `tree-sitter-c` and
`tree-sitter-cpp` if and only if the pinned version is published there. The
exact tag, commit and SHA-256 for every component, plus the per-platform build
inputs, are recorded in a dependency manifest at
`docs/architecture/code-editor-evidence/tree-sitter-manifest.md` and are never
changed without re-verifying against the upstream release, exactly as the
[HarfBuzz pin](../architecture/text-font-rendering-evidence/dependency-manifest.md)
requires. Versions are added to `config/tool_versions.json`/`conanfile.py` as
appropriate so the existing pin-consistency checks cover them.

Concrete revisions are intentionally filled in by the M3 recipe change rather
than this ADR, so the pins are verified against a working checkout and build.
This ADR fixes the components, their licenses, the recipe home, the compilation
model and the update policy; M3 records the verified tags and hashes in the
manifest and cites this ADR.

### Query files

Each grammar's highlight, fold, indent, locals, injection and textobject query
files (`*.scm`) are installed as data beside the editor under
`config/code-languages/<language>/`, loaded at runtime. Their provenance (the
same tagged revision as the grammar, or a Ludus-authored query where upstream
has none) is recorded in the manifest with the grammar. Capture names map to the
design-token syntax roles in one table in the language registry. The query set
and the grammar revision are verified together, because a query references the
grammar's node names.

### Compilation and warnings

- The runtime and all grammar sources compile as C in one private static target
  (`ludus_editor_tree_sitter`), linked only by the editor, never installed.
- The target builds with the project warning set where the generated sources
  allow it. Generated `parser.c` tables and upstream scanners can trip specific
  warnings; any suppression is scoped to this target with a per-warning
  justification in the recipe or target, and is not applied to engine code. The
  engine's warnings-as-errors policy and `-fno-exceptions` are unchanged; no
  warning is globally weakened to accommodate the grammars.
- The runtime calls each external scanner's `create` entry point through a
  prototyped `void *(*)(void)` slot, and the C++, Slang, Luau, CMake and
  Markdown inline scanners define it with an empty parameter list. In C11 that
  is an unprototyped, different function type, which Clang's function
  sanitizer rejects under the UBSan presets. Those grammar objects compile as
  C23, where an empty list means `(void)`, so every sanitizer check stays on.
  MSVC compiles those grammars as C11 and has no Clang function sanitizer.
- MSVC runtime diagnostics C4018, C4232 and C4244 are scoped out on the upstream
  runtime target for its signed capture bounds, imported CRT allocator callback
  initializers and internal symbol/capture narrowing. The editor's own sources
  retain those warnings as errors.
- No exceptions cross the boundary because tree-sitter has none. The editor seam
  values stay Qt-free and tree-sitter-free; the syntax worker owns every
  `TSParser`, `TSTree` and `TSQueryCursor`.

### WebAssembly build

The browser editor compiles the same runtime and grammar C sources with the
pinned Emscripten toolchain ([ADR 0009](0009-browser-webgpu-toolchain.md)),
producing the parser in the browser syntax worker (or in main-thread slices when
threads are unavailable), under the same stale-result rules as native. The
runtime's own `lib/binding_web` and `wasm` CLI paths are not used; the editor
drives the C API directly, so the browser build is the same C compiled for
Wasm, not a separate WebAssembly parser distribution.

### Update policy

Tree-sitter and its grammars are updated deliberately, never floated. A bump
changes the pinned tag and verified hash in the manifest and the recipe in one
change, re-verifies the query set against the new grammar node names, and runs
the M3 highlighting-correctness and randomized-edit regression tests before the
bump lands. Grammar node-name changes that break a query are treated as a
breaking change and fixed in the same bump. The manifest records who verified
each pin and when.

## Amendment: the LUSL grammar

The [Ludus Shading Language](0027-ludus-shading-language.md) has no upstream
grammar. Its [design](../architecture/shading-language.md#tooling) asks for a
Ludus-owned grammar under this ADR's recipe and pinning rules. The table above
covers only upstream grammars, which ship a generated `parser.c`, and this ADR
keeps the tree-sitter CLI out of the build. A grammar Ludus writes needs that
CLI to generate its parser, so the amendment fixes how it is generated, checked
and kept in step with the compiler.

| Component | Source | License | Home |
| --- | --- | --- | --- |
| tree-sitter-lusl | Ludus-authored `grammar.js` and queries | The Ludus project license | `apps/editor/syntax/lusl/` |

- **Generated parser in the repository.** `grammar.js` and the generated
  `src/parser.c` (plus `src/tree_sitter/*.h`) are committed together. The
  editor build compiles `parser.c` into `ludus_editor_tree_sitter` like the
  upstream grammars, and never runs the CLI.
- **Pinned generator.** One script regenerates the parser with the tree-sitter
  CLI version pinned in `config/tool_versions.json`. That is the same version
  as the runtime pin, so the parser's ABI matches the runtime the editor links.
  The script writes the SHA-256 of `grammar.js` and the CLI version into a
  header comment of `parser.c`.
- **Staleness check without the CLI.** A test compares `grammar.js`'s SHA-256
  and the pinned CLI version with the values recorded in `parser.c`. It fails
  when someone edits the grammar without regenerating, or bumps the pin without
  regenerating. CI therefore needs neither Node nor the CLI.
- **One keyword source.** A test checks that the grammar's keywords equal
  `tooling::shader_language::GetKeywords()`, including contextual words such as
  `space` and `slang`. The compiler stays the owner, and the editor's lexical
  fallback and the grammar cannot drift from it.
- **Queries.** Highlight, fold, indent, locals and textobject queries are
  Ludus-authored under `config/code-languages/lusl/`. They are verified against
  the grammar's node names in the same way as the upstream queries.
- **Scope.** The grammar gives syntax structure only. Types, names and
  diagnostics stay with `luslc lsp`, which runs the real compiler.
- **Order.** The LUSL grammar lands after, or together with, the M3 change that
  adds the runtime recipe.

Updates follow the [update policy](#update-policy): a LUSL syntax change edits
the compiler, `grammar.js`, the regenerated parser and the queries in one
change.

## Consequences

- The editor gains accurate structure for seven languages from one tree per
  document, replacing per-language hand-written parsers the engine would
  otherwise own and maintain.
- Eight new pinned third-party components enter `third_party/` and the
  dependency manifest. The Ludus-owned LUSL grammar adds a generated parser
  that is checked against its source hash rather than downloaded. CI's pin-consistency and notice checks must cover them;
  the third-party notices file is updated with the eight MIT grammars and the
  runtime in the M3 change.
- The browser editor carries the same grammars compiled to Wasm, increasing the
  browser payload; the architecture already accounts for this in its browser
  parity check (M9).
- Grammar revisions are a maintenance surface: a language's upstream grammar may
  lag the language itself. The architecture keeps the lexical fallback for
  exactly this reason, so an outdated or missing grammar degrades to lexical
  coloring rather than breaking editing.
- Not fixed by this ADR and deferred to the M3 recipe change: the exact tags,
  commits and SHA-256 hashes; whether `tree-sitter-c`/`tree-sitter-cpp` are
  taken from ConanCenter or a Ludus-owned recipe at the pinned version; and the
  per-platform warning suppressions the generated sources actually require. Each
  is recorded in the manifest with its verification.
