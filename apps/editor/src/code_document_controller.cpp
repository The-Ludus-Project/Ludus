#include "internal/code_document_controller.h"

#include <ludus/foundation/base/core.h>

#include "internal/code_text.h"

#include <QScrollBar>
#include <QStringEncoder>
#include <QTextCursor>
#include <QTextDocument>

// Thanks to The Qt Company, "QTextDocument", signals contentsChange and
// contentsChanged, and "QTextCursor", beginEditBlock/endEditBlock: observe
// committed input while retaining Qt's layout, clipboard and IME implementation.
// https://doc.qt.io/qt-6/qtextdocument.html#contentsChange
// https://doc.qt.io/qt-6/qtextcursor.html#beginEditBlock
namespace ludus::editor
{
namespace
{
using namespace foundation;
constexpr int64 TYPING_GROUP_MILLISECONDS = 1000;

QString DescribeCodeEditStatus(CodeEditStatus status)
{
    switch (status)
    {
        case CodeEditStatus::Capacity:
            return QStringLiteral("Text edit exceeds the document limit; the previous source was retained.");
        case CodeEditStatus::Conflict:
            return QStringLiteral("The document changed during this edit; its current source was retained.");
        case CodeEditStatus::InvalidText:
            return QStringLiteral("Text edit is not valid UTF-8 or splits a Unicode character; source was retained.");
        case CodeEditStatus::Unavailable:
            return QStringLiteral("This source is no longer available for editing.");
        case CodeEditStatus::Ok:
            return {};
    }
    return {};
}

QByteArray CopyCodeText(BorrowedView<const char> bytes)
{
    return QByteArray(bytes.GetDataOrNull(), static_cast<isize>(bytes.GetSize()));
}
} // namespace

CodeDocumentController::CodeDocumentController(CodeEditor& inoutEditor, QObject* parentOrNull)
    : QObject(parentOrNull), mEditor(inoutEditor)
{
    connect(&mEditor, &CodeEditor::EditStarted, this, &CodeDocumentController::beginOperation);
    connect(&mEditor, &CodeEditor::EditFinished, this, &CodeDocumentController::endOperation);
    connect(&mEditor, &CodeEditor::EditBoundary, this, &CodeDocumentController::BreakUndoGroup);
    connect(mEditor.document(), &QTextDocument::contentsChange, this, &CodeDocumentController::captureChange);
    connect(mEditor.document(), &QTextDocument::contentsChanged, this, [this]() {
        if (mOperationDepth == 0)
        {
            commitPending();
        }
    });
    connect(&mEditor, &CodeEditor::cursorPositionChanged, this, &CodeDocumentController::captureSelectionChange);
    connect(&mEditor, &CodeEditor::selectionChanged, this, &CodeDocumentController::captureSelectionChange);
}

void CodeDocumentController::captureSelectionChange()
{
    if (mIsSynchronizing == false && mHasPending == false && mOperationDepth == 0 && IsBound() == true)
    {
        const CodeTextSelection current = CaptureSelection();
        if (current.AnchorBytes != mLastSelection.AnchorBytes || current.CaretBytes != mLastSelection.CaretBytes)
        {
            BreakUndoGroup();
        }
        mLastSelection = current;
    }
}

CodeDocumentController::~CodeDocumentController()
{
    mEditor.SetDocumentHistory(false);
}

bool CodeDocumentController::IsBound() const noexcept
{
    return mHostOrNull != nullptr && mIdentity.Document != 0;
}

void CodeDocumentController::Bind(CodeDocumentHost* hostOrNull)
{
    if (mHostOrNull != hostOrNull)
    {
        BreakUndoGroup();
        const bool wasBound = mIdentity.Document != 0;
        mHostOrNull = hostOrNull;
        mIdentity = {};
        if (wasBound == true)
        {
            Q_EMIT BindingChanged();
        }
        mEditor.SetDocumentHistory(hostOrNull != nullptr);
    }
    Synchronize();
}

CodeTextCapture CodeDocumentController::Capture() const
{
    if (mHostOrNull == nullptr)
    {
        return {};
    }
    const CodeDocumentSnapshot snapshot = mHostOrNull->GetSnapshot();
    return {snapshot.Identity, CopyCodeText(snapshot.Text), snapshot.LimitBytes};
}

CodeTextSelection CodeDocumentController::CaptureSelection() const
{
    const QByteArray& bytes = mHasPending == true ? mPendingBytes : mBytes;
    CodeTextSelection selection;
    const QTextCursor cursor = mEditor.textCursor();
    static_cast<void>(GetCodeByteOffset(bytes, cursor.anchor(), selection.AnchorBytes));
    static_cast<void>(GetCodeByteOffset(bytes, cursor.position(), selection.CaretBytes));
    return selection;
}

void CodeDocumentController::RestoreSelection(CodeTextSelection selection)
{
    int32 anchor = 0;
    int32 caret = 0;
    if (GetCodeCursorPosition(mBytes, selection.AnchorBytes, anchor) == false ||
        GetCodeCursorPosition(mBytes, selection.CaretBytes, caret) == false)
    {
        return;
    }
    const bool wasSynchronizing = mIsSynchronizing;
    mIsSynchronizing = true;
    QTextCursor cursor(mEditor.document());
    cursor.setPosition(anchor);
    cursor.setPosition(caret, QTextCursor::KeepAnchor);
    mEditor.setTextCursor(cursor);
    mLastSelection = selection;
    mIsSynchronizing = wasSynchronizing;
}

void CodeDocumentController::Synchronize(bool shouldRestoreView)
{
    if (mIsSynchronizing == true)
    {
        return;
    }
    if (mHostOrNull == nullptr)
    {
        const bool wasBound = mIdentity.Document != 0;
        mIdentity = {};
        mBytes.clear();
        mHasPending = false;
        mPendingBytes.clear();
        if (wasBound == true)
        {
            Q_EMIT BindingChanged();
        }
        return;
    }
    const CodeDocumentSnapshot snapshot = mHostOrNull->GetSnapshot();
    const bool isNewBinding =
        snapshot.Identity.Epoch != mIdentity.Epoch || snapshot.Identity.Document != mIdentity.Document;
    const QByteArray bytes = CopyCodeText(snapshot.Text);
    if (shouldRestoreView == true || isNewBinding == true || bytes != mBytes)
    {
        QString text;
        if (DecodeCodeText(bytes, text) == false)
        {
            mEditor.setReadOnly(true);
            Q_EMIT Failed(QStringLiteral("This source is not valid UTF-8; fix its encoding before editing."));
            return;
        }
        mIsSynchronizing = true;
        const int32 vertical = mEditor.verticalScrollBar()->value();
        const int32 horizontal = mEditor.horizontalScrollBar()->value();
        const QTextCursor current = mEditor.textCursor();
        if (isNewBinding == true)
        {
            mEditor.setPlainText(text);
        }
        else
        {
            const int32 anchor = current.anchor();
            const int32 caret = current.position();
            QString old = mEditor.document()->toRawText();
            old.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
            int32 prefix = 0;
            while (prefix < old.size() && prefix < text.size() && old.at(prefix) == text.at(prefix))
            {
                ++prefix;
            }
            if (prefix > 0 && prefix < old.size() && old.at(prefix).isLowSurrogate() == true)
            {
                --prefix;
            }
            int32 suffix = 0;
            while (suffix < old.size() - prefix && suffix < text.size() - prefix &&
                   old.at(old.size() - suffix - 1) == text.at(text.size() - suffix - 1))
            {
                ++suffix;
            }
            if (suffix > 0 && suffix < text.size() && text.at(text.size() - suffix).isLowSurrogate() == true)
            {
                --suffix;
            }
            if (old != text)
            {
                QTextCursor edit(mEditor.document());
                edit.setPosition(prefix);
                edit.setPosition(static_cast<int32>(old.size()) - suffix, QTextCursor::KeepAnchor);
                edit.insertText(text.mid(prefix, text.size() - prefix - suffix));
            }
            const auto clampPosition = [&text](int32 position) {
                int32 clamped = qMin(position, static_cast<int32>(text.size()));
                if (clamped > 0 && clamped < text.size() && text.at(clamped).isLowSurrogate() == true)
                {
                    --clamped;
                }
                return clamped;
            };
            QTextCursor restored(mEditor.document());
            restored.setPosition(clampPosition(anchor));
            restored.setPosition(clampPosition(caret), QTextCursor::KeepAnchor);
            mEditor.setTextCursor(restored);
            mEditor.verticalScrollBar()->setValue(vertical);
            mEditor.horizontalScrollBar()->setValue(horizontal);
        }
        mEditor.document()->setModified(false);
        mEditor.setReadOnly(false);
        mIsSynchronizing = false;
    }
    mBytes = bytes;
    mHasCrLf = bytes.contains("\r\n");
    mIdentity = snapshot.Identity;
    mHasPending = false;
    mPendingBytes.clear();
    if (isNewBinding == true)
    {
        BreakUndoGroup();
        Q_EMIT BindingChanged();
    }
    mLastSelection = CaptureSelection();
    updateHistoryAvailability();
}

void CodeDocumentController::updateHistoryAvailability()
{
    mEditor.SetHistoryAvailability(mHostOrNull != nullptr && mHostOrNull->CanUndo() == true,
                                   mHostOrNull != nullptr && mHostOrNull->CanRedo() == true);
}

void CodeDocumentController::BreakUndoGroup()
{
    mHasTextGroup = false;
    if (mHostOrNull != nullptr)
    {
        mHostOrNull->BreakUndoGroup();
    }
}

void CodeDocumentController::beginOperation(CodeEditKind kind)
{
    if (mOperationDepth == 0)
    {
        mKind = kind;
        mBefore = CaptureSelection();
    }
    else if (kind == CodeEditKind::Command)
    {
        mKind = CodeEditKind::Command;
    }
    ++mOperationDepth;
}

void CodeDocumentController::endOperation()
{
    LUDUS_ASSERT(mOperationDepth > 0);
    --mOperationDepth;
    if (mOperationDepth == 0)
    {
        commitPending();
    }
}

void CodeDocumentController::captureChange(int32 position, int32 removed, int32 added)
{
    if (mIsSynchronizing == true || IsBound() == false)
    {
        return;
    }
    if (mHasPending == false)
    {
        mPendingBytes = mBytes;
        mPendingStatus = CodeEditStatus::Ok;
        if (mOperationDepth == 0)
        {
            mBefore = mLastSelection;
            mKind = CodeEditKind::Command;
        }
    }
    mHasPending = true;
    if (mPendingStatus != CodeEditStatus::Ok)
    {
        return;
    }
    int32 length = 0;
    static_cast<void>(GetCodeCursorPosition(mPendingBytes, static_cast<usize>(mPendingBytes.size()), length));
    const int32 removedUnits = qMin(removed, length - position);
    const int32 addedUnits = qMin(added, mEditor.document()->characterCount() - 1 - position);
    usize start = 0;
    usize end = 0;
    if (removedUnits < 0 || addedUnits < 0 || GetCodeByteOffset(mPendingBytes, position, start) == false ||
        GetCodeByteOffset(mPendingBytes, position + removedUnits, end) == false)
    {
        mPendingStatus = CodeEditStatus::InvalidText;
        return;
    }
    QTextCursor inserted(mEditor.document());
    inserted.setPosition(position);
    inserted.setPosition(position + addedUnits, QTextCursor::KeepAnchor);
    QString text = inserted.selectedText();
    text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    QString oldText;
    if (DecodeCodeText(mPendingBytes.mid(static_cast<isize>(start), static_cast<isize>(end - start)), oldText) ==
            true &&
        oldText == text)
    {
        return;
    }
    if (mHasCrLf == true)
    {
        text.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
    }
    QStringEncoder encoder(QStringEncoder::Utf8);
    const QByteArray replacement = encoder.encode(text);
    if (encoder.hasError() == true)
    {
        mPendingStatus = CodeEditStatus::InvalidText;
        return;
    }
    const CodeDocumentSnapshot snapshot = mHostOrNull->GetSnapshot();
    const usize retained = static_cast<usize>(mPendingBytes.size()) - (end - start);
    if (retained > snapshot.LimitBytes || static_cast<usize>(replacement.size()) > snapshot.LimitBytes - retained)
    {
        mPendingStatus = CodeEditStatus::Capacity;
        return;
    }
    mPendingBytes.replace(static_cast<isize>(start), static_cast<isize>(end - start), replacement);
}

bool CodeDocumentController::canMerge(const CodeTextEdit& edit) const
{
    if (mHasTextGroup == false || mKind != mLastKind || mTypingClock.isValid() == false ||
        mTypingClock.elapsed() > TYPING_GROUP_MILLISECONDS)
    {
        return false;
    }
    if (mKind == CodeEditKind::Typing)
    {
        const QByteArray inserted = CopyCodeText(edit.Inserted);
        const bool isWhitespace = inserted == " " || inserted == "\t";
        return edit.RemovedBytes == 0 && edit.Inserted.IsEmpty() == false && inserted.contains('\n') == false &&
               edit.StartBytes == mLastStartBytes + mLastInsertedBytes &&
               (mHasTrailingWhitespace == false || isWhitespace == true);
    }
    if (edit.Inserted.IsEmpty() == false)
    {
        return false;
    }
    return (mKind == CodeEditKind::Backspace && edit.StartBytes + edit.RemovedBytes == mLastStartBytes) ||
           (mKind == CodeEditKind::Delete && edit.StartBytes == mLastStartBytes);
}

void CodeDocumentController::commitPending()
{
    if (mHasPending == false || mIsSynchronizing == true || mOperationDepth != 0 || mHostOrNull == nullptr)
    {
        return;
    }
    const CodeTextEdit edit = MakeCodeTextEdit(mBytes, mPendingBytes);
    if (edit.RemovedBytes == 0 && edit.Inserted.IsEmpty() == true && mPendingStatus == CodeEditStatus::Ok)
    {
        mHasPending = false;
        mPendingBytes.clear();
        return;
    }
    const CodeTextSelection after = CaptureSelection();
    CodeEditStatus status = mPendingStatus;
    if (status == CodeEditStatus::Ok)
    {
        status = mHostOrNull->ApplyEdit(mIdentity, edit, mBefore, after, canMerge(edit));
    }
    if (status != CodeEditStatus::Ok)
    {
        const bool isSameRevision = mHostOrNull->GetSnapshot().Identity == mIdentity;
        mHasPending = false;
        BreakUndoGroup();
        Synchronize(true);
        if (isSameRevision == true)
        {
            RestoreSelection(mBefore);
        }
        Q_EMIT Failed(DescribeCodeEditStatus(status));
        return;
    }
    mLastStartBytes = edit.StartBytes;
    mLastInsertedBytes = edit.Inserted.GetSize();
    mHasTrailingWhitespace = edit.Inserted.GetSize() == 1 && edit.Inserted[0] == ' ';
    mLastKind = mKind;
    mHasTextGroup = mKind != CodeEditKind::Command;
    mTypingClock.start();
    mBytes = mPendingBytes;
    mPendingBytes.clear();
    mHasPending = false;
    mIdentity = mHostOrNull->GetSnapshot().Identity;
    mLastSelection = CaptureSelection();
    updateHistoryAvailability();
    Q_EMIT TextAccepted();
}

CodeEditStatus CodeDocumentController::ApplyFormattedText(const CodeDocumentIdentity& expected,
                                                          const QByteArray& text,
                                                          CodeTextSelection selection)
{
    if (mHostOrNull == nullptr || mEditor.isReadOnly() == true)
    {
        return CodeEditStatus::Unavailable;
    }
    if (mHostOrNull->GetSnapshot().Identity != expected || expected != mIdentity)
    {
        return CodeEditStatus::Conflict;
    }
    QString decoded;
    if (DecodeCodeText(text, decoded) == false)
    {
        return CodeEditStatus::InvalidText;
    }
    BreakUndoGroup();
    const CodeTextEdit edit = MakeCodeTextEdit(mBytes, text);
    const CodeEditStatus status = mHostOrNull->ApplyEdit(expected, edit, CaptureSelection(), selection, false);
    if (status == CodeEditStatus::Ok)
    {
        const int32 vertical = mEditor.verticalScrollBar()->value();
        const int32 horizontal = mEditor.horizontalScrollBar()->value();
        Synchronize();
        RestoreSelection(selection);
        mEditor.verticalScrollBar()->setValue(vertical);
        mEditor.horizontalScrollBar()->setValue(horizontal);
        Q_EMIT TextAccepted();
    }
    return status;
}
} // namespace ludus::editor
