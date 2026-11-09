# Code editor: verified tree-sitter manifest

This manifest records every tree-sitter component the editor's
[syntax service](../code-editor.md#syntax-service) builds, as
[ADR 0026](../../decisions/0026-tree-sitter-syntax-dependency.md) requires.
Each hash below was computed from the archive actually downloaded. Never change
a pin without downloading and verifying the new archive, and record who
verified it and when.

## Runtime

| Component | Version | Tag and commit | Archive | SHA-256 | License |
| --- | --- | --- | --- | --- | --- |
| tree-sitter runtime (`lib/` only) | 0.27.1 | `v0.27.1`, `10eb5ac119eec9cb4267a8e035066473983f5f3a` | https://github.com/tree-sitter/tree-sitter/archive/refs/tags/v0.27.1.tar.gz (1,026,138 bytes) | `982cd3d4d9eb7be18c243240a622423fd2e4ecb4bec2cd98832c2f3fa1f0f333` | MIT, `LICENSE` (Max Brunsfeld), SHA-256 `c5cfb43042b6b72045f4ba997834d0a7786d2793d91680868b5815b39f14fc78` |

- **Verification:** downloaded twice on 2026-11-07, with identical hashes. The
  tag points at that commit (GitHub API `git/ref/tags/v0.27.1`), and the
  archive's `lib/include/tree_sitter/api.h` defines
  `TREE_SITTER_LANGUAGE_VERSION 15` and
  `TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION 13`.
- **Why this release:** v0.27.2 was published on the day of the pin. v0.27.1
  (2026-10-08) had a month of use, so it meets the two-week minimum age for
  pinned artifacts.
- **Grammar ABI:** grammar pins must be generated for ABI 13 through 15. A
  test fails if the runtime's range changes.

## How the build uses the pin

- **Recorded pin:** `third_party/tree-sitter/SOURCE-LOCK.json` records the
  values above. `third_party/tree-sitter/LICENSE` is the archive's `LICENSE`,
  byte for byte.
- **Fetch and verify:** `apps/editor/cmake/EditorTreeSitter.cmake` downloads
  the archive with CMake `FetchContent`, and the download fails on a SHA-256
  mismatch. Configuration then fails if the archive's `LICENSE` differs from
  the committed copy.
- **What is compiled:** only `lib/src/lib.c`, the runtime's amalgamation, with
  the macros upstream's build defines: `_POSIX_C_SOURCE=200112L`,
  `_DEFAULT_SOURCE`, `_BSD_SOURCE` and `_DARWIN_C_SOURCE`. The Wasm-store
  feature stays off. The Rust CLI, bindings and upstream build files are
  unused.
- **Target:** `ludus_editor_tree_sitter`, a private static C11 target linked by
  `ludus_editor_code`. Native and Emscripten builds compile the same source.
  Nothing is installed or exported to the SDK.

### Warnings

The target uses the C subset of the project warning set, with
warnings-as-errors when `LUDUS_WARNINGS_AS_ERRORS` is on. These suppressions
apply to this target only:

| Warning | Source | Reason |
| --- | --- | --- |
| `-Wundef` | `lib/src/unicode/umachine.h` | The runtime's trimmed ICU headers test platform macros such as `U_PLATFORM` that the trimmed copy never defines; ICU relies on them evaluating to 0 |
| `-Wmissing-format-attribute`, `-Wformat-nonliteral` (from `-Wformat=2`) | `lib/src/lexer.c`, `ts_lexer__log` | An internal helper forwards a printf format to `vsnprintf` without declaring the attribute |

Measured with the pinned Clang 23 on macOS (arm64). Another compiler that
reports more must add its suppression here, with a reason, in the change that
needs it.

## Browser package

`scripts/build-web-editor` copies `third_party/tree-sitter/LICENSE` into the
package as `licenses/tree-sitter.txt`, and each grammar's
`third_party/tree-sitter-<language>/LICENSE` as
`licenses/tree-sitter-<language>.txt`. `--check-package` requires all eight.

## Grammars

The seven upstream grammars of ADR 0026, pinned on 2026-11-08 by Claude Code for
Minha Ju. Each archive was downloaded twice, with identical hashes, and the tag
was resolved to its commit through the GitHub API. Every pin is at least two
weeks old on that date.

| Grammar | Version | Tag and commit | Published | Archive | SHA-256 | Grammar ABI |
| --- | --- | --- | --- | --- | --- | --- |
| [tree-sitter-c](https://github.com/tree-sitter/tree-sitter-c) | 0.24.2 | `v0.24.2`, `b780e47fc780ddc8da13afa35a3f4ed5c157823d` | 2026-04-22 | `archive/refs/tags/v0.24.2.tar.gz` (381,758 bytes) | `2eeb4db31f8fa0865e45488503d13403923bcb485a1bdb637abff8c42dd97364` | 15 |
| [tree-sitter-cpp](https://github.com/tree-sitter/tree-sitter-cpp) | 0.23.4 | `v0.23.4`, `f41e1a044c8a84ea9fa8577fdd2eab92ec96de02` | 2024-11-11 | `archive/refs/tags/v0.23.4.tar.gz` (1,135,921 bytes) | `7a2c55afe3028f4105f25762ea58cc16537d1f5a1dcd9cca90410b3cd5d46051` | 14 |
| [tree-sitter-slang](https://github.com/tree-sitter-grammars/tree-sitter-slang) | 0.3.1 | `v0.3.1`, `c7ab1ee2bea2ac431fa78d72b80d8957d5a5bcd3` | 2025-07-29 | `archive/refs/tags/v0.3.1.tar.gz` (1,850,533 bytes) | `81c5ca98baa698071294d5d7f71c11e77000501a3c0f7a8cd0fec08e117c09ff` | 15 |
| [tree-sitter-luau](https://github.com/polychromatist/tree-sitter-luau) | 0.3.0 | no tag, `71b03e66b2c8dd04e0133c9b998a54a58f239ca4` | 2025-12-08 | `archive/71b03e66b2c8dd04e0133c9b998a54a58f239ca4.tar.gz` (244,057 bytes) | `353fbd5bfa204025e717cea2296cd0f678454ccc90f7745a9341b76cbe746431` | 14 |
| [tree-sitter-json](https://github.com/tree-sitter/tree-sitter-json) | 0.24.8 | `v0.24.8`, `ee35a6ebefcef0c5c416c0d1ccec7370cfca5a24` | 2024-11-11 | `archive/refs/tags/v0.24.8.tar.gz` (39,832 bytes) | `acf6e8362457e819ed8b613f2ad9a0e1b621a77556c296f3abea58f7880a9213` | 14 |
| [tree-sitter-cmake](https://github.com/uyha/tree-sitter-cmake) | 0.7.5 | `v0.7.5`, `e997bd0b275ca525ce9befecedf5299031183661` | 2026-09-13 | `archive/refs/tags/v0.7.5.tar.gz` (66,850 bytes) | `b1c664921d06a6609f2b4fdfbd6685baea35066873ae5b75e11be55317d281f9` | 14 |
| [tree-sitter-markdown](https://github.com/tree-sitter-grammars/tree-sitter-markdown) (`markdown` and `markdown_inline`) | 0.5.4 | `v0.5.4`, `41ff24a6c775492c543b09228aeb98d1cd2b3a0f` | 2026-10-24 | `archive/refs/tags/v0.5.4.tar.gz` (429,539 bytes) | `481ce2c1cce7b1130bc8455c186052bd0fa461f1ff488666994c6a3da7fca2ac` | 15 |

Archive paths are relative to each grammar's GitHub repository. The ABI is the
`LANGUAGE_VERSION` of the generated `parser.c`; all lie in the runtime's range
of 13 to 15.

- **Why these releases:** each is the newest tag published at least two weeks
  before 2026-11-08, except for CMake, whose v0.7.6 (2026-10-30) was too new.
  tree-sitter-markdown v0.5.4 was published 15 days before the pin.
- **Luau has no tags.** `polychromatist/tree-sitter-luau`, the upstream ADR 0026
  names, has never tagged a release. The pin is the commit at the head of `main`
  on 2026-11-08, which sets the grammar version to 0.3.0, and the archive is
  that commit's. It is 11 months old.

### Licenses

All are MIT. `third_party/tree-sitter-<language>/LICENSE` is each archive's
`LICENSE`, byte for byte.

| Grammar | Copyright | License SHA-256 |
| --- | --- | --- |
| tree-sitter-c, tree-sitter-cpp, tree-sitter-json | Max Brunsfeld | `2e0110e07abef7c2548b26ec9d6969775617ca539a0dc8dbeeb14d6452c711d1` |
| tree-sitter-slang | Stephan Seitz | `188fdcca13247f567cddb0d5155c622de1a6cf3f0245e52f0986907b6c2819f5` |
| tree-sitter-luau | polychromatist | `e0e95c519c8d60939371897b229a5bec66971c654a8767c6a0e7200911423a09` |
| tree-sitter-cmake | Uy Ha | `97334c0f9c0962b33cbe03223f584b8ed64c2d14419ce80cc2e945eb8d367d28` |
| tree-sitter-markdown | Matthias Deiml | `52ec8a1bf8256511e2a92613e0bb41ffefba3897f16fa958d7bb28c466dd4804` |

### What is compiled

Only the generated parser and the hand-written scanner of each grammar. The
grammar sources (`grammar.js`), bindings and upstream build files are unused,
and no parser is regenerated, so the build needs neither Node nor the
tree-sitter CLI. The generated parsers are never committed to this repository:
`tree-sitter-cpp`'s `parser.c` alone is 17 MB, and Slang's is 27 MB.

| Grammar | Sources | Entry point |
| --- | --- | --- |
| C | `src/parser.c` | `tree_sitter_c` |
| C++ | `src/parser.c`, `src/scanner.c` | `tree_sitter_cpp` |
| Slang | `src/parser.c`, `src/scanner.c` | `tree_sitter_slang` |
| Luau | `src/parser.c`, a repaired copy of `src/scanner.c` | `tree_sitter_luau` |
| JSON | `src/parser.c` | `tree_sitter_json` |
| CMake | `src/parser.c`, `src/scanner.c` | `tree_sitter_cmake` |
| Markdown | `tree-sitter-markdown/src/{parser,scanner}.c` | `tree_sitter_markdown` |
| Markdown inline | `tree-sitter-markdown-inline/src/{parser,scanner}.c` | `tree_sitter_markdown_inline` |

Each grammar's `SOURCE-LOCK.json` lists these sources. Each grammar compiles as
its own C11 object library against its own generated `src/tree_sitter/parser.h`,
and the objects join `ludus_editor_tree_sitter`. The editor declares the entry
points in `apps/editor/src/internal/code_syntax_grammars.h`.
`apps/editor/tests/code_syntax_grammar_tests.cpp` loads every grammar into the
runtime and parses a snippet of each language without `ERROR` or `MISSING`
nodes.

### Luau scanner repair

Upstream's `process_name` grows its name buffer when a name reaches 256 bytes,
but discards the pointer that `ts_realloc` returns. When the allocator moves
the block, the scanner writes through freed memory. The build therefore
compiles a copy of `scanner.c` from the build tree, in which that one line keeps
the result (`name = ts_realloc(...)`). The downloaded tree is unchanged.
Configuration fails if the archive no longer contains exactly that line, so a
pin update must re-check the repair. A test parses a 280-byte Luau name, and
sanitizer builds report a regression there. The fix should be offered upstream,
and the repair removed once a pinned revision contains it.

### Grammar warnings

The grammars use the same C warning set as the runtime. These suppressions
apply only to the grammar named:

| Warning | Grammar | Reason |
| --- | --- | --- |
| `-Wstrict-prototypes` | C++, Slang, Luau, CMake, Markdown inline | The scanner defines `tree_sitter_<name>_external_scanner_create()` with an empty parameter list instead of `(void)` |
| `-Wimplicit-fallthrough` | Luau | The scanner's switch statements fall through without annotations |
| `-Wunused-function` | Luau | The scanner keeps an unused `skip_if` helper |
| `-Wunused-parameter` | Luau | Two token readers ignore `valid_symbols` |

The generated `parser.c` files need no suppressions. Measured with the pinned
Clang 23 on macOS (arm64), like the runtime's.

### Known grammar gaps

Found while writing the parse test; neither is fixed upstream after v0.3.1.
Highlighting keeps the lexical fallback for these lines, and the M3 queries
must tolerate the resulting nodes.

- **Slang, stacked attributes:** tree-sitter-slang 0.3.1 (through the
  tree-sitter-hlsl grammar it extends) allows at most one attribute before a
  function. `[shader("compute")]` followed by `[numthreads(8, 8, 1)]` puts the
  first attribute in an `ERROR` node. Ludus entry points commonly use both.
- **Slang, global generic resources:** a top-level `RWTexture2D<float4> Output;`
  parses without an `ERROR` node, but as a `template_type` followed by an
  expression statement instead of a declaration. The same type parses correctly
  as a parameter.

### Query files

Each archive also carries the grammar's own query files at the same revision.
None are installed yet. The change that adds the highlighting queries under
`config/code-languages/<language>/` records, per query, whether it is taken from
this revision or written by Ludus.

| Grammar | Queries in the pinned archive |
| --- | --- |
| C | `queries/highlights.scm`, `tags.scm` |
| C++ | `queries/highlights.scm`, `injections.scm`, `tags.scm` |
| Slang | None |
| Luau | `helix-queries/{highlights,indents,injections,locals,textobjects}.scm` |
| JSON | `queries/highlights.scm` |
| CMake | `queries/{folds,highlights,indents,injections}.scm` |
| Markdown | `{highlights,injections}.scm` in `tree-sitter-markdown/queries/` and in `tree-sitter-markdown-inline/queries/` |

## LUSL grammar

Not yet added. The Ludus-owned LUSL grammar follows the
[LUSL amendment](../../decisions/0026-tree-sitter-syntax-dependency.md#amendment-the-lusl-grammar).
