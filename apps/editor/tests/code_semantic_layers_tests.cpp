#include "internal/code_editor.h"
#include "internal/code_semantic_layers.h"
#include "internal/code_structure_view.h"
#include "internal/code_syntax_highlighter.h"
#include "internal/code_syntax_service.h"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>

#include <QFontMetricsF>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
void WaitForSyntax(const CodeEditor& editor)
{
    const CodeSyntaxService* serviceOrNull = editor.document()->findChild<CodeSyntaxService*>();
    REQUIRE(serviceOrNull != nullptr);
    REQUIRE(QTest::qWaitFor([serviceOrNull]() { return serviceOrNull->GetCurrentResultOrNull() != nullptr; }));
}

/// The foreground the highlighter gave `position`, or an invalid color.
QColor GetForeground(const CodeEditor& editor, int32 position)
{
    const QTextBlock block = editor.document()->findBlock(position);
    const int32 column = position - block.position();
    for (const QTextLayout::FormatRange& range : block.layout()->formats())
    {
        if (range.start <= column && column < range.start + range.length)
        {
            return range.format.foreground().color();
        }
    }
    return {};
}

CodeSyntaxFold MakeFold(int32 first, int32 last, CodeFoldKind kind)
{
    CodeSyntaxFold fold;
    fold.FirstLine = first;
    fold.LastLine = last;
    fold.Kind = kind;
    return fold;
}

QJsonArray MakeArray(std::initializer_list<int32> values)
{
    QJsonArray array;
    for (const int32 value : values)
    {
        array.append(value);
    }
    return array;
}
} // namespace

TEST_CASE("Semantic tokens decode relative to the legend inside the authored source",
          "[editor][code-editor][assistance]")
{
    const QString prefix = QStringLiteral("// prefix\n");
    const QString source = QStringLiteral("float a = b;\nfloat c;\n");
    const QString unit = prefix + source;
    const CodeSemanticSource authored
    {
        .Start = static_cast<int32>(prefix.size()),
        .End = static_cast<int32>(unit.size()),
        .FirstLine = 1,
    };
    const QJsonObject legend{
        {QStringLiteral("tokenTypes"),
         QJsonArray{QStringLiteral("variable"), QStringLiteral("function"), QStringLiteral("unknownType")}},
        {QStringLiteral("tokenModifiers"), QJsonArray{QStringLiteral("declaration"), QStringLiteral("readonly")}},
    };
    const QJsonArray data = MakeArray({
        0,  3, 6, 0, 0, // "prefix": generated text is not authored
        1,  6, 1, 0, 2, // "a", readonly: a constant
        0,  4, 1, 1, 0, // "b": a function
        0,  0, 1, 1, 0, // overlaps "b"
        0,  1, 5, 0, 0, // runs past the end of its line
        1,  0, 5, 2, 0, // a type with no role keeps its syntax color
        0,  6, 1, 0, 0, // "c": a variable
        -1, 0, 1, 0, 0, // malformed: ends the decode
        0,  1, 1, 0, 0,
    });
    const QList<CodeSyntaxToken> tokens =
        DecodeSemanticTokens(data, legend, unit, LanguagePositionEncoding::Utf16, authored);
    REQUIRE(tokens.size() == 3);
    CHECK(tokens.at(0) == CodeSyntaxToken({6, 7}, CodeSyntaxRole::Constant));
    CHECK(tokens.at(1) == CodeSyntaxToken({10, 11}, CodeSyntaxRole::Function));
    CHECK(tokens.at(2) == CodeSyntaxToken({19, 20}, CodeSyntaxRole::Variable));

    // UTF-8 characters count bytes: "é" is two.
    const QString accented = QStringLiteral("é = a;");
    const QList<CodeSyntaxToken> utf8 = DecodeSemanticTokens(MakeArray({0, 5, 1, 0, 0}),
                                                             legend,
                                                             accented,
                                                             LanguagePositionEncoding::Utf8,
                                                             { .Start = 0, .End = 6, .FirstLine = 0 });
    REQUIRE(utf8.size() == 1);
    CHECK(utf8.at(0) == CodeSyntaxToken({4, 5}, CodeSyntaxRole::Variable));

    CodeSyntaxRole role = CodeSyntaxRole::Default;
    CHECK(ReadSemanticTokenRole(QStringLiteral("parameter"), false, role) == true);
    CHECK(role == CodeSyntaxRole::Parameter);
    CHECK(ReadSemanticTokenRole(QStringLiteral("label"), false, role) == false);
    CHECK(role == CodeSyntaxRole::Parameter);
}

