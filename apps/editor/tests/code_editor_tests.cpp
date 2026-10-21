#include "internal/code_editor.h"
#include "internal/code_editor_panel.h"
#include "internal/code_syntax_highlighter.h"
#include "internal/render_graph_document.h"

#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QInputMethodEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSysInfo>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
void SelectText(CodeEditor& inoutEditor, int32 start, int32 end)
{
    QTextCursor cursor(inoutEditor.document());
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    inoutEditor.setTextCursor(cursor);
}

void TriggerAction(CodeEditor& inoutEditor, const QString& label)
{
    for (QAction* action : inoutEditor.actions())
    {
        if (action->text() == label)
        {
            action->trigger();
            return;
        }
    }
    FAIL("Missing code editor action");
}

void FinishHighlighting()
{
    // Qt queues the first highlight and then the coalesced selection update.
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}
} // namespace

TEST_CASE("code editing keeps native clipboard selection and undo", "[editor][text]")
{
    CodeEditor editor;
    editor.setPlainText(QStringLiteral("first second"));
    SelectText(editor, 0, 5);
    editor.copy();
    CHECK(QApplication::clipboard()->text() == QStringLiteral("first"));
    editor.cut();
    CHECK(editor.toPlainText() == QStringLiteral(" second"));
    editor.undo();
    CHECK(editor.toPlainText() == QStringLiteral("first second"));
    editor.redo();
    CHECK(editor.toPlainText() == QStringLiteral(" second"));
}

TEST_CASE("selection indentation excludes an end at the next line start", "[editor][text]")
{
    CodeEditor editor;
    const QString original = QStringLiteral("one\ntwo\nthree");
    editor.setPlainText(original);
    SelectText(editor, 0, 8);
    editor.IndentSelection();
    CHECK(editor.toPlainText() == QStringLiteral("    one\n    two\nthree"));
    editor.undo();
    CHECK(editor.toPlainText() == original);
    editor.redo();
    editor.selectAll();
    editor.IndentSelection(true);
    CHECK(editor.toPlainText() == original);
}

TEST_CASE("Tab aligns to a stop and Backtab removes existing indentation", "[editor][text]")
{
    CodeEditor editor;
    editor.setPlainText(QStringLiteral("ab"));
    editor.moveCursor(QTextCursor::End);
    QTest::keyClick(&editor, Qt::Key_Tab);
    CHECK(editor.toPlainText() == QStringLiteral("ab  "));
    editor.setPlainText(QStringLiteral("\tvalue\n  value"));
    editor.selectAll();
    QTest::keyClick(&editor, Qt::Key_Backtab, Qt::ShiftModifier);
    CHECK(editor.toPlainText() == QStringLiteral("value\nvalue"));
    editor.undo();
    CHECK(editor.toPlainText() == QStringLiteral("\tvalue\n  value"));
}

