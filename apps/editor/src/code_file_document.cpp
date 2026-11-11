#include "internal/code_file_document.h"
#include "internal/editor_file_write.h"

#include "internal/code_edit_set.h"
#include "internal/code_editor_config.h"
#include "internal/code_text_detect.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QObject>

#include <utility>

// The file document host (CE03, CE04, CE22). Loading runs on the background
// pool through LoadCodeFile; the host itself is a GUI-thread value over the
// loaded bytes, with its own bounded history, atomic save and conflict check.

namespace ludus::editor
{
using foundation::BorrowedView;
using foundation::uint64;
using foundation::usize;

namespace
{
/// The largest file the host will read into memory at all, matching the refusal
/// threshold so a file beyond it never reaches detection.
constexpr qint64 MAX_LOADABLE_BYTES = static_cast<qint64>(CODE_FILE_READ_ONLY_LIMIT_BYTES);

[[nodiscard]] QByteArray hashBytes(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

[[nodiscard]] QString describeReadOnly(const CodeTextProfile& profile)
{
    switch (profile.Encoding)
    {
        case CodeTextEncoding::InvalidUtf8:
            return QObject::tr("This file is not valid UTF-8, so it opened read-only to avoid corrupting it. "
                               "Use Open Externally to edit it in another editor.");
        default:
            break;
    }
    switch (profile.SizeMode)
    {
        case CodeFileSizeMode::VeryLarge:
            return QObject::tr("This file is very large, so it opened read-only. Use Open Externally to edit it.");
        case CodeFileSizeMode::Large:
            return QObject::tr("This file is large, so syntax, language servers and some features are off. "
                               "Editing and saving still work.");
        default:
            break;
    }
    return QString();
}
} // namespace

CodeFileLoadResult LoadCodeFile(const QString& absolutePath) noexcept
{
    CodeFileLoadResult result;
    const QFileInfo info(absolutePath);
    result.CanonicalPath = info.absoluteFilePath();
    QFile file(absolutePath);
    const qint64 size = info.size();
    if (size > MAX_LOADABLE_BYTES)
    {
        result.Status = CodeFileLoadStatus::Failed;
        result.Explanation = QObject::tr("This file is too large to open in the editor. Use Open Externally.");
        return result;
    }
    if (file.open(QIODevice::ReadOnly) == false)
    {
        result.Status = CodeFileLoadStatus::Failed;
        result.Explanation = QObject::tr("The file could not be read: %1").arg(file.errorString());
        return result;
    }
    const QByteArray raw = file.readAll();
    file.close();

    CodeTextProfile profile;
    const usize bomLength = DetectCodeTextProfile(raw.constData(), static_cast<usize>(raw.size()), profile);
    if (profile.SizeMode == CodeFileSizeMode::Refused)
    {
        result.Status = CodeFileLoadStatus::Failed;
        result.Explanation = QObject::tr("This file is too large to open in the editor. Use Open Externally.");
        return result;
    }

    // Store the editable bytes without the BOM; the save path re-emits it.
    result.Bytes = raw.mid(static_cast<qsizetype>(bomLength));

    // EditorConfig overrides detected indentation and line endings (CE04). It is
    // read on this background thread, never on the UI thread during typing.
    if (profile.Encoding != CodeTextEncoding::InvalidUtf8)
    {
        const CodeEditorConfigSettings config = ResolveEditorConfig(result.CanonicalPath);
        ApplyEditorConfig(config, profile);
        // An EditorConfig charset that the host cannot edit keeps the file
        // read-only rather than silently re-encoding it on save.
        if (config.HasCharset == true && config.IsCharsetUtf8 == false)
        {
            profile.IsReadOnly = true;
        }
    }

    result.Profile = profile;
    result.FileSize = size;
    result.ModifiedMsecsSinceEpoch = info.lastModified().toMSecsSinceEpoch();
    result.ContentHash = hashBytes(raw);
    result.Explanation = describeReadOnly(profile);
    result.Status = profile.IsReadOnly == true ? CodeFileLoadStatus::ReadOnly : CodeFileLoadStatus::Ok;
    return result;
}

CodeFileDocument::CodeFileDocument(CodeFileLoadResult&& result, uint64 epoch)
    : mCanonicalPath(std::move(result.CanonicalPath)), mSaved(result.Bytes), mDraft(std::move(result.Bytes)),
      mProfile(result.Profile), mExplanation(std::move(result.Explanation)), mIsReadOnly(result.Profile.IsReadOnly),
      mEpoch(epoch), mDiskSize(result.FileSize), mDiskModifiedMsecs(result.ModifiedMsecsSinceEpoch),
      mDiskHash(std::move(result.ContentHash))
{
}

uint64 CodeFileDocument::revision() const noexcept
{
    return mHistory.Revision();
}

CodeDocumentSnapshot CodeFileDocument::GetSnapshot() const noexcept
{
    return {{mEpoch, 1, mHistory.Revision()},
            {mDraft.constData(), static_cast<usize>(mDraft.size())},
            CODE_FILE_NORMAL_LIMIT_BYTES,
            IsDirty()};
}

CodeEditStatus
CodeFileDocument::ApplyEdits(const CodeDocumentIdentity& expected, const CodeEditSet& edits, bool shouldMerge)
{
    if (mIsReadOnly == true)
    {
        return CodeEditStatus::Unavailable;
    }
    if (expected != GetSnapshot().Identity)
    {
        return CodeEditStatus::Conflict;
    }
    return applyByteEdits(edits, shouldMerge);
}

CodeEditStatus CodeFileDocument::applyByteEdits(const CodeEditSet& edits, bool shouldMerge)
{
    const BorrowedView<const char> original{mDraft.constData(), static_cast<usize>(mDraft.size())};
    QByteArray candidate;
    const CodeEditStatus status = ValidateAndApplyEditSet(original, CODE_FILE_NORMAL_LIMIT_BYTES, edits, candidate);
    if (status != CodeEditStatus::Ok)
    {
        return status;
    }
    if (candidate == mDraft)
    {
        // A no-op set (or a set that cancels out) records no history, matching the
        // single-edit path. Selections are left for the caller to restore.
        return CodeEditStatus::Ok;
    }
    const CodeHistoryState previous = { .Text = mDraft, .Selections = CopyCodeSelectionSet(edits.Before) };
    const CodeHistoryState next = { .Text = candidate, .Selections = CopyCodeSelectionSet(edits.After) };
    const bool canMerge = shouldMerge == true && mHasMergeableGroup == true;
    if (mHistory.Commit(previous, next, canMerge) == false)
    {
        // A full history (revision saturated) is still allowed to edit the
        // draft; it simply cannot record another undo step.
        mDraft = std::move(candidate);
        mDraftSelections = next.Selections;
        mHasMergeableGroup = false;
        return CodeEditStatus::Ok;
    }
    mDraft = std::move(candidate);
    mDraftSelections = next.Selections;
    mHasMergeableGroup = true;
    return CodeEditStatus::Ok;
}

void CodeFileDocument::BreakUndoGroup() noexcept
{
    mHasMergeableGroup = false;
    mHistory.BreakGroup();
}

bool CodeFileDocument::CanUndo() const noexcept
{
    return mHistory.CanUndo();
}

bool CodeFileDocument::CanRedo() const noexcept
{
    return mHistory.CanRedo();
}

bool CodeFileDocument::Undo(QList<CodeTextSelection>& outSelections)
{
    BreakUndoGroup();
    CodeHistoryState state = { .Text = mDraft, .Selections = mDraftSelections };
    if (mHistory.Undo(state) == false)
    {
        return false;
    }
    mDraft = state.Text;
    mDraftSelections = state.Selections;
    outSelections = state.Selections;
    return true;
}

bool CodeFileDocument::Redo(QList<CodeTextSelection>& outSelections)
{
    BreakUndoGroup();
    CodeHistoryState state = { .Text = mDraft, .Selections = mDraftSelections };
    if (mHistory.Redo(state) == false)
    {
        return false;
    }
    mDraft = state.Text;
    mDraftSelections = state.Selections;
    outSelections = state.Selections;
    return true;
}

void CodeFileDocument::SetProfileLineEnding(CodeLineEnding ending) noexcept
{
    mProfile.LineEnding = ending;
    mProfile.LineEndingSource = CodeTextProfileSource::EditorConfig;
}

bool CodeFileDocument::Save(QString& outError)
{
    if (mIsReadOnly == true)
    {
        outError = QObject::tr("This document is read-only and cannot be saved.");
        return false;
    }
    QByteArray output;
    if (mProfile.Encoding == CodeTextEncoding::Utf8Bom)
    {
        output.append(CODE_UTF8_BOM_0);
        output.append(CODE_UTF8_BOM_1);
        output.append(CODE_UTF8_BOM_2);
    }
    output.append(mDraft);

    QString error;
    if (WriteEditorFileAtomically(mCanonicalPath, output, error) == false)
    {
        outError = QObject::tr("The file could not be saved: %1. Close other programs using it or check its "
                               "permissions, then save again.")
                       .arg(error);
        return false;
    }
    mSaved = mDraft;
    captureDiskState();
    BreakUndoGroup();
    return true;
}

bool CodeFileDocument::ReloadFromDisk(QString& outError)
{
    const CodeFileLoadResult reloaded = LoadCodeFile(mCanonicalPath);
    if (reloaded.Status == CodeFileLoadStatus::Failed)
    {
        outError = reloaded.Explanation;
        return false;
    }
    mSaved = reloaded.Bytes;
    mDraft = reloaded.Bytes;
    mProfile = reloaded.Profile;
    mIsReadOnly = reloaded.Profile.IsReadOnly;
    mExplanation = reloaded.Explanation;
    mDiskSize = reloaded.FileSize;
    mDiskModifiedMsecs = reloaded.ModifiedMsecsSinceEpoch;
    mDiskHash = reloaded.ContentHash;
    mDraftSelections.clear();
    mHistory.BreakGroup();
    mHasMergeableGroup = false;
    return true;
}

bool CodeFileDocument::HasExternalChange() const
{
    const QFileInfo info(mCanonicalPath);
    if (info.exists() == false)
    {
        // A deleted file is an external change the caller resolves.
        return mDiskSize != 0;
    }
    if (info.size() != mDiskSize || info.lastModified().toMSecsSinceEpoch() != mDiskModifiedMsecs)
    {
        // Size or mtime moved; confirm with a hash so a touch is not a conflict.
        QFile file(mCanonicalPath);
        if (file.open(QIODevice::ReadOnly) == false)
        {
            return true;
        }
        const QByteArray bytes = file.readAll();
        return hashBytes(bytes) != mDiskHash;
    }
    return false;
}

void CodeFileDocument::AdoptAsBaseline()
{
    // Keep Mine: the on-disk content becomes the new baseline for conflict
    // detection, but the user's draft is preserved so it stays dirty and can be
    // saved over the external change.
    captureDiskState();
}

QByteArray CodeFileDocument::GetSavedHash() const
{
    return hashBytes(mSaved);
}

void CodeFileDocument::DiscardDraft() noexcept
{
    mDraft = mSaved;
    mDraftSelections.clear();
    mHistory.Reset();
    mHasMergeableGroup = false;
}

CodeRecoveredDraftStatus CodeFileDocument::RestoreRecoveredDraft(const QByteArray& expectedSavedHash,
                                                                 const QByteArray& draftBytes)
{
    if (mIsReadOnly == true || draftBytes == mDraft)
    {
        return CodeRecoveredDraftStatus::Unchanged;
    }
    // Re-anchor the recovered draft on top of the current baseline. A recovered
    // buffer carries no undo steps: history belongs to the crashed session and
    // is intentionally dropped so Undo cannot reach stale text. A draft edited
    // over different on-disk content is still adopted, never dropped; the caller
    // shows the conflict so the user decides between the draft and the file.
    mDraft = draftBytes;
    mDraftSelections.clear();
    mHistory.Reset();
    mHasMergeableGroup = false;
    return hashBytes(mSaved) == expectedSavedHash ? CodeRecoveredDraftStatus::Restored
                                                  : CodeRecoveredDraftStatus::RestoredOverChangedFile;
}

void CodeFileDocument::captureDiskState()
{
    const QFileInfo info(mCanonicalPath);
    QByteArray onDisk;
    QFile file(mCanonicalPath);
    if (file.open(QIODevice::ReadOnly) == true)
    {
        onDisk = file.readAll();
    }
    mDiskSize = info.size();
    mDiskModifiedMsecs = info.lastModified().toMSecsSinceEpoch();
    mDiskHash = hashBytes(onDisk);
}
} // namespace ludus::editor