TEST_CASE("Server folding ranges decode by kind inside the authored source", "[editor][code-editor][assistance]")
{
    const QString prefix = QStringLiteral("// p\n");
    const QString unit = prefix + QStringLiteral("a\nb\nc\nd\n");
    const CodeSemanticSource authored
    {
        .Start = static_cast<int32>(prefix.size()),
        .End = static_cast<int32>(unit.size()),
        .FirstLine = 1,
    };
    const auto range = [](int32 first, int32 last, const QString& kind = {}) {
        QJsonObject value{{QStringLiteral("startLine"), first}, {QStringLiteral("endLine"), last}};
        if (kind.isEmpty() == false)
        {
            value.insert(QStringLiteral("kind"), kind);
        }
        return value;
    };
    QJsonObject comment = range(2, 4, QStringLiteral("comment"));
    comment.insert(QStringLiteral("collapsedText"), QStringLiteral("notes"));
    const QList<CodeSyntaxFold> folds = DecodeFoldingRanges(QJsonArray{range(1, 3, QStringLiteral("imports")),
                                                                       range(0, 2, QStringLiteral("imports")),
                                                                       range(2, 2),
                                                                       range(2, 9),
                                                                       comment,
                                                                       range(3, 4, QStringLiteral("other"))},
                                                            unit,
                                                            authored);
    REQUIRE(folds.size() == 3);
    CHECK(folds.at(0).FirstLine == 0);
    CHECK(folds.at(0).LastLine == 2);
    CHECK(folds.at(0).Kind == CodeFoldKind::Imports);
    CHECK(folds.at(1).Kind == CodeFoldKind::Comment);
    CHECK(folds.at(1).Label == QStringLiteral("notes"));
    CHECK(folds.at(2).FirstLine == 2);
    CHECK(folds.at(2).Kind == CodeFoldKind::Syntax);
    CHECK(folds.at(2).IsFromServer == true);
}

TEST_CASE("Semantic tokens color over syntax and move with edits", "[editor][code-editor][assistance]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    const QString source = QStringLiteral("float value = Call(1);\nvalue = 2;\n");
    editor.setPlainText(source);
    editor.resize(600, 300);
    editor.show();
    WaitForSyntax(editor);
    const int32 call = static_cast<int32>(source.indexOf(QStringLiteral("Call")));
    const int32 value = static_cast<int32>(source.lastIndexOf(QStringLiteral("value")));
    REQUIRE(QTest::qWaitFor([&editor, call]() { return GetForeground(editor, call).isValid() == true; }));
    const QColor function = GetForeground(editor, call);
    CHECK(GetForeground(editor, value) != function);

    editor.SetSemanticTokens({CodeSyntaxToken({value, value + 5}, CodeSyntaxRole::Function)});
    REQUIRE(QTest::qWaitFor([&editor, value, function]() { return GetForeground(editor, value) == function; }));
    CHECK(editor.toPlainText() == source);
    CHECK(editor.document()->isUndoAvailable() == false);

    // Text inserted before a token moves it; at its end, it stays as it was.
    QTextCursor cursor(editor.document());
    cursor.insertText(QStringLiteral("x"));
    REQUIRE(editor.GetSemanticTokens().size() == 1);
    CHECK(editor.GetSemanticTokens().at(0).GetStart() == value + 1);
    cursor.setPosition(value + 1 + 5);
    cursor.insertText(QStringLiteral("s"));
    REQUIRE(editor.GetSemanticTokens().size() == 1);
    CHECK(editor.GetSemanticTokens().at(0).GetEnd() == value + 1 + 5);
    // An edit inside a token drops it until the server classifies the text again.
    cursor.setPosition(value + 3);
    cursor.insertText(QStringLiteral("y"));
    CHECK(editor.GetSemanticTokens().isEmpty() == true);
    // A language change clears them.
    editor.SetSemanticTokens({CodeSyntaxToken({0, 1}, CodeSyntaxRole::Function)});
    editor.SetLanguage(CodeLanguage::Cpp);
    CHECK(editor.GetSemanticTokens().isEmpty() == true);
}

