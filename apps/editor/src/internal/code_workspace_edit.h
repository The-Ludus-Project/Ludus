#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/code_document.h"
#include "internal/code_navigation_results.h"
#include "internal/language_positions.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace ludus::editor
{
/// One replacement of a workspace edit: UTF-16 offsets [Start, End) in the
/// text its file change names.
struct CodeWorkspaceTextEdit final
{
    foundation::int32 Start = 0;
    foundation::int32 End = 0;
    QString NewText;
    [[nodiscard]] bool operator==(const CodeWorkspaceTextEdit& other) const = default;
};

/// The edits of a workspace edit for one file (code editor M4, CE11).
struct CodeWorkspaceFileChange final
{
    /// The package file, or empty for the source being edited.
    QString File;
    /// The text the offsets refer to, as the language server saw it.
    QString Text;
    /// Ascending and non-overlapping.
    QList<CodeWorkspaceTextEdit> Edits;
};

/// Edits kept per workspace edit; a larger one is rejected before anything applies.
inline constexpr foundation::int32 CODE_MAX_WORKSPACE_EDITS = 65536;

/// What a workspace edit is read against: the server's virtual unit for the
/// edited source and the exact inventory of other captured sources.
struct CodeWorkspaceEditContext final
{
    QString Uri;
    QString Unit;
    /// Authored source offsets [SourceStart, SourceEnd) in Unit.
    foundation::int32 SourceStart = 0;
    foundation::int32 SourceEnd = 0;
    /// The document version the server last received; a versioned edit for Uri must match.
    foundation::int32 SentVersion = 0;
    CodeNavigationSources Sources;
    LanguagePositionEncoding Encoding = LanguagePositionEncoding::Utf16;
    /// Whether edits may reach package files other than the edited source.
    bool CanEditOtherFiles = false;
};

/// Read an LSP `WorkspaceEdit` (`changes` or `documentChanges`) into one change
/// per file. The whole edit is rejected, with the reason and its repair in
/// outMessage and outChanges preserved, when any part would touch generated or
/// read-only text, an unknown URI, a file other than the edited source while
/// CanEditOtherFiles is false, or an earlier document version; when it creates,
/// renames or deletes files; when ranges are invalid or overlap; or beyond
/// CODE_MAX_WORKSPACE_EDIT_FILES files or CODE_MAX_WORKSPACE_EDITS edits.
[[nodiscard]] bool ReadCodeWorkspaceEdit(const QJsonObject& edit,
                                         const CodeWorkspaceEditContext& context,
                                         QList<CodeWorkspaceFileChange>& outChanges,
                                         QString& outMessage);

/// `text` with `edits` (ascending, non-overlapping) applied.
[[nodiscard]] QString ApplyCodeWorkspaceTextEdits(const QString& text, const QList<CodeWorkspaceTextEdit>& edits);

/// Apply `edits`, whose offsets refer to `draft` decoded with DecodeCodeText, to
/// the UTF-8 draft itself, keeping its CRLF line endings. False, preserving
/// outResult, when an offset is not a boundary in the draft.
[[nodiscard]] bool
ApplyCodeWorkspaceEditBytes(const QByteArray& draft, const QList<CodeWorkspaceTextEdit>& edits, QByteArray& outResult);

/// Where `position` in a change's text lies after its edits: a position inside a
/// replaced range moves to that replacement's start.
[[nodiscard]] foundation::int32 MapCodeWorkspacePosition(const QList<CodeWorkspaceTextEdit>& edits,
                                                         foundation::int32 position);

class CodeDocumentController;
class CodeWorkspaceEditHost;

/// Apply validated `changes` for the source bound to `inoutDocument` as one undo
/// step named `name`: through `hostOrNull` when it is set, otherwise only the
/// edited source's change (a change for another file needs a host). `caret` is
/// the edited source's caret before the edits; it follows them.
[[nodiscard]] CodeEditStatus ApplyCodeWorkspaceChanges(CodeDocumentController& inoutDocument,
                                                       CodeWorkspaceEditHost* hostOrNull,
                                                       const QList<CodeWorkspaceFileChange>& changes,
                                                       foundation::int32 caret,
                                                       const char* name);

/// Applies a validated workspace edit as one transaction (CE11): every file
/// changes, or none does. Implemented by the owner of the files' drafts.
class CodeWorkspaceEditHost
{
public:
    virtual ~CodeWorkspaceEditHost() = default;
    /// Apply `changes` as one undo step named `name` if the edited source still
    /// has identity `expected` and every change's text is still its file's draft.
    /// `caret` is the edited source's caret, in its text after the edits. Nothing
    /// changes on failure.
    [[nodiscard]] virtual CodeEditStatus ApplyWorkspaceEdit(const CodeDocumentIdentity& expected,
                                                            const QList<CodeWorkspaceFileChange>& changes,
                                                            foundation::int32 caret,
                                                            const char* name) = 0;
};
} // namespace ludus::editor