TEST_CASE("newline indentation and committed IME text preserve native undo", "[editor][text][ime]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    editor.setPlainText(QStringLiteral("    if (value) {"));
    editor.moveCursor(QTextCursor::End);
    QTest::keyClick(&editor, Qt::Key_Return);
    CHECK(editor.toPlainText() == QStringLiteral("    if (value) {\n        "));
    editor.undo();
    CHECK(editor.toPlainText() == QStringLiteral("    if (value) {"));
    editor.setPlainText({});
    QInputMethodEvent preedit(QStringLiteral("候"), {});
    QApplication::sendEvent(&editor, &preedit);
    CHECK(editor.toPlainText().isEmpty() == true);
    QInputMethodEvent commit;
    const QString text = QString::fromUtf8("文字😀");
    commit.setCommitString(text);
    QApplication::sendEvent(&editor, &commit);
    CHECK(editor.toPlainText() == text);
    editor.undo();
    CHECK(editor.toPlainText().isEmpty() == true);
}

TEST_CASE("line comments preserve indentation and undo as one command", "[editor][text]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    const QString original = QStringLiteral("  float value;\n\n\treturn value;");
    editor.setPlainText(original);
    editor.selectAll();
    editor.ToggleLineComment();
    CHECK(editor.toPlainText() == QStringLiteral("  // float value;\n\n\t// return value;"));
    editor.undo();
    CHECK(editor.toPlainText() == original);
    editor.redo();
    editor.selectAll();
    editor.ToggleLineComment();
    CHECK(editor.toPlainText() == original);
    editor.SetLanguage(CodeLanguage::PlainText);
    editor.ToggleLineComment();
    CHECK(editor.toPlainText() == original);
}

TEST_CASE("whole line commands handle selection boundaries and the final line", "[editor][text]")
{
    CodeEditor editor;
    const QString original = QStringLiteral("one\ntwo\nthree");
    editor.setPlainText(original);
    SECTION("duplicate selected lines")
    {
        SelectText(editor, 0, 8);
        editor.DuplicateLines();
        CHECK(editor.toPlainText() == QStringLiteral("one\ntwo\none\ntwo\nthree"));
    }
    SECTION("move selected lines down")
    {
        SelectText(editor, 0, 8);
        editor.MoveLines(true);
        CHECK(editor.toPlainText() == QStringLiteral("three\none\ntwo"));
        editor.MoveLines(false);
        CHECK(editor.toPlainText() == original);
        editor.MoveLines(false);
        CHECK(editor.toPlainText() == original);
        return;
    }
    SECTION("move final line up")
    {
        editor.moveCursor(QTextCursor::End);
        editor.MoveLines(false);
        CHECK(editor.toPlainText() == QStringLiteral("one\nthree\ntwo"));
    }
    SECTION("delete final line")
    {
        editor.moveCursor(QTextCursor::End);
        editor.DeleteLines();
        CHECK(editor.toPlainText() == QStringLiteral("one\ntwo"));
    }
    SECTION("delete middle line")
    {
        editor.GoToLine(2);
        editor.DeleteLines();
        CHECK(editor.toPlainText() == QStringLiteral("one\nthree"));
    }
    SECTION("delete all lines")
    {
        editor.selectAll();
        editor.DeleteLines();
        CHECK(editor.toPlainText().isEmpty() == true);
    }
    editor.undo();
    CHECK(editor.toPlainText() == original);
}

TEST_CASE("read-only code buffers reject all modifying commands", "[editor][text]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    const QString original = QStringLiteral("float value;\nreturn value;");
    editor.setPlainText(original);
    editor.selectAll();
    editor.setReadOnly(true);
    editor.IndentSelection();
    editor.IndentSelection(true);
    editor.ToggleLineComment();
    editor.DuplicateLines();
    editor.MoveLines(true);
    editor.DeleteLines();
    QTest::keyClick(&editor, Qt::Key_Return);
    CHECK(editor.toPlainText() == original);
    CHECK(editor.document()->isUndoAvailable() == false);
}

TEST_CASE("Slang lexical state crosses lines and excludes strings from bracket matching", "[editor][text]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    editor.setPlainText(
        QStringLiteral("float4 Main() { /* }\nstill comment ] */\n\"} \\\" [\"; // )\nreturn value;\n}"));
    FinishHighlighting();
    const QTextBlock first = editor.document()->firstBlock();
    const QTextBlock second = first.next();
    const QTextBlock third = second.next();
    REQUIRE(first.userData() != nullptr);
    REQUIRE(second.userData() != nullptr);
    REQUIRE(third.userData() != nullptr);
    CHECK(static_cast<CodeBlockData*>(first.userData())->Brackets.size() == 3);
    CHECK(static_cast<CodeBlockData*>(second.userData())->Brackets.isEmpty() == true);
    CHECK(static_cast<CodeBlockData*>(third.userData())->Brackets.isEmpty() == true);
    CHECK(first.userState() == 1);
    CHECK(second.userState() == 0);
    CHECK(first.layout()->formats().isEmpty() == false);
    editor.GoToLine(5);
    FinishHighlighting();
    REQUIRE(editor.extraSelections().size() == 3);
    CHECK(editor.extraSelections().at(1).cursor.selectedText() == QStringLiteral("}"));
    CHECK(editor.extraSelections().at(2).cursor.selectedText() == QStringLiteral("{"));
    // Removing the opening comment invalidates the following block's state.
    SelectText(editor, 16, 18);
    editor.textCursor().removeSelectedText();
    FinishHighlighting();
    CHECK(editor.document()->firstBlock().userState() == 0);
    editor.undo();
    FinishHighlighting();
    CHECK(editor.document()->firstBlock().userState() == 1);
}

