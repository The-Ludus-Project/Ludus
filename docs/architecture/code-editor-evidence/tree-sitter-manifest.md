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
package as `licenses/tree-sitter.txt`, and `--check-package` requires it.

## Grammars

Not yet pinned. Each grammar of ADR 0026 is added here with its tag, commit,
archive hash, license and query provenance in the change that builds it. The
Ludus-owned LUSL grammar follows the
[LUSL amendment](../../decisions/0026-tree-sitter-syntax-dependency.md#amendment-the-lusl-grammar).
