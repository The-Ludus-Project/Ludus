#include "internal/code_editor.h"
#include "internal/code_editor_panel.h"
#include "internal/code_navigation.h"
#include "internal/code_navigation_results.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QFrame>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QTest>
#include <QTextDocument>
#include <QTimer>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
QJsonObject MakeRange(int32 line, int32 start, int32 end)
{
    return {
        {QStringLiteral("start"), QJsonObject{{QStringLiteral("line"), line}, {QStringLiteral("character"), start}}},
        {QStringLiteral("end"), QJsonObject{{QStringLiteral("line"), line}, {QStringLiteral("character"), end}}}};
}
} // namespace

TEST_CASE("Navigation accepts compiler symbols and locations only through captured UTF-16 sources",
          "[editor][code-navigation]")
{
    const QString uri = QStringLiteral("file:///context.slang");
    const QString prefix = QStringLiteral("struct Generated {};\n");
    const QString source = QString::fromUtf8("// 😀\nfloat Value() { return 1; }\n");
    const QString unit = prefix + source;
    QJsonObject symbol{{QStringLiteral("name"), QStringLiteral("Value")},
                       {QStringLiteral("selectionRange"), MakeRange(2, 6, 11)}};
    QJsonObject generated{{QStringLiteral("name"), QStringLiteral("Generated")},
                          {QStringLiteral("selectionRange"), MakeRange(0, 7, 16)}};
    QString message;
    const QList<CodeNavigationTarget> symbols = ReadCodeNavigationResults(CodeNavigationKind::Symbols,
                                                                          QJsonArray{generated, symbol},
                                                                          uri,
                                                                          unit,
                                                                          prefix,
                                                                          source,
                                                                          QStringLiteral("color.slang"),
                                                                          {},
                                                                          message);
    REQUIRE(symbols.size() == 1);
    CHECK(symbols.first().Text.mid(symbols.first().Start, symbols.first().End - symbols.first().Start) ==
          QStringLiteral("Value"));
    CHECK(symbols.first().File == QStringLiteral("color.slang"));
    CHECK(symbols.first().IsReadOnly == false);
    const QJsonObject link{{QStringLiteral("targetUri"), uri},
                           {QStringLiteral("targetSelectionRange"), MakeRange(0, 7, 16)}};
    const QList<CodeNavigationTarget> definition = ReadCodeNavigationResults(CodeNavigationKind::Definition,
                                                                             QJsonArray{link},
                                                                             uri,
                                                                             unit,
                                                                             prefix,
                                                                             source,
                                                                             QStringLiteral("color.slang"),
                                                                             {},
                                                                             message);
    REQUIRE(definition.size() == 1);
    CHECK(definition.first().IsReadOnly == true);
    CHECK(definition.first().Text == unit);
    QMap<QString, CodeNavigationSource> sources;
    sources.insert(QStringLiteral("file:///helper.slang"),
                   {QStringLiteral("helpers/helper.slang"), QStringLiteral("float Helper();\n")});
    const QJsonObject location{{QStringLiteral("uri"), QStringLiteral("file:///helper.slang")},
                               {QStringLiteral("range"), MakeRange(0, 6, 12)}};
    const QList<CodeNavigationTarget> references =
        ReadCodeNavigationResults(CodeNavigationKind::References,
                                  QJsonArray{location,
                                             location,
                                             QJsonObject{{QStringLiteral("uri"), QStringLiteral("file:///etc/passwd")},
                                                         {QStringLiteral("range"), MakeRange(0, 0, 1)}}},
                                  uri,
                                  unit,
                                  prefix,
                                  source,
                                  QStringLiteral("color.slang"),
                                  sources,
                                  message);
    REQUIRE(references.size() == 1);
    CHECK(references.first().File == QStringLiteral("helpers/helper.slang"));
    CHECK(message.contains(QStringLiteral("omitted")) == true);
}

