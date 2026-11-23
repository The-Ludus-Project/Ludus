#include "internal/language_requests.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>

#include <QJsonValue>

// Thanks to Microsoft, Language Server Protocol 3.17, "Server Capabilities" and
// "Language Features": each request is gated on the capability its section names.
// https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/
namespace ludus::editor
{
namespace
{
using namespace foundation;
using Priority = LanguageRequestPriority;
using Kind = LanguageRequestKind;

constexpr StaticArray<LanguageRequestDescriptor, LANGUAGE_REQUEST_KIND_COUNT> REQUESTS{{
    {Kind::Completion,
     QLatin1StringView("textDocument/completion"),
     QLatin1StringView("completionProvider"),
     {},
     Priority::Interactive,
     true},
    {Kind::SignatureHelp,
     QLatin1StringView("textDocument/signatureHelp"),
     QLatin1StringView("signatureHelpProvider"),
     {},
     Priority::Interactive,
     true},
    {Kind::Hover,
     QLatin1StringView("textDocument/hover"),
     QLatin1StringView("hoverProvider"),
     {},
     Priority::Interactive,
     true},
    {Kind::Definition,
     QLatin1StringView("textDocument/definition"),
     QLatin1StringView("definitionProvider"),
     {},
     Priority::Interactive,
     true},
    {Kind::Declaration,
     QLatin1StringView("textDocument/declaration"),
     QLatin1StringView("declarationProvider"),
     {},
     Priority::Interactive,
     true},
    {Kind::TypeDefinition,
     QLatin1StringView("textDocument/typeDefinition"),
     QLatin1StringView("typeDefinitionProvider"),
     {},
     Priority::Interactive,
     true},
    {Kind::Implementation,
     QLatin1StringView("textDocument/implementation"),
     QLatin1StringView("implementationProvider"),
     {},
     Priority::Interactive,
     true},
    {Kind::References,
     QLatin1StringView("textDocument/references"),
     QLatin1StringView("referencesProvider"),
     {},
     Priority::Interactive,
     true},
    {Kind::DocumentSymbols,
     QLatin1StringView("textDocument/documentSymbol"),
     QLatin1StringView("documentSymbolProvider"),
     {},
     Priority::Background,
     true},
    {Kind::SemanticTokens,
     QLatin1StringView("textDocument/semanticTokens/full"),
     QLatin1StringView("semanticTokensProvider"),
     QLatin1StringView("full"),
     Priority::Background,
     true},
    {Kind::InlayHints,
     QLatin1StringView("textDocument/inlayHint"),
     QLatin1StringView("inlayHintProvider"),
     {},
     Priority::Background,
     true},
    {Kind::FoldingRanges,
     QLatin1StringView("textDocument/foldingRange"),
     QLatin1StringView("foldingRangeProvider"),
     {},
     Priority::Background,
     true},
    {Kind::CodeActions,
     QLatin1StringView("textDocument/codeAction"),
     QLatin1StringView("codeActionProvider"),
     {},
     Priority::Interactive,
     true},
    {Kind::PrepareRename,
     QLatin1StringView("textDocument/prepareRename"),
     QLatin1StringView("renameProvider"),
     QLatin1StringView("prepareProvider"),
     Priority::Interactive,
     true},
    {Kind::Rename,
     QLatin1StringView("textDocument/rename"),
     QLatin1StringView("renameProvider"),
     {},
     Priority::Interactive,
     true},
    {Kind::CompletionResolve,
     QLatin1StringView("completionItem/resolve"),
     QLatin1StringView("completionProvider"),
     QLatin1StringView("resolveProvider"),
     Priority::Interactive,
     false},
    {Kind::WorkspaceSymbols,
     QLatin1StringView("workspace/symbol"),
     QLatin1StringView("workspaceSymbolProvider"),
     {},
     Priority::Workspace,
     false},
    {Kind::CodeActionResolve,
     QLatin1StringView("codeAction/resolve"),
     QLatin1StringView("codeActionProvider"),
     QLatin1StringView("resolveProvider"),
     Priority::Interactive,
     false},
}};

bool IsEnabled(const QJsonValue& value)
{
    return value.isObject() == true || value.toBool(false) == true;
}
} // namespace

const LanguageRequestDescriptor& DescribeLanguageRequest(LanguageRequestKind kind) noexcept
{
    const usize index = static_cast<usize>(kind);
    LUDUS_ASSERT(index < LANGUAGE_REQUEST_KIND_COUNT);
    LUDUS_ASSERT(REQUESTS[index].Kind == kind);
    return REQUESTS[index];
}

bool IsLanguageRequestAdvertised(const QJsonObject& capabilities, LanguageRequestKind kind)
{
    const LanguageRequestDescriptor& descriptor = DescribeLanguageRequest(kind);
    const QJsonValue capability = capabilities.value(descriptor.Capability);
    if (IsEnabled(capability) == false)
    {
        return false;
    }
    return descriptor.Option.isEmpty() == true || IsEnabled(capability.toObject().value(descriptor.Option)) == true;
}
} // namespace ludus::editor
