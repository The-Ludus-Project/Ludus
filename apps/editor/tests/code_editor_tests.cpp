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
#include <QFontInfo>
#include <QHelpEvent>
#include <QInputMethodEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QSysInfo>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QToolTip>

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

TEST_CASE("diagnostics map scalar columns, points and whole lines into the text", "[editor][text][diagnostics]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    // U+1F600 occupies two UTF-16 units but one scalar column.
    editor.setPlainText(QStringLiteral("float a = 1;\n/*\U0001F600*/ bad;\n    return\n\n"));
    editor.SetDiagnostics(
        {
            { .Line = 2, .Column = 7, .Length = 3, .Message = QStringLiteral("undefined identifier 'bad'.") },
            { .Line = 3, .Column = 11, .Severity = CodeDiagnosticSeverity::Warning, .Message = QStringLiteral("eol") },
            { .Line = 1, .Message = QStringLiteral("line only") },
            { .Line = 4, .Message = QStringLiteral("empty line") },
            { .Line = 99, .Message = QStringLiteral("outside") },
            { .Line = 0, .Message = QStringLiteral("no line") },
        },
        true);
    REQUIRE(editor.GetDiagnosticCount() == 4);
    CHECK(editor.GetDiagnosticRange(0).selectedText() == QStringLiteral("bad"));
    CHECK(editor.GetDiagnosticRange(1).hasSelection() == false);
    CHECK(editor.GetDiagnosticRange(1).atBlockEnd() == true);
    CHECK(editor.GetDiagnosticRange(2).selectedText() == QStringLiteral("float a = 1;"));
    CHECK(editor.GetDiagnosticRange(4).isNull() == true);
    FinishHighlighting();
    int32 underlines = 0;
    for (const QTextEdit::ExtraSelection& selection : editor.extraSelections())
    {
        underlines += selection.format.underlineStyle() == QTextCharFormat::WaveUnderline ? 1 : 0;
    }
    // The empty line keeps only its gutter marker; the point underlines its preceding character.
    CHECK(underlines == 3);
    const int32 badStart = editor.GetDiagnosticRange(0).selectionStart();
    CHECK(editor.DescribeDiagnosticsAt(badStart + 1) == QStringLiteral("Error: undefined identifier 'bad'."));
    CHECK(editor.DescribeDiagnosticsAt(badStart - 3).isEmpty() == true);
    CHECK(editor.DescribeDiagnosticsAt(editor.GetDiagnosticRange(1).position() - 1) == QStringLiteral("Warning: eol"));
    CHECK(editor.DescribeLineDiagnostics(3) == QStringLiteral("Error: empty line"));
    editor.GoToLine(2, 7);
    CHECK(editor.textCursor().position() == badStart);
    editor.GoToLine(2, 100);
    CHECK(editor.textCursor().atBlockEnd() == true);
}

TEST_CASE("diagnostics follow edits and go out of date only when text changes", "[editor][text][diagnostics]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    editor.setPlainText(QStringLiteral("float v = missing;"));
    editor.SetDiagnostics({{ .Line = 1, .Column = 11, .Length = 7, .Message = QStringLiteral("undefined") }}, true);
    FinishHighlighting();
    // Lexical reformatting reports same-length document changes without editing text.
    editor.SetLanguage(CodeLanguage::PlainText);
    editor.SetLanguage(CodeLanguage::Slang);
    editor.setPalette(QPalette(Qt::black));
    FinishHighlighting();
    CHECK(editor.AreDiagnosticsCurrent() == true);
    QTextCursor cursor(editor.document());
    cursor.insertText(QStringLiteral("// note\n"));
    CHECK(editor.AreDiagnosticsCurrent() == false);
    REQUIRE(editor.GetDiagnosticCount() == 1);
    CHECK(editor.GetDiagnosticRange(0).selectedText() == QStringLiteral("missing"));
    CHECK(editor.GetDiagnosticRange(0).block().blockNumber() == 1);
    CHECK(editor.DescribeLineDiagnostics(1).startsWith(QStringLiteral("Out of date:")) == true);
    FinishHighlighting();
    // An out-of-date report keeps its squiggle, fainter, so the error can still be found.
    bool hasFaintSquiggle = false;
    for (const QTextEdit::ExtraSelection& selection : editor.extraSelections())
    {
        hasFaintSquiggle = hasFaintSquiggle || (selection.format.underlineStyle() == QTextCharFormat::WaveUnderline &&
                                                selection.format.underlineColor().alpha() < 255);
    }
    CHECK(hasFaintSquiggle == true);

    SECTION("a same-length replacement is an edit")
    {
        editor.SetDiagnostics({{ .Line = 2, .Column = 11, .Length = 7, .Message = QStringLiteral("again") }}, true);
        QTextCursor replace(editor.document());
        replace.setPosition(0);
        replace.setPosition(1, QTextCursor::KeepAnchor);
        replace.insertText(QStringLiteral("#"));
        CHECK(editor.AreDiagnosticsCurrent() == false);
    }
    SECTION("replacing the whole text removes the markers")
    {
        editor.setPlainText(QStringLiteral("other file"));
        CHECK(editor.GetDiagnosticCount() == 0);
    }
    SECTION("a report for an earlier version starts out of date")
    {
        editor.SetDiagnostics({{ .Line = 1, .Message = QStringLiteral("old") }}, false);
        CHECK(editor.AreDiagnosticsCurrent() == false);
        editor.SetDiagnostics({}, true);
        CHECK(editor.GetDiagnosticCount() == 0);
        CHECK(editor.DescribeLineDiagnostics(0).isEmpty() == true);
    }
}