TEST_CASE("Nested symbols and malformed or oversized server results preserve navigation bounds",
          "[editor][code-navigation]")
{
    const QString uri = QStringLiteral("file:///context.slang");
    const QString source = QString::fromUtf8("// 😀\nstruct A { float X; };\n");
    int32 offset = 99;
    CHECK(GetCodeProtocolOffset(source,
                                QJsonObject{{QStringLiteral("line"), 0}, {QStringLiteral("character"), 4}},
                                offset) == false);
    CHECK(offset == 99);
    CHECK(GetCodeProtocolOffset(source,
                                QJsonObject{{QStringLiteral("line"), -1}, {QStringLiteral("character"), 0}},
                                offset) == false);
    CHECK(GetCodeProtocolOffset(source,
                                QJsonObject{{QStringLiteral("line"), 9999}, {QStringLiteral("character"), 0}},
                                offset) == false);
    QJsonObject child{{QStringLiteral("name"), QStringLiteral("X")},
                      {QStringLiteral("selectionRange"), MakeRange(1, 17, 18)}};
    QJsonObject parent{{QStringLiteral("name"), QStringLiteral("A")},
                       {QStringLiteral("selectionRange"), MakeRange(1, 7, 8)},
                       {QStringLiteral("children"), QJsonArray{child}}};
    QString message;
    const QList<CodeNavigationTarget> targets = ReadCodeNavigationResults(CodeNavigationKind::Symbols,
                                                                          QJsonArray{parent},
                                                                          uri,
                                                                          source,
                                                                          {},
                                                                          source,
                                                                          {},
                                                                          {},
                                                                          message);
    REQUIRE(targets.size() == 2);
    CHECK(targets.at(1).Text.mid(targets.at(1).Start, 1) == QStringLiteral("X"));
    QJsonArray huge;
    for (int32 index = 0; index < 9000; ++index)
    {
        huge.append(parent);
    }
    const QList<CodeNavigationTarget> bounded =
        ReadCodeNavigationResults(CodeNavigationKind::Symbols, huge, uri, source, {}, source, {}, {}, message);
    CHECK(bounded.size() <= 256);
    CHECK(message.contains(QStringLiteral("limit")) == true);
}

TEST_CASE("Line and diagnostic jumps share anchored Back and Forward without editing or owning Undo",
          "[editor][code-navigation]")
{
    CodeEditor editor;
    editor.setPlainText(QStringLiteral("one\ntwo\nthree\nfour\n"));
    editor.document()->setModified(false);
    editor.GoToLine(3);
    editor.GoToLine(4);
    editor.GetNavigation().Back();
    CHECK(editor.textCursor().blockNumber() == 2);
    editor.GetNavigation().Back();
    CHECK(editor.textCursor().position() == 0);
    editor.GetNavigation().Forward();
    CHECK(editor.textCursor().blockNumber() == 2);
    CHECK(editor.document()->isModified() == false);
    CHECK(editor.document()->isUndoAvailable() == false);
    editor.SetDiagnostics({{2, 1, 0, CodeDiagnosticSeverity::Error, QStringLiteral("test")}}, true);
    editor.GoToDiagnostic();
    editor.GetNavigation().Back();
    CHECK(editor.textCursor().blockNumber() == 2);
    // Qt anchors history through edits while this source remains bound.
    editor.GoToLine(4);
    QTextCursor insertion(editor.document());
    insertion.insertText(QStringLiteral("new\n"));
    editor.GetNavigation().Back();
    CHECK(editor.textCursor().blockNumber() == 3);
}