TEST_CASE("Server folding ranges add only kinds the grammar lacks and keep collapsed ranges",
          "[editor][code-editor][assistance][syntax]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    const QString source =
        QStringLiteral("int Run()\n{\n  return 1;\n}\nint a;\nint b;\nint c;\nint d;\nint e;\nint f;\n");
    editor.setPlainText(source);
    WaitForSyntax(editor);
    CodeStructureView& structure = editor.GetStructure();
    REQUIRE(structure.HasFold(1) == true);
    REQUIRE(structure.HasFold(4) == false);
    structure.SetServerFolds({
        MakeFold(4, 6, CodeFoldKind::Imports),
        MakeFold(7, 9, CodeFoldKind::Syntax),   // the grammar's kind
        MakeFold(1, 3, CodeFoldKind::Imports),  // a syntax range starts there
        MakeFold(8, 20, CodeFoldKind::Imports), // past the document
    });
    const QList<CodeSyntaxFold> offered = structure.GetServerFolds();
    REQUIRE(offered.size() == 1);
    CHECK(offered.at(0).FirstLine == 4);
    CHECK(offered.at(0).Kind == CodeFoldKind::Imports);
    CHECK(structure.HasFold(7) == false);

    structure.ToggleFold(4);
    CHECK(editor.document()->findBlockByNumber(5).isVisible() == false);
    // The server range moves with an edit above it and stays collapsed through
    // the next syntax result, before the server answers again.
    QTextCursor cursor(editor.document());
    cursor.insertText(QStringLiteral("int zero;\n"));
    WaitForSyntax(editor);
    CHECK(structure.GetHiddenLineCount(5) == 2);
    CHECK(editor.toPlainText() == QStringLiteral("int zero;\n") + source);
    // Server ranges leave with the server; Unfold All always reveals everything.
    structure.SetServerFolds({});
    CHECK(structure.GetServerFolds().isEmpty() == true);
    CHECK(editor.document()->findBlockByNumber(6).isVisible() == true);
}

TEST_CASE("Inlay hints decode labels and positions inside the authored source", "[editor][code-editor][assistance]")
{
    const QString prefix = QStringLiteral("// p\n");
    const QString source = QStringLiteral("Scale(a, b);\n");
    const QString unit = prefix + source;
    const CodeSemanticSource authored
    {
        .Start = static_cast<int32>(prefix.size()),
        .End = static_cast<int32>(unit.size()),
        .FirstLine = 1,
    };
    const auto hint = [](int32 line, int32 character, const QJsonValue& label, int32 kind) {
        return QJsonObject{
            {QStringLiteral("position"),
             QJsonObject{{QStringLiteral("line"), line}, {QStringLiteral("character"), character}}},
            {QStringLiteral("label"), label},
            {QStringLiteral("kind"), kind},
        };
    };
    QJsonObject padded = hint(1, 6, QStringLiteral("value:"), 2);
    padded.insert(QStringLiteral("paddingRight"), true);
    const QList<CodeInlayHint> hints = DecodeInlayHints(
        QJsonArray{
            hint(1,
                 9,
                 QJsonArray{QJsonObject{{QStringLiteral("value"), QStringLiteral("fac")}},
                            QJsonObject{{QStringLiteral("value"), QStringLiteral("tor:")}}},
                 2), // out of order, from parts
            padded,
            hint(0, 2, QStringLiteral("generated"), 1),  // in the prefix
            hint(1, 1, QStringLiteral(" "), 1),          // nothing to show
            hint(1, 2, QStringLiteral("two\nlines"), 1), // spans lines
            hint(1, 11, QString(CodeSemanticLayers::MAX_INLAY_HINT_LABEL_UNITS + 1, QLatin1Char('x')), 1),
        },
        unit,
        LanguagePositionEncoding::Utf16,
        authored);
    REQUIRE(hints.size() == 3);
    CHECK(hints.at(0).Position == 6);
    CHECK(hints.at(0).Label == QStringLiteral("value:"));
    CHECK(hints.at(0).Kind == CodeInlayHintKind::Parameter);
    CHECK(hints.at(0).HasPaddingRight == true);
    CHECK(hints.at(1).Position == 9);
    CHECK(hints.at(1).Label == QStringLiteral("factor:"));
    CHECK(hints.at(2).Kind == CodeInlayHintKind::Type);
    CHECK(hints.at(2).Label.size() == CodeSemanticLayers::MAX_INLAY_HINT_LABEL_UNITS);
    CHECK(hints.at(2).Label.endsWith(QChar(0x2026)) == true);
}

