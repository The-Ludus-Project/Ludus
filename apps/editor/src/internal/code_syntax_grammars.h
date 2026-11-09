#pragma once

// The upstream tree-sitter grammars that ludus_editor_tree_sitter compiles for the syntax service
// (docs/decisions/0026-tree-sitter-syntax-dependency.md). Their pins, licenses and query provenance
// are recorded in docs/architecture/code-editor-evidence/tree-sitter-manifest.md. Only the syntax
// worker may use these; tree-sitter types never cross the editor's seam values.

#include <tree_sitter/api.h>

#ifdef __cplusplus
extern "C" {
#endif
/// The C grammar (tree-sitter/tree-sitter-c); a static table that lives for the whole process.
const TSLanguage* tree_sitter_c(void);
/// The C++ grammar (tree-sitter/tree-sitter-cpp); a static table that lives for the whole process.
const TSLanguage* tree_sitter_cpp(void);
/// The Slang grammar (tree-sitter-grammars/tree-sitter-slang); a static table that lives for the
/// whole process.
const TSLanguage* tree_sitter_slang(void);
/// The Luau grammar (polychromatist/tree-sitter-luau); a static table that lives for the whole
/// process.
const TSLanguage* tree_sitter_luau(void);
/// The JSON grammar (tree-sitter/tree-sitter-json); a static table that lives for the whole process.
const TSLanguage* tree_sitter_json(void);
/// The CMake grammar (uyha/tree-sitter-cmake); a static table that lives for the whole process.
const TSLanguage* tree_sitter_cmake(void);
/// The Markdown block grammar (tree-sitter-grammars/tree-sitter-markdown). Inline content parses
/// separately with tree_sitter_markdown_inline() over the block tree's inline ranges.
const TSLanguage* tree_sitter_markdown(void);
/// The Markdown inline grammar (tree-sitter-grammars/tree-sitter-markdown) for the inline ranges of
/// a tree_sitter_markdown() tree; a static table that lives for the whole process.
const TSLanguage* tree_sitter_markdown_inline(void);
#ifdef __cplusplus
}
#endif