TEST_CASE("Cross-file navigation retains drafts and history and rejects removed targets locally",
          "[editor][code-navigation]")
{
    CodeEditor editor;
    QMap<QString, QString> drafts{{QStringLiteral("a.slang"), QStringLiteral("a0\na1\na2\n")},
                                  {QStringLiteral("b.slang"), QStringLiteral("b0\nb1\n")}};
    const QStringList files = drafts.keys();
    const auto bind = [&](const QString& file) {
        editor.GetNavigation().BeginSource(QStringLiteral("package"), file, drafts.keys());
        editor.setPlainText(drafts.value(file));
        editor.GetNavigation().EndSource();
    };
    bind(QStringLiteral("a.slang"));
    editor.GoToLine(3);
    QObject::connect(&editor.GetNavigation(), &CodeNavigation::FileRequested, &editor, bind);
    REQUIRE(
        editor.GetNavigation().Navigate(
            {QStringLiteral("B"), QStringLiteral("b.slang"), drafts.value(QStringLiteral("b.slang")), 3, 5, false}) ==
        true);
    CHECK(editor.GetNavigation().GetSource() == QStringLiteral("b.slang"));
    editor.GetNavigation().Back();
    CHECK(editor.GetNavigation().GetSource() == QStringLiteral("a.slang"));
    CHECK(editor.textCursor().blockNumber() == 2);
    editor.GetNavigation().Forward();
    CHECK(editor.GetNavigation().GetSource() == QStringLiteral("b.slang"));
    drafts.remove(QStringLiteral("a.slang"));
    editor.GetNavigation().BeginSource(QStringLiteral("package"), QStringLiteral("b.slang"), drafts.keys());
    editor.GetNavigation().EndSource();
    const QTextCursor before = editor.textCursor();
    editor.GetNavigation().ShowResults({{QStringLiteral("Removed source"), QStringLiteral("a.slang"), {}, 0, 0, false}},
                                       QStringLiteral("Old results"));
    editor.GetNavigation().Back();
    CHECK(editor.textCursor() == before);
    QListWidget* staleResultsOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeNavigationResults"));
    REQUIRE(staleResultsOrNull != nullptr);
    CHECK(staleResultsOrNull->count() == 0);
    QLabel* messageOrNull = editor.findChild<QLabel*>(QStringLiteral("codeNavigationMessage"));
    REQUIRE(messageOrNull != nullptr);
    CHECK(messageOrNull->text().contains(QStringLiteral("removed")) == true);
    editor.GetNavigation().Dismiss();
    editor.GetNavigation().BeginSource(QStringLiteral("other package"), QStringLiteral("b.slang"), files);
    editor.GetNavigation().EndSource();
    editor.GetNavigation().Back();
    CHECK(editor.textCursor() == before);
}

TEST_CASE("Navigation chooser filters without previewing and Escape preserves source and selection",
          "[editor][code-navigation]")
{
    CodeEditor editor;
    editor.setPlainText(QStringLiteral("source"));
    editor.show();
    const QTextCursor before = editor.textCursor();
    QSignalSpy chosen(&editor.GetNavigation(), &CodeNavigation::ResultChosen);
    editor.GetNavigation().ShowResults({{QStringLiteral("Evaluate Material"), {}, {}, 0, 0, false},
                                        {QStringLiteral("Trace Scene"), {}, {}, 0, 0, false}},
                                       QStringLiteral("Choose a symbol"));
    QLineEdit* filterOrNull = editor.findChild<QLineEdit*>(QStringLiteral("codeNavigationFilter"));
    QListWidget* resultsOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeNavigationResults"));
    REQUIRE(filterOrNull != nullptr);
    REQUIRE(resultsOrNull != nullptr);
    QTest::keyClicks(filterOrNull, QStringLiteral("material"));
    REQUIRE(resultsOrNull->count() == 1);
    QTest::keyClick(filterOrNull, Qt::Key_Return);
    REQUIRE(chosen.size() == 1);
    CHECK(chosen.first().first().toInt() == 0);
    CHECK(editor.textCursor() == before);
    QTest::keyClick(filterOrNull, Qt::Key_Escape);
    CHECK(editor.GetNavigation().IsChoosing() == false);
    CHECK(editor.toPlainText() == QStringLiteral("source"));
    CHECK(editor.textCursor() == before);
}

TEST_CASE("Generated definitions are inspected read-only without changing the authored buffer",
          "[editor][code-navigation]")
{
    CodeEditor editor;
    editor.setPlainText(QStringLiteral("authored"));
    bool isReadOnly = false;
    QTimer::singleShot(0, &editor, [&]() {
        QPlainTextEdit* sourceOrNull = editor.findChild<QPlainTextEdit*>(QStringLiteral("codeNavigationReadOnly"));
        if (sourceOrNull != nullptr)
        {
            isReadOnly = sourceOrNull->isReadOnly() == true &&
                         sourceOrNull->textCursor().selectedText() == QStringLiteral("Scene");
            static_cast<QDialog*>(sourceOrNull->parentWidget())->reject();
        }
    });
    CHECK(editor.GetNavigation().Navigate(
              {QStringLiteral("Scene API"), {}, QStringLiteral("struct Scene {};"), 7, 12, true}) == true);
    CHECK(isReadOnly == true);
    CHECK(editor.toPlainText() == QStringLiteral("authored"));
    CHECK(editor.document()->isUndoAvailable() == false);
}
