#include "internal/code_navigation.h"

#include "internal/code_editor.h"

#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFrame>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QScreen>
#include <QTextBlock>
#include <QTextDocument>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
using namespace foundation;
constexpr int32 MAX_HISTORY = 64;
constexpr int32 PICKER_WIDTH = 640;
constexpr int32 PICKER_HEIGHT = 320;
constexpr int32 INSPECT_WIDTH = 900;
constexpr int32 INSPECT_HEIGHT = 600;
constexpr int32 PICKER_OFFSET = 8;
constexpr qsizetype FIRST_SEMANTIC_ACTION = 3;

int32 ClampOffset(const QString& text, int32 offset)
{
    int32 result = qBound(0, offset, static_cast<int32>(text.size()));
    if (result > 0 && result < text.size() && text.at(result).isLowSurrogate() == true)
    {
        --result;
    }
    return result;
}
} // namespace

CodeNavigation::CodeNavigation(CodeEditor& inoutEditor)
    : QObject(&inoutEditor), mEditor(inoutEditor), mPopup(new QFrame(&inoutEditor, Qt::Popup)),
      mFilter(new QLineEdit(mPopup)), mResults(new QListWidget(mPopup)), mMessage(new QLabel(mPopup))
{
    mPopup->setObjectName(QStringLiteral("codeNavigationPicker"));
    mFilter->setObjectName(QStringLiteral("codeNavigationFilter"));
    mFilter->setPlaceholderText(QStringLiteral("Type to filter; Enter to jump; Escape to cancel"));
    mFilter->setAccessibleName(QStringLiteral("Filter source navigation results"));
    mResults->setObjectName(QStringLiteral("codeNavigationResults"));
    mResults->setAccessibleName(QStringLiteral("Source navigation results"));
    mMessage->setObjectName(QStringLiteral("codeNavigationMessage"));
    mMessage->setTextFormat(Qt::PlainText);
    mMessage->setWordWrap(true);
    QVBoxLayout* layout = new QVBoxLayout(mPopup);
    layout->addWidget(mFilter);
    layout->addWidget(mResults, 1);
    layout->addWidget(mMessage);
    mPopup->installEventFilter(this);
    mFilter->installEventFilter(this);
    mResults->installEventFilter(this);
    connect(mFilter, &QLineEdit::textChanged, this, &CodeNavigation::filterResults);
    connect(mFilter, &QLineEdit::returnPressed, this, &CodeNavigation::acceptResult);
    connect(mResults, &QListWidget::itemActivated, this, [this]() { acceptResult(); });
    const auto addAction = [this](const QString& label, const QKeySequence& shortcut, const auto& operation) {
        QAction* action = new QAction(label, this);
        action->setShortcut(shortcut);
        action->setShortcutContext(Qt::WidgetShortcut);
        mEditor.addAction(action);
        mActions.append(action);
        connect(action, &QAction::triggered, this, operation);
        return action;
    };
    mBack = addAction(QStringLiteral("Navigate Back"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Left), [this]() {
        Back();
    });
    mForward = addAction(QStringLiteral("Navigate Forward"),
                         QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Right),
                         [this]() { Forward(); });
    mFiles =
        addAction(QStringLiteral("Quick Open Source"), QKeySequence(Qt::CTRL | Qt::Key_P), [this]() { openFile(); });
    addAction(QStringLiteral("Go to Symbol"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O), [this]() {
        Q_EMIT Requested(CodeNavigationKind::Symbols);
    });
    addAction(QStringLiteral("Go to Definition"), QKeySequence(Qt::Key_F12), [this]() {
        Q_EMIT Requested(CodeNavigationKind::Definition);
    });
    mReferences = addAction(QStringLiteral("Find References"), QKeySequence(Qt::SHIFT | Qt::Key_F12), [this]() {
        Q_EMIT Requested(CodeNavigationKind::References);
    });
    mPopup->hide();
    RefreshActions();
}

const QString& CodeNavigation::GetSource() const noexcept
{
    return mFile;
}
QList<QAction*> CodeNavigation::GetActions() const
{
    return mActions;
}
bool CodeNavigation::IsChoosing() const
{
    return mPopup->isVisible();
}

void CodeNavigation::RefreshActions()
{
    mBack->setEnabled(mPrevious.isEmpty() == false);
    mForward->setEnabled(mNext.isEmpty() == false);
    mFiles->setEnabled(mFilesAvailable.isEmpty() == false);
    for (qsizetype index = FIRST_SEMANTIC_ACTION; index < mActions.size(); ++index)
    {
        mActions.at(index)->setEnabled(mEditor.GetLanguage() == CodeLanguage::Slang);
    }
    mReferences->setEnabled(mEditor.GetLanguage() == CodeLanguage::Slang && mCanFindReferences == true);
    mReferences->setToolTip(
        mCanFindReferences == true
            ? QStringLiteral("Find references in the selected compiler context.")
            : QStringLiteral("The selected slangd does not advertise reference search. Use Find for text matches."));
}

void CodeNavigation::SetReferencesAvailable(bool canFindReferences)
{
    mCanFindReferences = canFindReferences;
    RefreshActions();
}