TEST_CASE("switching a lexical profile preserves contents history and selection", "[editor][text]")
{
    CodeEditor editor;
    editor.setPlainText(QStringLiteral("float value"));
    editor.moveCursor(QTextCursor::End);
    editor.insertPlainText(QStringLiteral(";"));
    const int32 position = editor.textCursor().position();
    editor.SetLanguage(CodeLanguage::Slang);
    FinishHighlighting();
    CHECK(editor.textCursor().position() == position);
    CHECK(editor.toPlainText() == QStringLiteral("float value;"));
    editor.SetLanguage(CodeLanguage::PlainText);
    FinishHighlighting();
    CHECK(editor.document()->firstBlock().layout()->formats().isEmpty() == true);
    editor.undo();
    CHECK(editor.toPlainText() == QStringLiteral("float value"));
}

TEST_CASE("find wraps and replace-all is finite and one undo group", "[editor][text][search]")
{
    CodeEditor* editor = new CodeEditor;
    CodeEditorPanel panel(editor);
    editor->setPlainText(QStringLiteral("value VALUE value"));
    TriggerAction(*editor, QStringLiteral("Replace"));
    QLineEdit* findOrNull = panel.findChild<QLineEdit*>(QStringLiteral("codeEditorFind"));
    QLineEdit* replacementOrNull = panel.findChild<QLineEdit*>(QStringLiteral("codeEditorReplacement"));
    QPushButton* replaceOrNull = panel.findChild<QPushButton*>(QStringLiteral("codeEditorReplaceAll"));
    REQUIRE(findOrNull != nullptr);
    REQUIRE(replacementOrNull != nullptr);
    REQUIRE(replaceOrNull != nullptr);
    findOrNull->setText(QStringLiteral("value"));
    replacementOrNull->setText(QStringLiteral("valuevalue"));
    replaceOrNull->click();
    CHECK(editor->toPlainText() == QStringLiteral("valuevalue valuevalue valuevalue"));
    editor->undo();
    CHECK(editor->toPlainText() == QStringLiteral("value VALUE value"));
    editor->moveCursor(QTextCursor::End);
    QTest::keyClick(findOrNull, Qt::Key_Return);
    CHECK(editor->textCursor().selectionStart() == 0);
    CHECK(editor->textCursor().selectedText() == QStringLiteral("value"));
    findOrNull->setText(QStringLiteral("absent"));
    QTest::keyClick(findOrNull, Qt::Key_Return);
    QLabel* statusOrNull = panel.findChild<QLabel*>(QStringLiteral("codeEditorFindStatus"));
    REQUIRE(statusOrNull != nullptr);
    CHECK(statusOrNull->text() == QStringLiteral("No matches"));
    editor->setReadOnly(true);
    findOrNull->setText(QStringLiteral("value"));
    replaceOrNull->click();
    CHECK(editor->toPlainText() == QStringLiteral("value VALUE value"));
}

TEST_CASE("go to line clamps and the gutter grows with the line count", "[editor][text]")
{
    CodeEditor editor;
    editor.setPlainText(QStringLiteral("one\ntwo\nthree"));
    QWidget* gutterOrNull = editor.findChild<QWidget*>(QStringLiteral("codeEditorGutter"));
    REQUIRE(gutterOrNull != nullptr);
    const int32 initialWidth = gutterOrNull->width();
    editor.GoToLine(100);
    CHECK(editor.textCursor().blockNumber() == 2);
    editor.GoToLine(0);
    CHECK(editor.textCursor().blockNumber() == 0);
    editor.setPlainText(QString(100, QLatin1Char('\n')));
    CHECK(gutterOrNull->width() > initialWidth);
    editor.resize(640, 480);
    editor.show();
    FinishHighlighting();
    CHECK(editor.grab().isNull() == false);
}