TEST_CASE("the caret line's diagnostic message is drawn under the line until that text is edited",
          "[editor][text][diagnostics]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    editor.setPlainText(QStringLiteral("float a;\nfloat b = bad;\nfloat c;\nfloat d;"));
    editor.resize(480, 260);
    editor.show();
    editor.SetDiagnostics(
        {{ .Line = 2, .Column = 11, .Length = 3, .Message = QStringLiteral("undefined identifier 'bad'.") }},
        true);
    editor.GoToLine(2, 11);
    FinishHighlighting();
    const QTextCursor nextLine(editor.document()->findBlockByNumber(2));
    const QRect nextLineCursor = editor.cursorRect(nextLine);
    const QRect below(0, nextLineCursor.top(), editor.viewport()->width(), nextLineCursor.height());
    const auto grabBelow = [&editor, &below]() { return editor.viewport()->grab(below).toImage(); };
    const QImage withCallout = grabBelow();
    // The same text with no marker is the reference for what the line below normally looks like.
    editor.SetDiagnostics({}, true);
    FinishHighlighting();
    const QImage plain = grabBelow();
    CHECK(withCallout != plain);
    // A report that is already out of date still explains itself.
    editor.SetDiagnostics(
        {{ .Line = 2, .Column = 11, .Length = 3, .Message = QStringLiteral("undefined identifier 'bad'.") }},
        false);
    FinishHighlighting();
    CHECK(grabBelow() != plain);
    // Typing on that line makes way: the message describes text that is being replaced.
    editor.GoToLine(2, 11);
    QTextCursor cursor = editor.textCursor();
    cursor.insertText(QStringLiteral("x"));
    FinishHighlighting();
    CHECK(grabBelow() == plain);
    // Another line has no message to show.
    editor.SetDiagnostics(
        {{ .Line = 2, .Column = 11, .Length = 3, .Message = QStringLiteral("undefined identifier 'bad'.") }},
        true);
    editor.GoToLine(4, 1);
    FinishHighlighting();
    CHECK(editor.viewport()->grab(below).toImage() == plain);
}

