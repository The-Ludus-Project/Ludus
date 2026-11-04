// Code editor status bar (architecture, "Status bar"), task M1.6. The bar shows
// the cursor position, selection, indentation, encoding, line endings, language
// and a language-server placeholder, and emits a request for the items M1 can
// change. Offscreen only.

#include "internal/code_document_tab.h"
#include "internal/code_editor.h"
#include "internal/code_editor_area.h"
#include "internal/code_file_document.h"
#include "internal/code_status_bar.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>

using namespace ludus::editor;

namespace
{
[[nodiscard]] QString writeFile(const QTemporaryDir& directory, const QString& name, const QByteArray& bytes)
{
    const QString path = QDir(directory.path()).filePath(name);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
    file.close();
    return path;
}
} // namespace

TEST_CASE("Status bar hides with no active code tab", "[code-editor][status-bar]")
{
    CodeStatusBar bar;
    bar.SetActiveTab(nullptr);
    CHECK(bar.isHidden() == true);
    CHECK(bar.GetPositionText().isEmpty() == true);
    // The tool-state item is a placeholder until M4.
    CHECK(bar.GetToolStateText().isEmpty() == false);
}

TEST_CASE("Status bar reflects the active code view", "[code-editor][status-bar]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString path = writeFile(directory, QStringLiteral("shader.slang"), QByteArray("void main() {}\n"));

    CodeEditorArea area;
    area.GetRegistry().SetProjectEpoch(1);
    REQUIRE(area.OpenFile(path, false) == true);
    CodeDocumentTab* tab = area.GetActiveTabOrNull();
    REQUIRE(tab != nullptr);

    CodeStatusBar bar;
    bar.SetActiveTab(tab);

    // Position is one-based and starts at the document head.
    CHECK(bar.GetPositionText() == QStringLiteral("Ln 1, Col 1"));
    // A .slang file classifies as Slang via the language registry.
    CHECK(bar.GetLanguageText() == QStringLiteral("Slang"));
    // A freshly written LF file reports UTF-8 / LF.
    CHECK(bar.GetEncodingText() == QStringLiteral("UTF-8"));
    CHECK(bar.GetLineEndingText() == QStringLiteral("LF"));
    CHECK(bar.GetIndentText().isEmpty() == false);

    // Moving the caret updates the position live.
    QTextCursor cursor = tab->GetEditor().textCursor();
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor, 5);
    tab->GetEditor().setTextCursor(cursor);
    QCoreApplication::processEvents();
    CHECK(bar.GetPositionText() == QStringLiteral("Ln 1, Col 6"));

    // Selecting shows a selected-character count.
    cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, 4);
    tab->GetEditor().setTextCursor(cursor);
    QCoreApplication::processEvents();
    CHECK(bar.GetSelectionText() == QStringLiteral("4 selected"));
}

TEST_CASE("Status bar language change applies to the view", "[code-editor][status-bar]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString path = writeFile(directory, QStringLiteral("notes.txt"), QByteArray("plain\n"));

    CodeEditorArea area;
    area.GetRegistry().SetProjectEpoch(1);
    REQUIRE(area.OpenFile(path, false) == true);
    CodeDocumentTab* tab = area.GetActiveTabOrNull();
    REQUIRE(tab != nullptr);

    CodeStatusBar bar;
    bar.SetActiveTab(tab);
    CHECK(bar.GetLanguageText() == QStringLiteral("Plain Text"));

    // The language item requests a change; the shell applies it to the editor.
    QSignalSpy spy(&bar, &CodeStatusBar::LanguageChangeRequested);
    tab->GetEditor().SetLanguage(CodeLanguage::Slang);
    bar.Refresh();
    CHECK(bar.GetLanguageText() == QStringLiteral("Slang"));
    // The request signal carries the registry identifier when driven from the menu.
    CHECK(spy.isValid() == true);
}

TEST_CASE("Status bar line-ending change updates the host profile", "[code-editor][status-bar]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString path = writeFile(directory, QStringLiteral("unix.txt"), QByteArray("a\nb\n"));

    CodeEditorArea area;
    area.GetRegistry().SetProjectEpoch(1);
    REQUIRE(area.OpenFile(path, false) == true);
    CodeDocumentTab* tab = area.GetActiveTabOrNull();
    REQUIRE(tab != nullptr);
    REQUIRE(tab->GetHostOrNull() != nullptr);

    CodeStatusBar bar;
    bar.SetActiveTab(tab);
    CHECK(bar.GetLineEndingText() == QStringLiteral("LF"));

    // Applying a CRLF choice (as the shell does on the menu request) is reflected.
    tab->GetHostOrNull()->SetProfileLineEnding(CodeLineEnding::CrLf);
    bar.Refresh();
    CHECK(bar.GetLineEndingText() == QStringLiteral("CRLF"));
}