TEST_CASE("bracket matching rejects mismatched nesting and bounds distant matches", "[editor][text]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    SECTION("mismatched nesting")
    {
        editor.setPlainText(QStringLiteral("([)]"));
    }
    SECTION("distant match")
    {
        constexpr int32 DISTANT_BRACKET_CHARACTERS = 40000;
        editor.setPlainText(QLatin1Char('(') + QString(DISTANT_BRACKET_CHARACTERS, QLatin1Char(' ')) +
                            QLatin1Char(')'));
    }
    SelectText(editor, 0, 0);
    FinishHighlighting();
    CHECK(editor.extraSelections().size() == 1);
}

TEST_CASE("shader-size text input records an offscreen latency baseline", "[.benchmark][editor][text]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    QString source;
    SECTION("many ordinary lines")
    {
        const QString line = QStringLiteral("float4 Shade(float4 value) { return value; }\n");
        while (source.size() + line.size() <= RenderGraphDocument::MAX_FILE_BYTES)
        {
            source += line;
        }
    }
    SECTION("one long line")
    {
        source = QString(RenderGraphDocument::MAX_FILE_BYTES - 1, QLatin1Char('x'));
    }
    editor.resize(800, 600);
    editor.setPlainText(source);
    editor.show();
    editor.GoToLine(editor.blockCount() / 2);
    editor.moveCursor(QTextCursor::EndOfBlock);
    FinishHighlighting();
    constexpr int32 SAMPLE_COUNT = 20;
    QVector<int64> samples;
    for (int32 sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        QElapsedTimer timer;
        timer.start();
        editor.insertPlainText(QStringLiteral("x"));
        FinishHighlighting();
        samples.append(timer.nsecsElapsed());
        editor.undo();
        FinishHighlighting();
    }
    std::sort(samples.begin(), samples.end());
    INFO("Offscreen synthetic baseline; includes Qt event processing, excludes native input/compositor latency.");
    INFO("System: " << QSysInfo::prettyProductName().toStdString() << " / "
                    << QSysInfo::currentCpuArchitecture().toStdString() << "; Qt " << qVersion());
    INFO("Characters=" << source.size() << ", blocks=" << editor.blockCount() << ", edit nanoseconds p50="
                       << samples.at(SAMPLE_COUNT / 2) << ", p95=" << samples.at(SAMPLE_COUNT - 1));
    CHECK(editor.toPlainText().compare(source) == 0);
}

TEST_CASE("plain Qt long-line input records the comparison baseline", "[.benchmark][editor][text]")
{
    QPlainTextEdit editor;
    editor.setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    editor.setLineWrapMode(QPlainTextEdit::NoWrap);
    const QString source(RenderGraphDocument::MAX_FILE_BYTES - 1, QLatin1Char('x'));
    editor.resize(800, 600);
    editor.setPlainText(source);
    editor.show();
    editor.moveCursor(QTextCursor::End);
    FinishHighlighting();
    constexpr int32 SAMPLE_COUNT = 20;
    QVector<int64> samples;
    for (int32 sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        QElapsedTimer timer;
        timer.start();
        editor.insertPlainText(QStringLiteral("x"));
        FinishHighlighting();
        samples.append(timer.nsecsElapsed());
        editor.undo();
        FinishHighlighting();
    }
    std::sort(samples.begin(), samples.end());
    INFO("Plain Qt comparison, same font and NoWrap, characters=" << source.size() << ", nanoseconds p50="
                                                                  << samples.at(SAMPLE_COUNT / 2)
                                                                  << ", p95=" << samples.at(SAMPLE_COUNT - 1));
    CHECK(editor.toPlainText().compare(source) == 0);
}

TEST_CASE("native shortcut dispatch reaches the shared find command", "[editor][text][keyboard]")
{
    CodeEditor editor;
    QSignalSpy requested(&editor, &CodeEditor::FindRequested);
    editor.show();
    editor.activateWindow();
    editor.setFocus();
    FinishHighlighting();
    QTest::keySequence(&editor, QKeySequence(QKeySequence::Find));
    CHECK(requested.count() == 1);
}