CodeNavigation::Location CodeNavigation::capture(const QTextCursor& cursor) const
{
    return {mFile, cursor, cursor.blockNumber(), cursor.positionInBlock()};
}

QTextCursor CodeNavigation::resolve(const Location& location) const
{
    if (location.Anchor.isNull() == false)
    {
        return location.Anchor;
    }
    const QTextBlock block = mEditor.document()->findBlockByNumber(qBound(0, location.Block, mEditor.blockCount() - 1));
    QTextCursor cursor(block);
    cursor.setPosition(block.position() + ClampOffset(block.text(), location.Column));
    return cursor;
}

void CodeNavigation::appendLocation(QList<Location>& inoutLocations, const Location& location)
{
    if (inoutLocations.isEmpty() == false)
    {
        const Location& last = inoutLocations.last();
        if (last.File == location.File && last.Block == location.Block && last.Column == location.Column)
        {
            return;
        }
    }
    if (inoutLocations.size() >= MAX_HISTORY)
    {
        inoutLocations.removeFirst();
    }
    inoutLocations.append(location);
}

void CodeNavigation::Remember(const QTextCursor& cursor)
{
    if (mIsTraversing == true || mIsBinding == true)
    {
        return;
    }
    appendLocation(mPrevious, capture(cursor));
    mNext.clear();
    RefreshActions();
}

void CodeNavigation::RememberCurrent()
{
    Remember(mEditor.textCursor());
}

void CodeNavigation::freezeLocations(QList<Location>& inoutLocations)
{
    for (Location& location : inoutLocations)
    {
        if (location.File == mFile && location.Anchor.isNull() == false)
        {
            location.Block = location.Anchor.blockNumber();
            location.Column = location.Anchor.positionInBlock();
            location.Anchor = {};
        }
    }
}

void CodeNavigation::anchorLocations(QList<Location>& inoutLocations)
{
    for (Location& location : inoutLocations)
    {
        if (location.File == mFile)
        {
            location.Anchor = resolve(location);
        }
    }
}

void CodeNavigation::Clear()
{
    Dismiss();
    mPrevious.clear();
    mNext.clear();
    mFile.clear();
    mScope.clear();
    RefreshActions();
}

void CodeNavigation::BeginSource(const QString& scope, const QString& file, const QStringList& files)
{
    if (scope != mScope)
    {
        Clear();
        mScope = scope;
    }
    if (file != mFile)
    {
        if (mFile.isEmpty() == false)
        {
            RememberCurrent();
        }
        freezeLocations(mPrevious);
        freezeLocations(mNext);
        Dismiss();
    }
    mIsBinding = true;
    mFile = file;
    mFilesAvailable = files;
}

void CodeNavigation::EndSource()
{
    mIsBinding = false;
    anchorLocations(mPrevious);
    anchorLocations(mNext);
    RefreshActions();
}

void CodeNavigation::Back()
{
    traverse(false);
}
void CodeNavigation::Forward()
{
    traverse(true);
}

void CodeNavigation::traverse(bool shouldGoForward)
{
    QList<Location>& locations = shouldGoForward == true ? mNext : mPrevious;
    if (locations.isEmpty() == true)
    {
        return;
    }
    const Location destination = locations.last();
    if (destination.File != mFile && mFilesAvailable.contains(destination.File) == false)
    {
        ReportFailure(
            QStringLiteral("That source was removed. Quick Open an available source; the current view was retained."));
        return;
    }
    Location origin = capture(mEditor.textCursor());
    const QString scope = mScope;
    mIsTraversing = true;
    if (destination.File != mFile)
    {
        Q_EMIT FileRequested(destination.File);
        origin.Anchor = {};
    }
    if (scope == mScope && destination.File == mFile)
    {
        Location resolved = destination;
        if (origin.File != destination.File)
        {
            resolved.Anchor = {};
        }
        mEditor.setTextCursor(resolve(resolved));
        mEditor.ensureCursorVisible();
        locations.removeLast();
        appendLocation(shouldGoForward == true ? mPrevious : mNext, origin);
        mEditor.setFocus();
    }
    else
    {
        ReportFailure(QStringLiteral("Cannot open that source. The navigation history was retained."));
    }
    mIsTraversing = false;
    RefreshActions();
}

void CodeNavigation::openFile()
{
    Q_EMIT Dismissed();
    QList<CodeNavigationTarget> targets;
    for (const QString& file : mFilesAvailable)
    {
        targets.append({file, file, {}, 0, 0, false});
    }
    ShowResults(targets,
                QStringLiteral("Open a source in this package. Unsaved draft bytes stay with their document owner."));
    mIsFilePicker = true;
}

void CodeNavigation::ShowResults(const QList<CodeNavigationTarget>& targets, const QString& message)
{
    mTargets = targets;
    mIsFilePicker = false;
    mMessage->setText(message);
    mFilter->clear();
    filterResults();
    showPicker();
}

