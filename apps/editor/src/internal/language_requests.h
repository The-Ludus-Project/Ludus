#pragma once

#include <ludus/foundation/base/types.h>

#include <QJsonObject>
#include <QLatin1StringView>

namespace ludus::editor
{
/// Every LSP request the generic client may send (code editor M4, CE09). The
/// numeric values index the client's request table and are not persisted.
enum class LanguageRequestKind : foundation::uint8
{
    Completion,
    SignatureHelp,
    Hover,
    Definition,
    Declaration,
    TypeDefinition,
    Implementation,
    References,
    DocumentSymbols,
    SemanticTokens,
    InlayHints,
    FoldingRanges,
    CodeActions,
    PrepareRename,
    Rename,
    CompletionResolve,
    WorkspaceSymbols,
    CodeActionResolve,
    Count,
};

/// The number of request kinds, for tables indexed by LanguageRequestKind.
inline constexpr foundation::usize LANGUAGE_REQUEST_KIND_COUNT =
    static_cast<foundation::usize>(LanguageRequestKind::Count);

/// When the client sends a request. Interactive requests flush pending text
/// changes and go at once. Background requests wait for the synchronization
/// debounce, then go before workspace queries, which always go last.
enum class LanguageRequestPriority : foundation::uint8
{
    Interactive,
    Background,
    Workspace,
};

/// One row of the request table: its LSP method, the server capability that
/// enables it, its priority and whether the client adds the document.
struct LanguageRequestDescriptor final
{
    LanguageRequestKind Kind = LanguageRequestKind::Completion;
    /// The JSON-RPC method, such as "textDocument/completion".
    QLatin1StringView Method;
    /// The `ServerCapabilities` member that must be present and not false.
    QLatin1StringView Capability;
    /// A member of that capability's options object that must also be present
    /// and not false ("resolveProvider"), or empty when the member alone suffices.
    QLatin1StringView Option;
    LanguageRequestPriority Priority = LanguageRequestPriority::Interactive;
    /// True when the client inserts `textDocument` for the current document.
    bool IsDocumentScoped = true;
};

/// The table row for `kind`; valid for the process lifetime.
[[nodiscard]] const LanguageRequestDescriptor& DescribeLanguageRequest(LanguageRequestKind kind) noexcept;

/// True when `capabilities` (an initialize result's `capabilities`) enables `kind`:
/// the member is `true` or an options object, and the option, when the row names
/// one, is `true` or an object.
[[nodiscard]] bool IsLanguageRequestAdvertised(const QJsonObject& capabilities, LanguageRequestKind kind);
} // namespace ludus::editor
