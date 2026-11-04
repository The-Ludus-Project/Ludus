#pragma once

#include "internal/code_document.h"
#include "internal/code_text_profile.h"
#include "internal/document_history.h"

#include <ludus/foundation/base/types.h>

#include <QByteArray>
#include <QString>

namespace ludus::editor
{
/// How a load attempt ended. The file host is created only after a load
/// succeeds or opens read-only; a hard failure produces no host (CE03, CE04).
enum class CodeFileLoadStatus : foundation::uint8
{
    /// The file loaded as editable UTF-8.
    Ok,
    /// The file loaded but is read-only (invalid UTF-8, or a size threshold).
    ReadOnly,
    /// The file could not be read, or is too large to open at all.
    Failed,
};

/// The result of loading a file on the background pool: the editable bytes (BOM
/// stripped), the detected-then-EditorConfig profile, and an explanation when
/// the document is read-only or failed. Qt-free apart from the path/message
/// carriers, so the loader can run off the UI thread.
struct CodeFileLoadResult final
{
    CodeFileLoadStatus Status = CodeFileLoadStatus::Failed;
    QString CanonicalPath;
    /// Editable UTF-8 with any BOM removed; LF/CRLF/mixed endings are preserved.
    QByteArray Bytes;
    CodeTextProfile Profile;
    /// A reader-facing reason shown in the read-only or failure banner. It names
    /// the cause and, where one applies, the fix (for example Open Externally).
    QString Explanation;
    /// Size, modification time and a content hash captured at load, so the
    /// watcher can tell an external change from the editor's own save.
    qint64 FileSize = 0;
    qint64 ModifiedMsecsSinceEpoch = 0;
    QByteArray ContentHash;
};

/// Load a project text file on the calling (background) thread: read the bytes,
/// detect encoding/BOM/line endings/indentation, resolve .editorconfig, and
/// classify the size mode. Never executes project code and never touches a
/// widget. The host is constructed from the result on the UI thread.
[[nodiscard]] CodeFileLoadResult LoadCodeFile(const QString& absolutePath) noexcept;

/// The outcome of CodeFileDocument::RestoreRecoveredDraft.
enum class CodeRecoveredDraftStatus : foundation::uint8
{
    /// Nothing changed: the host is read-only or already holds the draft.
    Unchanged,
    /// The draft was adopted over the same on-disk content it was edited from.
    Restored,
    /// The draft was adopted, but the file changed on disk after it was journaled.
    RestoredOverChangedFile,
};

/// Authoritative host for one ordinary project text file. It owns the UTF-8
/// draft, a bounded undo history, the saved baseline and the text profile, and
/// implements atomic save (CE03) and external-change conflict handling. It knows
/// nothing about widgets, tokens or language servers, matching the seam's
/// contract. GUI-thread use only; loading is done by LoadCodeFile beforehand.
class CodeFileDocument final : public CodeDocumentHost
{
public:
    /// Construct from a completed load. The identity epoch is the project epoch,
    /// so a project reload invalidates outstanding edits. A read-only result
    /// produces a host whose ApplyEdit always returns Unavailable.
    CodeFileDocument(CodeFileLoadResult&& result, foundation::uint64 epoch);

    [[nodiscard]] CodeDocumentSnapshot GetSnapshot() const noexcept override;
    [[nodiscard]] CodeEditStatus ApplyEdit(const CodeDocumentIdentity& expected,
                                           const CodeTextEdit& edit,
                                           CodeTextSelection before,
                                           CodeTextSelection after,
                                           bool shouldMerge) override;
    void BreakUndoGroup() noexcept override;
    [[nodiscard]] bool CanUndo() const noexcept override;
    [[nodiscard]] bool CanRedo() const noexcept override;

    /// Undo/redo one step, returning whether the draft changed.
    [[nodiscard]] bool Undo();
    [[nodiscard]] bool Redo();