void CodeNavigation::showPicker()
{
    const QRect available = mEditor.screen()->availableGeometry();
    mPopup->resize(qMin(PICKER_WIDTH, available.width()), qMin(PICKER_HEIGHT, available.height()));
    QPoint position = mEditor.mapToGlobal(QPoint(0, PICKER_OFFSET));
    position.setX(qBound(available.left(), position.x(), available.right() - mPopup->width() + 1));
    position.setY(qBound(available.top(), position.y(), available.bottom() - mPopup->height() + 1));
    mPopup->move(position);
    mPopup->show();
    mFilter->setFocus();
}

void CodeNavigation::filterResults()
{
    mResults->clear();
    const QStringList words = mFilter->text().simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (qsizetype index = 0; index < mTargets.size(); ++index)
    {
        const QString& label = mTargets.at(index).Label;
        bool isMatching = true;
        for (const QString& word : words)
        {
            isMatching = isMatching == true && label.contains(word, Qt::CaseInsensitive) == true;
        }
        if (isMatching == true)
        {
            QListWidgetItem* item = new QListWidgetItem(label, mResults);
            item->setData(Qt::UserRole, index);
            item->setToolTip(label);
        }
    }
    if (mResults->count() > 0)
    {
        mResults->setCurrentRow(0);
    }
}

void CodeNavigation::acceptResult()
{
    if (mResults->currentItem() == nullptr)
    {
        return;
    }
    const int32 index = mResults->currentItem()->data(Qt::UserRole).toInt();
    if (index < 0 || index >= mTargets.size())
    {
        return;
    }
    if (mIsFilePicker == true)
    {
        static_cast<void>(Navigate(mTargets.at(index)));
    }
    else
    {
        Q_EMIT ResultChosen(index);
    }
}

void CodeNavigation::Dismiss()
{
    mPopup->hide();
    mTargets.clear();
}

void CodeNavigation::ReportFailure(const QString& message)
{
    mTargets.clear();
    filterResults();
    mMessage->setText(message);
    showPicker();
}

bool CodeNavigation::Navigate(const CodeNavigationTarget& target)
{
    // Copy before hiding: focus changes can cancel and release the provider's results.
    const CodeNavigationTarget destination = target;
    if (destination.IsReadOnly == true)
    {
        Dismiss();
        QDialog dialog(&mEditor);
        dialog.setWindowTitle(QStringLiteral("Shader definition — read-only captured context"));
        dialog.resize(INSPECT_WIDTH, INSPECT_HEIGHT);
        QVBoxLayout* layout = new QVBoxLayout(&dialog);
        QLabel* label = new QLabel(destination.Label, &dialog);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        layout->addWidget(label);
        QPlainTextEdit* source = new QPlainTextEdit(&dialog);
        source->setObjectName(QStringLiteral("codeNavigationReadOnly"));
        source->setReadOnly(true);
        source->setFont(mEditor.font());
        source->setPlainText(destination.Text);
        QTextCursor cursor(source->document());
        cursor.setPosition(destination.Start);
        cursor.setPosition(destination.End, QTextCursor::KeepAnchor);
        source->setTextCursor(cursor);
        source->centerCursor();
        layout->addWidget(source, 1);
        QDialogButtonBox* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(close);
        static_cast<void>(dialog.exec());
        return true;
    }
    if (destination.File.isEmpty() == false && destination.File != mFile &&
        mFilesAvailable.contains(destination.File) == false)
    {
        ReportFailure(QStringLiteral("That source is no longer in the package. Request navigation again."));
        return false;
    }
    const bool hasChangedSource = destination.File != mFile && destination.File.isEmpty() == false;
    if (hasChangedSource == true)
    {
        Q_EMIT FileRequested(destination.File);
        if (mFile != destination.File)
        {
            ReportFailure(QStringLiteral("The source could not be opened; current view retained."));
            return false;
        }
    }
    const QString text = mEditor.toPlainText();
    if (destination.Text.isEmpty() == false && destination.Text != text)
    {
        ReportFailure(QStringLiteral("The target source changed. Request navigation again; no jump was made."));
        return false;
    }
    QTextCursor cursor(mEditor.document());
    cursor.setPosition(ClampOffset(text, destination.Start));
    cursor.setPosition(ClampOffset(text, destination.End), QTextCursor::KeepAnchor);
    if (hasChangedSource == false && cursor != mEditor.textCursor())
    {
        RememberCurrent();
    }
    Dismiss();
    mEditor.setTextCursor(cursor);
    mEditor.ensureCursorVisible();
    mEditor.setFocus();
    return true;
}

bool CodeNavigation::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == mPopup && event->type() == QEvent::Hide)
    {
        Q_EMIT Dismissed();
    }
    if (event->type() == QEvent::KeyPress)
    {
        const QKeyEvent* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape)
        {
            Dismiss();
            mEditor.setFocus();
            return true;
        }
        if (watched == mFilter && (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down))
        {
            if (mResults->count() > 0)
            {
                mResults->setCurrentRow(
                    qBound(0, mResults->currentRow() + (key->key() == Qt::Key_Up ? -1 : 1), mResults->count() - 1));
            }
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}
} // namespace ludus::editor