TEST_CASE("diagnostic hover shows escaped messages over text and gutter", "[editor][text][diagnostics]")
{
    CodeEditor editor;
    editor.setPlainText(QStringLiteral("vector<float, 3> v = bad;\nfine"));
    editor.SetDiagnostics({
    {
        .Line = 1,
        .Column = 22,
        .Length = 3,
        .Message = QStringLiteral("vector<float, 3> & bad"),
    }},
                          true);
    editor.resize(640, 480);
    editor.show();
    FinishHighlighting();
    QTextCursor inside(editor.document());
    inside.setPosition(22);
    const QRect caret = editor.cursorRect(inside);
    QHelpEvent textHelp(QEvent::ToolTip, caret.center(), editor.viewport()->mapToGlobal(caret.center()));
    QCoreApplication::sendEvent(editor.viewport(), &textHelp);
    CHECK(QToolTip::text().contains(QStringLiteral("vector&lt;float, 3&gt; &amp; bad")) == true);
    QToolTip::hideText();
    QWidget* gutterOrNull = editor.findChild<QWidget*>(QStringLiteral("codeEditorGutter"));
    REQUIRE(gutterOrNull != nullptr);
    const QPoint line(gutterOrNull->width() / 2, caret.center().y());
    QHelpEvent gutterHelp(QEvent::ToolTip, line, gutterOrNull->mapToGlobal(line));
    QCoreApplication::sendEvent(gutterOrNull, &gutterHelp);
    CHECK(QToolTip::text().contains(QStringLiteral("Error: vector&lt;float")) == true);
    QToolTip::hideText();
    const QPoint below(gutterOrNull->width() / 2,
                       editor.cursorRect(QTextCursor(editor.document()->lastBlock())).center().y());
    QHelpEvent emptyHelp(QEvent::ToolTip, below, gutterOrNull->mapToGlobal(below));
    QCoreApplication::sendEvent(gutterOrNull, &emptyHelp);
    CHECK(emptyHelp.isAccepted() == false);
    CHECK(editor.grab().isNull() == false);
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

TEST_CASE("presentation preferences preserve source selection and undo", "[editor][text][presentation]")
{
    CodeEditor* editor = new CodeEditor;
    CodeEditorPanel panel(editor);
    for (const QString& family : QFontDatabase::families())
    {
        if (QFontDatabase::isFixedPitch(family) == true)
        {
            CHECK(QFontInfo(editor->font()).fixedPitch() == true);
            break;
        }
    }
    editor->setPlainText(QStringLiteral("    float4 Color();"));
    SelectText(*editor, 4, 10);
    editor->insertPlainText(QStringLiteral("float3"));
    const QTextCursor selection = editor->textCursor();
    const QString text = editor->toPlainText();
    editor->SetFontPointSize(18);
    editor->SetWordWrap(true);
    editor->SetIndentGuidesVisible(false);
    CHECK(editor->toPlainText() == text);
    CHECK(editor->textCursor().position() == selection.position());
    CHECK(editor->CanUndo() == true);
    QSpinBox* size = panel.findChild<QSpinBox*>(QStringLiteral("codeEditorTextSize"));
    REQUIRE(size != nullptr);
    CHECK(size->value() == 18);
    CHECK(editor->lineWrapMode() == QPlainTextEdit::WidgetWidth);
    CodeEditor reopened;
    CHECK(reopened.font().pointSize() == 18);
    CHECK(reopened.lineWrapMode() == QPlainTextEdit::WidgetWidth);
    CHECK(reopened.property("indentGuidesVisible").toBool() == false);
    editor->SetFontPointSize(1000);
    CHECK(editor->font().pointSize() == CodeEditor::MAXIMUM_FONT_POINTS);
    TriggerAction(*editor, QStringLiteral("Reset Text Size"));
    CHECK(size->value() == CodeEditor::DEFAULT_FONT_POINTS);
    editor->Undo();
    CHECK(editor->toPlainText() == QStringLiteral("    float4 Color();"));
    editor->SetWordWrap(false);
    editor->SetIndentGuidesVisible(true);
}

TEST_CASE("diagnostic navigation orders unsorted messages and wraps without changing text",
          "[editor][text][diagnostics]")
{
    CodeEditor* editor = new CodeEditor;
    CodeEditorPanel panel(editor);
    editor->setPlainText(QStringLiteral("one\ntwo\nthree"));
    editor->SetDiagnostics({{3, 1, 1, CodeDiagnosticSeverity::Warning, QStringLiteral("third")},
                            {1, 2, 1, CodeDiagnosticSeverity::Error, QStringLiteral("first")}},
                           true);
    editor->GoToLine(1);
    editor->GoToDiagnostic();
    CHECK(editor->textCursor().selectionStart() == 1);
    editor->GoToDiagnostic();
    CHECK(editor->textCursor().blockNumber() == 2);
    editor->GoToDiagnostic();
    CHECK(editor->textCursor().selectionStart() == 1);
    editor->GoToDiagnostic(true);
    CHECK(editor->textCursor().blockNumber() == 2);
    editor->insertPlainText(QStringLiteral("changed"));
    QPushButton* issues = panel.findChild<QPushButton*>(QStringLiteral("codeEditorDiagnostics"));
    REQUIRE(issues != nullptr);
    CHECK(issues->text().contains(QStringLiteral("stale")) == true);
    editor->SetDiagnostics({}, true);
    CHECK(issues->isEnabled() == false);
}

TEST_CASE("automatic bracket pairs respect comments strings and native undo", "[editor][text][keyboard]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    QTest::keyClicks(&editor, QStringLiteral("("));
    CHECK(editor.toPlainText() == QStringLiteral("()"));
    CHECK(editor.textCursor().position() == 1);
    editor.Undo();
    CHECK(editor.toPlainText().isEmpty() == true);
    editor.Redo();
    SelectText(editor, 1, 1);
    QTest::keyClick(&editor, Qt::Key_Backspace);
    CHECK(editor.toPlainText().isEmpty() == true);
    QTest::keyClicks(&editor, QStringLiteral("[]"));
    CHECK(editor.toPlainText() == QStringLiteral("[]"));
    CHECK(editor.textCursor().position() == 2);
    for (const QString& prefix :
         {QStringLiteral("// comment "), QStringLiteral("/* comment "), QStringLiteral("\"string ")})
    {
        editor.setPlainText(prefix);
        editor.moveCursor(QTextCursor::End);
        QTest::keyClicks(&editor, QStringLiteral("("));
        CHECK(editor.toPlainText() == prefix + QStringLiteral("("));
    }
    editor.setPlainText(QStringLiteral("\"closed\" "));
    editor.moveCursor(QTextCursor::End);
    QTest::keyClicks(&editor, QStringLiteral("{"));
    CHECK(editor.toPlainText() == QStringLiteral("\"closed\" {}"));
    QTest::keyClick(&editor, Qt::Key_Return);
    CHECK(editor.toPlainText() == QStringLiteral("\"closed\" {\n    \n}"));
    CHECK(editor.textCursor().blockNumber() == 1);
    CHECK(editor.textCursor().positionInBlock() == 4);
    editor.Undo();
    CHECK(editor.toPlainText() == QStringLiteral("\"closed\" {}"));
    editor.setReadOnly(true);
    const QString before = editor.toPlainText();
    QTest::keyClicks(&editor, QStringLiteral("("));
    CHECK(editor.toPlainText() == before);
}
