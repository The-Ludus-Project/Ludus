#include "internal/code_status_bar.h"

#include "internal/code_document_tab.h"
#include "internal/code_editor.h"
#include "internal/code_file_document.h"
#include "internal/code_language_registry.h"

#include <QAction>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenu>
#include <QTextCursor>
#include <QToolButton>

#include <utility>

// The code editor's status bar (architecture, "Status bar"). It mirrors the
// active code view: position and selection from the editor's cursor, and the
// indentation, encoding, line endings and language from the bound file host's
// profile. Items whose change M1 supports (position -> Go to Line, line endings,
// language) are clickable and emit a request; encoding and indentation are shown
// for information, and the tool-state item is a placeholder until M4. The bar
// owns no document and mutates nothing; it only reflects and requests.

namespace ludus::editor
{
namespace
{
using foundation::BorrowedView;

[[nodiscard]] QString ToQString(BorrowedView<const char> view)
{
    return QString::fromUtf8(view.GetDataOrNull(), static_cast<qsizetype>(view.GetSize()));
}

[[nodiscard]] QString DescribeEncoding(CodeTextEncoding encoding)
{
    switch (encoding)
    {
        case CodeTextEncoding::Utf8:
            return QStringLiteral("UTF-8");
        case CodeTextEncoding::Utf8Bom:
            return QStringLiteral("UTF-8 with BOM");
        case CodeTextEncoding::InvalidUtf8:
            return QStringLiteral("Binary");
    }
    return QStringLiteral("UTF-8");
}

[[nodiscard]] QString DescribeLineEnding(CodeLineEnding ending)
{
    switch (ending)
    {
        case CodeLineEnding::Lf:
            return QStringLiteral("LF");
        case CodeLineEnding::CrLf:
            return QStringLiteral("CRLF");
        case CodeLineEnding::Mixed:
            return QStringLiteral("Mixed");
    }
    return QStringLiteral("LF");
}

[[nodiscard]] QString DescribeIndent(const CodeIndentProfile& indent)
{
    const QString unit = indent.Style == CodeIndentStyle::Tabs ? QStringLiteral("Tab Size") : QStringLiteral("Spaces");
    return QStringLiteral("%1: %2").arg(unit).arg(indent.Width);
}

/// A flat, clickable status-bar item with an accessible name. The click handler
/// is wired by the caller; a non-interactive item simply has none.
[[nodiscard]] QToolButton* MakeItem(QWidget* parent, const QString& objectName, const QString& accessibleName)
{
    QToolButton* button = new QToolButton(parent);
    button->setObjectName(objectName);
    button->setAccessibleName(accessibleName);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    return button;
}
} // namespace

CodeStatusBar::CodeStatusBar(QWidget* parentOrNull)
    : QWidget(parentOrNull),
      mPosition(MakeItem(this, QStringLiteral("codeStatusPosition"), QStringLiteral("Cursor position"))),
      mSelection(MakeItem(this, QStringLiteral("codeStatusSelection"), QStringLiteral("Selection"))),
      mIndent(MakeItem(this, QStringLiteral("codeStatusIndent"), QStringLiteral("Indentation"))),
      mEncoding(MakeItem(this, QStringLiteral("codeStatusEncoding"), QStringLiteral("Encoding"))),
      mLineEnding(MakeItem(this, QStringLiteral("codeStatusLineEnding"), QStringLiteral("Line endings"))),
      mLanguage(MakeItem(this, QStringLiteral("codeStatusLanguage"), QStringLiteral("Language mode"))),
      mToolState(MakeItem(this, QStringLiteral("codeStatusToolState"), QStringLiteral("Language server state")))
{
    setObjectName(QStringLiteral("codeStatusBar"));
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    for (QToolButton* item : {mPosition, mSelection, mIndent, mEncoding, mLineEnding, mLanguage, mToolState})
    {
        layout->addWidget(item);
    }

    // Position opens Go to Line for the active view (M1 supports this change).
    connect(mPosition, &QToolButton::clicked, this, &CodeStatusBar::showGoToLine);
    // Language and line endings are changeable in M1 through a small menu.
    connect(mLanguage, &QToolButton::clicked, this, &CodeStatusBar::showLanguageMenu);
    connect(mLineEnding, &QToolButton::clicked, this, &CodeStatusBar::showLineEndingMenu);
    // Encoding and indentation are information-only in M1; the tool state is a
    // placeholder until M4 wires the language client. They take no click action.
    mEncoding->setEnabled(false);
    mIndent->setEnabled(false);
    mToolState->setEnabled(false);
    mToolState->setText(QStringLiteral("No language server"));
    mToolState->setToolTip(QStringLiteral("Language server status appears here once M4 adds the language client."));

    SetActiveTab(nullptr);
}

CodeEditor* CodeStatusBar::editorOrNull() const
{
    return mTabOrNull == nullptr ? nullptr : &mTabOrNull->GetEditor();
}

CodeFileDocument* CodeStatusBar::hostOrNull() const
{
    return mTabOrNull == nullptr ? nullptr : mTabOrNull->GetHostOrNull();
}

void CodeStatusBar::disconnectEditor()
{
    if (CodeEditor* editor = editorOrNull(); editor != nullptr)
    {
        disconnect(editor, nullptr, this, nullptr);
    }
}

void CodeStatusBar::SetActiveTab(CodeDocumentTab* tabOrNull)
{
    if (mTabOrNull == tabOrNull)
    {
        Refresh();
        return;
    }
    disconnectEditor();
    mTabOrNull = tabOrNull;
    if (CodeEditor* editor = editorOrNull(); editor != nullptr)
    {
        // Cursor and selection come from the QPlainTextEdit base; the host has no
        // caret. Reconnect so the bar follows the active view live.
        connect(editor, &QPlainTextEdit::cursorPositionChanged, this, &CodeStatusBar::updatePosition);
        connect(editor, &QPlainTextEdit::selectionChanged, this, &CodeStatusBar::updateSelection);
        // A lifetime guard: if the editor is destroyed under us, drop the tab so
        // a later Refresh does not touch a dangling pointer.
        connect(editor, &QObject::destroyed, this, [this]() {
            mTabOrNull = nullptr;
            setVisible(false);
        });
    }
    setVisible(tabOrNull != nullptr);
    Refresh();
}

void CodeStatusBar::Refresh()
{
    const bool hasTab = mTabOrNull != nullptr;
    setVisible(hasTab);
    if (hasTab == false)
    {
        return;
    }
    updatePosition();
    updateSelection();
    updateFromHost();
}

void CodeStatusBar::updatePosition()
{
    CodeEditor* editor = editorOrNull();
    if (editor == nullptr)
    {
        mPosition->setText(QString{});
        return;
    }
    const QTextCursor cursor = editor->textCursor();
    // One-based line and column, like the Go to Line target and other editors.
    mPosition->setText(QStringLiteral("Ln %1, Col %2").arg(cursor.blockNumber() + 1).arg(cursor.columnNumber() + 1));
}

void CodeStatusBar::updateSelection()
{
    CodeEditor* editor = editorOrNull();
    if (editor == nullptr)
    {
        mSelection->setText(QString{});
        mSelection->setVisible(false);
        return;
    }
    const QTextCursor cursor = editor->textCursor();
    if (cursor.hasSelection() == false)
    {
        // A single caret shows no count; the multi-cursor count arrives in M2.
        mSelection->setVisible(false);
        mSelection->setText(QString{});
        return;
    }
    const int selected = cursor.selectionEnd() - cursor.selectionStart();
    mSelection->setVisible(true);
    mSelection->setText(QStringLiteral("%1 selected").arg(selected));
}

void CodeStatusBar::updateFromHost()
{
    CodeEditor* editor = editorOrNull();
    CodeFileDocument* host = hostOrNull();
    if (editor != nullptr)
    {
        const CodeLanguageDescriptor& descriptor = CodeLanguageRegistry::Get().Describe(editor->GetLanguage());
        mLanguage->setText(ToQString(descriptor.DisplayName));
    }
    if (host == nullptr)
    {
        // An empty (host-less) tab still has a language, but no file profile.
        mIndent->setText(QString{});
        mEncoding->setText(QString{});
        mLineEnding->setText(QString{});
        return;
    }
    const CodeTextProfile& profile = host->GetProfile();
    mIndent->setText(DescribeIndent(profile.Indent));
    mEncoding->setText(DescribeEncoding(profile.Encoding));
    mLineEnding->setText(DescribeLineEnding(profile.LineEnding));
    // A read-only host cannot change its line endings or language usefully.
    const bool editable = host->IsReadOnly() == false;
    mLineEnding->setEnabled(editable);
    mLanguage->setEnabled(true);
}

void CodeStatusBar::showGoToLine()
{
    CodeEditor* editor = editorOrNull();
    if (editor == nullptr)
    {
        return;
    }
    bool isAccepted = false;
    const int line = QInputDialog::getInt(this,
                                          QStringLiteral("Go to Line"),
                                          QStringLiteral("Line"),
                                          editor->textCursor().blockNumber() + 1,
                                          1,
                                          editor->blockCount(),
                                          1,
                                          &isAccepted);
    if (isAccepted == true)
    {
        editor->GoToLine(line);
    }
}

void CodeStatusBar::showLanguageMenu()
{
    CodeEditor* editor = editorOrNull();
    if (editor == nullptr)
    {
        return;
    }
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("codeStatusLanguageMenu"));
    const BorrowedView<const CodeLanguageDescriptor> descriptors = CodeLanguageRegistry::Get().GetDescriptors();
    for (foundation::usize index = 0; index < descriptors.GetSize(); ++index)
    {
        const CodeLanguageDescriptor& descriptor = descriptors.GetDataOrNull()[index];
        const QString identifier = ToQString(descriptor.Identifier);
        QAction* action = menu.addAction(ToQString(descriptor.DisplayName));
        action->setCheckable(true);
        action->setChecked(descriptor.Language == editor->GetLanguage());
        connect(action, &QAction::triggered, this, [this, identifier]() {
            Q_EMIT LanguageChangeRequested(identifier);
        });
    }
    menu.exec(mLanguage->mapToGlobal(mLanguage->rect().bottomLeft()));
}

