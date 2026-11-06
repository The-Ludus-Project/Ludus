// The LUSL language server: `luslc lsp` serves the Language Server Protocol on standard input and
// output over the same Ludus::ShaderLanguage library the compilers use
// (docs/modules/tooling/shader-language.md#language-server).
#pragma once

#include "support.hpp"

#include <ludus/tooling/shader_language/semantic.hpp>

namespace ludus::tools::luslc
{
/// Options of the language server.
struct LanguageServerOptions final
{
    /// Workspace root; empty takes the client's `rootUri` from `initialize`.
    std::string_view Root;
    /// Include directories relative to the root, as for `luslc lower`.
    BorrowedView<const BorrowedView<const char>> IncludeDirectories;
    /// Style setting of the diagnostics.
    language::StyleSetting Style = language::StyleSetting::Strict;
};

/// Serve until `exit`. Returns 0 after `shutdown` then `exit`, 1 otherwise.
[[nodiscard]] int RunLanguageServer(const LanguageServerOptions& options) noexcept;
} // namespace ludus::tools::luslc
