#include "internal/code_document_tab.h"

#include "internal/code_conflict_banner.h"
#include "internal/code_file_document.h"
#include "internal/code_language_registry.h"
#include "internal/code_structure_view.h"

#include <ludus/foundation/containers/borrowed_view.hpp>

#include <QScrollBar>
#include <QVBoxLayout>

// One open view of a file (CE01, CE02): a CodeEditor bound to a shared file host
// through a CodeDocumentController, plus its own view state and preview flag.
// Mirroring between two tabs of the same host runs through Synchronize, which is
// re-entrancy guarded, so a mirrored delta is never echoed back as a user edit.

namespace ludus::editor
{
using foundation::BorrowedView;
using foundation::int32;

namespace
{
constexpr const char* VIEW_STATE_PATH = "path";
constexpr const char* VIEW_STATE_PREVIEW = "preview";
constexpr const char* VIEW_STATE_ANCHOR = "anchor";
constexpr const char* VIEW_STATE_CARET = "caret";
constexpr const char* VIEW_STATE_SCROLL = "scroll";
} // namespace

CodeDocumentTab::CodeDocumentTab(CodeFileDocument* hostOrNull, const QString& absolutePath, QWidget* parentOrNull)
    : QWidget(parentOrNull), mEditor(new CodeEditor(this)), mController(new CodeDocumentController(*mEditor, this)),
      mBanner(new CodeConflictBanner(this)), mHostOrNull(hostOrNull), mPath(absolutePath)
{
    setObjectName(QStringLiteral("codeDocumentTab"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(mBanner);
    layout->addWidget(mEditor, 1);

    // Classify the language from the file name through the registry, matching
    // the render-graph Source pane's classification.
    const QByteArray pathUtf8 = mPath.toUtf8();
    mEditor->SetLanguage(CodeLanguageRegistry::Get().ClassifyFileName(
        BorrowedView<const char>(pathUtf8.constData(), static_cast<foundation::usize>(pathUtf8.size()))));

    connect(mController, &CodeDocumentController::TextAccepted, this, [this]() {
        // A first edit promotes a preview tab to permanent (CE01) and must mirror
        // to every sibling view of the same host.
        MakePermanent();
        RefreshHistoryAvailability();
        Q_EMIT Edited();
    });
    connect(mController, &CodeDocumentController::Failed, this, &CodeDocumentTab::Failed);
    connect(mEditor, &CodeEditor::UndoRequested, this, [this]() {
        QList<CodeTextSelection> selections;
        if (mHostOrNull != nullptr && mHostOrNull->Undo(selections) == true)
        {
            mController->Synchronize(true);
            // Restore the selection set recorded with the step so one undo brings
            // back both the text and every cursor (CE05).
            mController->RestoreSelectionSet(
                {selections.constData(), static_cast<foundation::usize>(selections.size())});
            RefreshHistoryAvailability();
            Q_EMIT Edited();
        }
    });
    connect(mEditor, &CodeEditor::RedoRequested, this, [this]() {
        QList<CodeTextSelection> selections;
        if (mHostOrNull != nullptr && mHostOrNull->Redo(selections) == true)
        {
            mController->Synchronize(true);
            mController->RestoreSelectionSet(
                {selections.constData(), static_cast<foundation::usize>(selections.size())});
            RefreshHistoryAvailability();
            Q_EMIT Edited();
        }
    });

    bindHost();
}

CodeDocumentTab::~CodeDocumentTab()
{
    delete mController;
    mController = nullptr;
}

void CodeDocumentTab::bindHost()
{
    mEditor->SetDocumentHistory(mHostOrNull != nullptr);
    mEditor->SetLargeFileMode(mHostOrNull != nullptr && mHostOrNull->GetProfile().SizeMode != CodeFileSizeMode::Normal);
    mController->Bind(mHostOrNull);
    if (mHostOrNull != nullptr)
    {
        mEditor->setReadOnly(mHostOrNull->IsReadOnly());
        if (mHostOrNull->IsReadOnly() == true)
        {
            mBanner->ShowExplanation(mHostOrNull->GetExplanation());
        }
        else if (mHostOrNull->GetExplanation().isEmpty() == false)
        {
            // Large-file degraded mode carries an explanation but stays editable.
            mBanner->ShowExplanation(mHostOrNull->GetExplanation());
        }
    }
    RefreshHistoryAvailability();
}

void CodeDocumentTab::SetPreview(bool isPreview)
{
    mIsPreview = isPreview;
}

void CodeDocumentTab::MakePermanent()
{
    if (mIsPreview == true)
    {
        mIsPreview = false;
        Q_EMIT BecamePermanent();
    }
}

void CodeDocumentTab::SetSessionKey(const QString& key)
{
    mSessionKey = key;
}

const QString& CodeDocumentTab::GetSessionKey() const noexcept
{
    return mSessionKey;
}

void CodeDocumentTab::BindSessionHost(CodeDocumentHost* hostOrNull)
{
    mSessionHostOrNull = hostOrNull;
    mController->Bind(hostOrNull);
    RefreshHistoryAvailability();
}

void CodeDocumentTab::SetReadOnlySource(const QString& text, const QString& explanation)
{
    mEditor->setReadOnly(true);
    mEditor->setPlainText(text);
    mBanner->ShowExplanation(explanation);
}

void CodeDocumentTab::AddSessionControls(QWidget& inoutControls, QWidget& inoutStatus)
{
    QVBoxLayout* column = static_cast<QVBoxLayout*>(layout());
    column->insertWidget(1, &inoutControls);
    column->addWidget(&inoutStatus);
}

void CodeDocumentTab::MirrorFromHost()
{
    // Re-read the shared host into this view's projection. Synchronize keeps the
    // view's own scroll and cursor and guards against re-emitting the delta.
    mController->Synchronize();
    RefreshHistoryAvailability();
}

void CodeDocumentTab::RefreshFromHost()
{
    if (mHostOrNull != nullptr)
    {
        mEditor->setReadOnly(mHostOrNull->IsReadOnly());
        mEditor->SetLargeFileMode(mHostOrNull->GetProfile().SizeMode != CodeFileSizeMode::Normal);
        mBanner->ShowExplanation(mHostOrNull->GetExplanation());
    }
    MirrorFromHost();
}

void CodeDocumentTab::RefreshHistoryAvailability()
{
    if (mHostOrNull != nullptr)
    {
        mEditor->SetHistoryAvailability(mHostOrNull->CanUndo(), mHostOrNull->CanRedo());
    }
    else if (mSessionHostOrNull != nullptr)
    {
        mEditor->SetHistoryAvailability(mSessionHostOrNull->CanUndo(), mSessionHostOrNull->CanRedo());
    }
    else
    {
        mEditor->SetHistoryAvailability(false, false);
    }
}

bool CodeDocumentTab::IsDirty() const
{
    return mHostOrNull != nullptr ? mHostOrNull->IsDirty()
                                  : mSessionHostOrNull != nullptr && mSessionHostOrNull->GetSnapshot().IsDirty;
}

bool CodeDocumentTab::IsReadOnly() const
{
    return mEditor->isReadOnly();
}

QJsonObject CodeDocumentTab::SaveViewState() const
{
    const QTextCursor cursor = mEditor->textCursor();
    QJsonObject state;
    state.insert(QLatin1String(VIEW_STATE_PATH), mPath);
    state.insert(QLatin1String(VIEW_STATE_PREVIEW), mIsPreview);
    state.insert(QLatin1String(VIEW_STATE_ANCHOR), cursor.anchor());
    state.insert(QLatin1String(VIEW_STATE_CARET), cursor.position());
    state.insert(QLatin1String(VIEW_STATE_SCROLL), mEditor->verticalScrollBar()->value());
    state.insert(QStringLiteral("folds"), mEditor->GetStructure().SaveFolds());
    return state;
}

void CodeDocumentTab::RestoreViewState(const QJsonObject& state)
{
    const int32 anchor = state.value(QLatin1String(VIEW_STATE_ANCHOR)).toInt(0);
    const int32 caret = state.value(QLatin1String(VIEW_STATE_CARET)).toInt(0);
    const int32 documentEnd = static_cast<int32>(mEditor->document()->characterCount()) - 1;
    QTextCursor cursor(mEditor->document());
    cursor.setPosition(qBound(0, anchor, documentEnd));
    cursor.setPosition(qBound(0, caret, documentEnd), QTextCursor::KeepAnchor);
    mEditor->RevealTextCursor(cursor);
    mEditor->GetStructure().RestoreFolds(state.value(QStringLiteral("folds")).toArray());
    mEditor->verticalScrollBar()->setValue(state.value(QLatin1String(VIEW_STATE_SCROLL)).toInt(0));
    if (state.value(QLatin1String(VIEW_STATE_PREVIEW)).toBool(false) == false)
    {
        mIsPreview = false;
    }
}
} // namespace ludus::editor
