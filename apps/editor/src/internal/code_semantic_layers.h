#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/code_document.h"
#include "internal/code_syntax_service.h"
#include "internal/language_client.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace ludus::editor
{
class CodeEditor;

/// The part of a server's virtual unit that is authored text: UTF-16 offsets
/// [Start, End) in the unit, and the unit line that holds Start.
struct CodeSemanticSource final
{
    foundation::int32 Start = 0;
    foundation::int32 End = 0;
    foundation::int32 FirstLine = 0;
};

/// The syntax role for an LSP semantic token type (LSP 3.17 `SemanticTokenTypes`),
/// such as "function" or "parameter". `isReadonly` comes from the `readonly`
/// modifier and turns a variable into a constant. False, preserving `outRole`,
/// for a type with no role, which keeps its syntax color.
[[nodiscard]] bool ReadSemanticTokenRole(const QString& type, bool isReadonly, CodeSyntaxRole& outRole);

/// Decode `textDocument/semanticTokens/full` data (five integers per token,
/// relative to the previous token) against the server's `legend` into ordered,
/// disjoint tokens of the authored source, with offsets relative to source.Start.
/// Tokens outside the source, spanning lines, overlapping an earlier token or of
/// a type with no role are dropped; at most CodeSemanticLayers::MAX_SEMANTIC_TOKENS are kept.
[[nodiscard]] QList<CodeSyntaxToken> DecodeSemanticTokens(const QJsonArray& data,
                                                          const QJsonObject& legend,
                                                          const QString& unit,
                                                          LanguagePositionEncoding encoding,
                                                          const CodeSemanticSource& source);

/// Decode a `textDocument/foldingRange` result into folds of the authored source,
/// with lines relative to source.FirstLine. `comment`, `region` and `imports`
/// keep their kind; any other kind is a Syntax range. `collapsedText` names the
/// placeholder. Ranges outside the source or shorter than two lines are dropped;
/// at most CodeStructureView::MAX_SERVER_FOLDS are kept.
[[nodiscard]] QList<CodeSyntaxFold>
DecodeFoldingRanges(const QJsonArray& ranges, const QString& unit, const CodeSemanticSource& source);

/// Decode a `textDocument/inlayHint` result into hints of the authored source,
/// ordered by position (relative to source.Start). A label is a string or the
/// joined values of its parts; labels longer than MAX_INLAY_HINT_LABEL_UNITS end
/// in an ellipsis. Hints outside the source, with an empty label or a label that
/// spans lines are dropped; at most CodeSemanticLayers::MAX_INLAY_HINTS are kept.
[[nodiscard]] QList<CodeInlayHint> DecodeInlayHints(const QJsonArray& hints,
                                                    const QString& unit,
                                                    LanguagePositionEncoding encoding,
                                                    const CodeSemanticSource& source);

/// Background semantic layers for one editor (code editor M4, CE10, CE26):
/// semantic tokens over the syntax colors, inlay hints and the server's folding ranges. A
/// layer is requested only when the ready server advertises it, after its
/// virtual unit is synchronized, and a reply is applied only to the document
/// revision and context it was asked for. Edits move the shown layers until the
/// next reply. GUI thread only; the editor and client outlive this object.
class CodeSemanticLayers final
{
public:
    /// Semantic tokens kept per document; a longer result keeps its first tokens.
    static constexpr foundation::int32 MAX_SEMANTIC_TOKENS = 131072;
    /// Inlay hints kept per document; a longer result keeps its first hints.
    static constexpr foundation::int32 MAX_INLAY_HINTS = 4096;
    /// UTF-16 units of an inlay hint label shown before it is shortened.
    static constexpr foundation::int32 MAX_INLAY_HINT_LABEL_UNITS = 64;
    CodeSemanticLayers(CodeEditor& inoutEditor, LanguageClient& inoutClient);
    /// Request every advertised layer for `unit`, which the caller has just passed
    /// to LanguageClient::SetDocument. `identity` and `context` name the draft and
    /// context it describes.
    void Refresh(const CodeDocumentIdentity& identity,
                 foundation::uint64 context,
                 const QString& unit,
                 const CodeSemanticSource& source);
    /// Apply a reply. False when `kind` is not a layer; stale replies are consumed
    /// and dropped.
    bool Receive(foundation::int32 request,
                 LanguageRequestKind kind,
                 const QJsonValue& value,
                 const CodeDocumentIdentity& currentIdentity,
                 foundation::uint64 currentContext);
    /// Cancel pending layers and remove the shown ones.
    void Clear();

private:
    struct Pending final
    {
        foundation::int32 Request = 0;
        CodeDocumentIdentity Identity;
        foundation::uint64 Context = 0;
        QString Unit;
        CodeSemanticSource Source;
    };
    [[nodiscard]] Pending& getPending(LanguageRequestKind kind) noexcept;
    CodeEditor& mEditor;
    LanguageClient& mClient;
    Pending mTokens;
    Pending mFolds;
    Pending mHints;
};
} // namespace ludus::editor
