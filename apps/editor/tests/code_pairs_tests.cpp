// Auto-closing pairs, surrounding pairs and smart indentation driven by the
// language registry (CE17). Offscreen; keys go through the production
// keyPressEvent via QTest so the view's structural editing runs. Plain text has
// no structural editing in the registry, so none of these behaviors apply to it.

#include "internal/code_editor.h"
#include "internal/code_language_registry.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>
#include <QTest>
#include <QTextCursor>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
void SelectRange(CodeEditor& inoutEditor, int anchor, int caret)
{
    QTextCursor cursor(inoutEditor.document());
    cursor.setPosition(anchor);
    cursor.setPosition(caret, QTextCursor::KeepAnchor);
    inoutEditor.setTextCursor(cursor);
}
} // namespace

TEST_CASE("Typing an opening bracket auto-closes it from the registry", "[editor][text][pairs]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    QTest::keyClicks(&editor, QStringLiteral("("));
    CHECK(editor.toPlainText() == QStringLiteral("()"));
    CHECK(editor.textCursor().position() == 1);
    // Typing the matching close steps over the auto-closed one, not a duplicate.
    QTest::keyClicks(&editor, QStringLiteral(")"));
    CHECK(editor.toPlainText() == QStringLiteral("()"));
    CHECK(editor.textCursor().position() == 2);
}

TEST_CASE("A quote auto-closes and steps over its own close", "[editor][text][pairs]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    QTest::keyClicks(&editor, QStringLiteral("\""));
    CHECK(editor.toPlainText() == QStringLiteral("\"\""));
    CHECK(editor.textCursor().position() == 1);
    QTest::keyClicks(&editor, QStringLiteral("\""));
    CHECK(editor.toPlainText() == QStringLiteral("\"\""));
    CHECK(editor.textCursor().position() == 2);
}

TEST_CASE("Backspace between a freshly typed pair deletes both", "[editor][text][pairs]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    QTest::keyClicks(&editor, QStringLiteral("["));
    CHECK(editor.toPlainText() == QStringLiteral("[]"));
    QTest::keyClick(&editor, Qt::Key_Backspace);
    CHECK(editor.toPlainText().isEmpty() == true);
}

TEST_CASE("Typing a bracket around a selection surrounds it", "[editor][text][pairs]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    editor.setPlainText(QStringLiteral("value"));
    SelectRange(editor, 0, 5);
    QTest::keyClicks(&editor, QStringLiteral("("));
    CHECK(editor.toPlainText() == QStringLiteral("(value)"));
    // The original text stays selected, now inside the pair.
    CHECK(editor.textCursor().selectedText() == QStringLiteral("value"));
    CHECK(editor.textCursor().selectionStart() == 1);
    CHECK(editor.textCursor().selectionEnd() == 6);
}

TEST_CASE("Typing a quote around a selection surrounds it", "[editor][text][pairs]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    editor.setPlainText(QStringLiteral("abc"));
    SelectRange(editor, 0, 3);
    QTest::keyClicks(&editor, QStringLiteral("\""));
    CHECK(editor.toPlainText() == QStringLiteral("\"abc\""));
    CHECK(editor.textCursor().selectedText() == QStringLiteral("abc"));
}

TEST_CASE("Return after an opening bracket indents and drops the close", "[editor][text][pairs]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    editor.setPlainText(QStringLiteral("void main() "));
    editor.moveCursor(QTextCursor::End);
    QTest::keyClicks(&editor, QStringLiteral("{"));
    CHECK(editor.toPlainText() == QStringLiteral("void main() {}"));
    QTest::keyClick(&editor, Qt::Key_Return);
    CHECK(editor.toPlainText() == QStringLiteral("void main() {\n    \n}"));
    CHECK(editor.textCursor().blockNumber() == 1);
    CHECK(editor.textCursor().positionInBlock() == 4);
}

TEST_CASE("Plain text has no structural editing", "[editor][text][pairs]")
{
    CodeEditor editor;
    // The default language is plain text; the registry marks it with no
    // structural editing, so brackets and quotes insert literally.
    REQUIRE(CodeLanguageRegistry::Get().Describe(CodeLanguage::PlainText).HasStructuralEditing == false);
    QTest::keyClicks(&editor, QStringLiteral("("));
    CHECK(editor.toPlainText() == QStringLiteral("("));
    editor.setPlainText(QStringLiteral("value"));
    SelectRange(editor, 0, 5);
    QTest::keyClicks(&editor, QStringLiteral("("));
    // The selection is replaced, not surrounded, in plain text.
    CHECK(editor.toPlainText() == QStringLiteral("("));
}