TEST_CASE("Inlay hints reserve space without changing the text and follow the setting",
          "[editor][code-editor][assistance]")
{
    const bool wasVisible = QSettings().value(QStringLiteral("codeEditor/inlayHints"), true).toBool();
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    const QString source = QStringLiteral("Scale(a, b);\nb;\n");
    editor.setPlainText(source);
    editor.resize(600, 300);
    editor.show();
    editor.setFocus();
    editor.SetInlayHintsVisible(true);
    WaitForSyntax(editor);
    const auto getX = [&editor](int32 position) {
        QTextCursor cursor(editor.document());
        cursor.setPosition(position);
        return editor.cursorRect(cursor).left();
    };
    const int32 argument = static_cast<int32>(source.indexOf(QLatin1Char('a')));
    const int32 lineStart = static_cast<int32>(source.lastIndexOf(QLatin1Char('b')));
    const int32 plainArgument = getX(argument);
    const int32 plainLineStart = getX(lineStart);
    CodeInlayHint value;
    value.Position = argument;
    value.Label = QStringLiteral("value:");
    value.Kind = CodeInlayHintKind::Parameter;
    value.HasPaddingRight = true;
    CodeInlayHint start = value;
    start.Position = lineStart;
    editor.SetInlayHints({value, start});
    const float64 width = MeasureCodeInlayHint(QFontMetricsF(editor.document()->defaultFont()), value);
    REQUIRE(QTest::qWaitFor([&]() { return getX(argument) > plainArgument; }));
    CHECK(qAbs(getX(argument) - plainArgument - width) <= 1.5);
    // A hint at a line start has no character before it to reserve space after.
    CHECK(getX(lineStart) == plainLineStart);
    CHECK(editor.toPlainText() == source);
    CHECK(editor.document()->isUndoAvailable() == false);
    static_cast<void>(editor.grab());

    // Hidden hints keep their positions; Ctrl+Alt shows them until a key is released.
    editor.SetInlayHintsVisible(false);
    CHECK(QSettings().value(QStringLiteral("codeEditor/inlayHints")).toBool() == false);
    REQUIRE(QTest::qWaitFor([&]() { return getX(argument) == plainArgument; }));
    CHECK(editor.GetInlayHints().size() == 2);
    QTest::keyPress(&editor, Qt::Key_Alt, Qt::ControlModifier | Qt::AltModifier);
    CHECK(editor.AreInlayHintsShown() == true);
    REQUIRE(QTest::qWaitFor([&]() { return getX(argument) > plainArgument; }));
    QTest::keyRelease(&editor, Qt::Key_Alt, Qt::ControlModifier);
    CHECK(editor.AreInlayHintsShown() == false);
    editor.SetInlayHintsVisible(true);

    // Edits move later hints and drop the one at the edit.
    QTextCursor cursor(editor.document());
    cursor.insertText(QStringLiteral("x"));
    REQUIRE(editor.GetInlayHints().size() == 2);
    CHECK(editor.GetInlayHints().at(0).Position == argument + 1);
    cursor.setPosition(argument + 1);
    cursor.insertText(QStringLiteral("y"));
    REQUIRE(editor.GetInlayHints().size() == 1);
    CHECK(editor.GetInlayHints().at(0).Position == lineStart + 2);
    editor.SetInlayHintsVisible(wasVisible);
}