    [[nodiscard]] const QString& GetCanonicalPath() const noexcept
    {
        return mCanonicalPath;
    }
    [[nodiscard]] const CodeTextProfile& GetProfile() const noexcept
    {
        return mProfile;
    }
    [[nodiscard]] bool IsReadOnly() const noexcept
    {
        return mIsReadOnly;
    }
    [[nodiscard]] const QString& GetExplanation() const noexcept
    {
        return mExplanation;
    }
    [[nodiscard]] bool IsDirty() const noexcept
    {
        return mDraft != mSaved;
    }

    /// Override indentation and/or line endings from resolved EditorConfig. The
    /// width used to save new lines follows the profile after this call.
    void SetProfileLineEnding(CodeLineEnding ending) noexcept;

    /// Atomically write the current draft to disk with QSaveFile, re-emitting a
    /// byte-order mark when the file had one and appending a final newline when
    /// EditorConfig asks for it (CE03). A failed write keeps the buffer dirty and
    /// reports the cause; the draft is never lost. Returns true on success.
    [[nodiscard]] bool Save(QString& outError);

    /// Reload the on-disk bytes into a clean buffer, discarding no dirty draft:
    /// the caller must only invoke this when the buffer is clean or the user
    /// chose Reload/Keep Theirs. Returns true when the reload succeeded.
    [[nodiscard]] bool ReloadFromDisk(QString& outError);

    /// Whether the file on disk differs from what this host last read or wrote,
    /// comparing size, modification time and a content hash so the editor's own
    /// save never reports a conflict (CE03). A null result means "no change".
    [[nodiscard]] bool HasExternalChange() const;

    /// Replace the draft with the caller's bytes as one undoable edit, used by
    /// Keep Mine to re-anchor against the new on-disk baseline without losing the
    /// user's text. The baseline becomes the current on-disk content.
    void AdoptAsBaseline();

    /// SHA-256 (raw bytes, not hex) of the current saved baseline, so the
    /// recovery journal records which on-disk content a draft was edited over.
    [[nodiscard]] QByteArray GetSavedHash() const;

    /// Discard the unsaved draft: the draft returns to the saved baseline and
    /// history is cleared, so Undo cannot bring the discarded text back. Used by
    /// Don't Save; it touches no file and cannot fail. Views must re-synchronize.
    void DiscardDraft() noexcept;

    /// Restore an unsaved draft recovered from the journal (CE03). A read-only
    /// host, or a draft identical to the current buffer, is left unchanged. The
    /// draft is otherwise always adopted, so recovered work is never dropped: when
    /// `expectedSavedHash` no longer matches the on-disk baseline the result is
    /// RestoredOverChangedFile, and the caller offers Compare, Keep Mine and
    /// Reload. A restored draft clears history and makes the host dirty.
    [[nodiscard]] CodeRecoveredDraftStatus RestoreRecoveredDraft(const QByteArray& expectedSavedHash,
                                                                 const QByteArray& draftBytes);

private:
    /// Apply one validated byte edit to the draft, pushing an undo entry. The
    /// validation mirrors the render-graph host: order, UTF-8 scalar boundaries
    /// and capacity are all checked before any mutation.
    [[nodiscard]] CodeEditStatus
    applyByteEdit(const CodeTextEdit& edit, CodeTextSelection before, CodeTextSelection after, bool shouldMerge);
    [[nodiscard]] foundation::uint64 revision() const noexcept;
    void captureDiskState();

    QString mCanonicalPath;
    QByteArray mSaved; ///< The last bytes read from or written to disk (no BOM).
    QByteArray mDraft; ///< The current editable bytes (no BOM).
    CodeTextProfile mProfile;
    QString mExplanation;
    bool mIsReadOnly = false;
    foundation::uint64 mEpoch = 0;
    DocumentHistory<QByteArray> mHistory;
    bool mHasMergeableGroup = false;
    qint64 mDiskSize = 0;
    qint64 mDiskModifiedMsecs = 0;
    QByteArray mDiskHash;
};
} // namespace ludus::editor