void CodeStatusBar::showLineEndingMenu()
{
    CodeFileDocument* host = hostOrNull();
    if (host == nullptr || host->IsReadOnly() == true)
    {
        return;
    }
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("codeStatusLineEndingMenu"));
    const CodeLineEnding current = host->GetProfile().LineEnding;
    const std::pair<const char*, CodeLineEnding> options[] = {
        {"LF", CodeLineEnding::Lf},
        {"CRLF", CodeLineEnding::CrLf},
    };
    for (const auto& option : options)
    {
        QAction* action = menu.addAction(QString::fromLatin1(option.first));
        action->setCheckable(true);
        action->setChecked(option.second == current);
        const QString ending = option.second == CodeLineEnding::CrLf ? QStringLiteral("crlf") : QStringLiteral("lf");
        connect(action, &QAction::triggered, this, [this, ending]() { Q_EMIT LineEndingChangeRequested(ending); });
    }
    menu.exec(mLineEnding->mapToGlobal(mLineEnding->rect().bottomLeft()));
}

QString CodeStatusBar::GetPositionText() const
{
    return mPosition->text();
}

QString CodeStatusBar::GetSelectionText() const
{
    return mSelection->text();
}

QString CodeStatusBar::GetIndentText() const
{
    return mIndent->text();
}

QString CodeStatusBar::GetEncodingText() const
{
    return mEncoding->text();
}

QString CodeStatusBar::GetLineEndingText() const
{
    return mLineEnding->text();
}

QString CodeStatusBar::GetLanguageText() const
{
    return mLanguage->text();
}

QString CodeStatusBar::GetToolStateText() const
{
    return mToolState->text();
}
} // namespace ludus::editor
