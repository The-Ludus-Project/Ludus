#include "internal/code_edit_set.h"
#include "internal/code_editor.h"
#include "internal/code_structure_view.h"
#include "internal/code_syntax_service.h"

#include <ludus/foundation/containers/static_array.hpp>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QCoreApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QRandomGenerator>
#include <QScrollBar>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
const CodeSyntaxService& GetSyntax(const CodeEditor& editor)
{
    const CodeSyntaxService* serviceOrNull = editor.document()->findChild<CodeSyntaxService*>();
    REQUIRE(serviceOrNull != nullptr);
    return *serviceOrNull;
}

const CodeSyntaxResult& WaitForResult(const CodeEditor& editor)
{
    const CodeSyntaxService& service = GetSyntax(editor);
    REQUIRE(QTest::qWaitFor([&service]() { return service.GetCurrentResultOrNull() != nullptr; }));
    return *service.GetCurrentResultOrNull();
}

bool HasToken(const CodeSyntaxResult& result, const QString& source, const QString& spelling, CodeSyntaxRole role)
{
    for (const CodeSyntaxToken& token : result.Tokens)
    {
        const QStringView text = QStringView(source).sliced(token.GetStart(), token.GetEnd() - token.GetStart());
        if (text.trimmed() == spelling && token.GetRole() == role)
        {
            return true;
        }
    }
    return false;
}

bool HasSymbol(const CodeSyntaxResult& result, QStringView name)
{
    for (int32 index : result.Outline)
    {
        if (result.GetLabel(result.Nodes.at(index)) == name)
        {
            return true;
        }
    }
    return false;
}

void TriggerCommand(CodeEditor& inoutEditor, const QString& label)
{
    for (QAction* action : inoutEditor.actions())
    {
        if (action->text() == label)
        {
            action->trigger();
            return;
        }
    }
    FAIL("Missing code editor command");
}
} // namespace

TEST_CASE("editor syntax publishes tokens and outline from every configured grammar", "[editor][code-editor][syntax]")
{
    struct Case final
    {
        CodeLanguage Language;
        QString Source;
        QString Token;
        CodeSyntaxRole Role;
        QString Symbol;
    };
    for (const Case& item : {
             Case{CodeLanguage::C,
                  QStringLiteral("int Add(int value) {\n  return value + 1;\n}\n"),
                  QStringLiteral("return"),
                  CodeSyntaxRole::Control,
                  QStringLiteral("Add")},
             Case{CodeLanguage::Cpp,
                  QStringLiteral("namespace Space {\nclass Counter {};\n}\n"),
                  QStringLiteral("Space"),
                  CodeSyntaxRole::Namespace,
                  QStringLiteral("Counter")},
             Case{CodeLanguage::Slang,
                  QStringLiteral("float Compute(float value) {\n  return value;\n}\n"),
                  QStringLiteral("Compute"),
                  CodeSyntaxRole::Function,
                  QStringLiteral("Compute")},
             Case{CodeLanguage::Lusl,
                  QStringLiteral("float32 Shade(in float32 normal) {\n  return normal;\n}\n"),
                  QStringLiteral("Shade"),
                  CodeSyntaxRole::Function,
                  QStringLiteral("Shade")},
             Case{CodeLanguage::Luau,
                  QStringLiteral("local function Length(value: number)\n  return value\nend\n"),
                  QStringLiteral("return"),
                  CodeSyntaxRole::Control,
                  QStringLiteral("Length")},
             Case{CodeLanguage::Json,
                  QStringLiteral("{\n  \"name\": \"é😀\",\n  \"values\": [7]\n}\n"),
                  QStringLiteral("\"é😀\""),
                  CodeSyntaxRole::String,
                  QStringLiteral("\"values\"")},
             Case{CodeLanguage::CMake,
                  QStringLiteral("function(Build)\n  message(\"hello\")\nendfunction()\n"),
                  QStringLiteral("message"),
                  CodeSyntaxRole::Function,
                  QStringLiteral("Build")},
             Case{CodeLanguage::Markdown,
                  QStringLiteral("# Overview\n\nText with `inline` code.\n\n## Scope\n\nMore text.\n"),
                  QStringLiteral("`inline`"),
                  CodeSyntaxRole::String,
                  QStringLiteral("## Scope")},
         })
    {
        CAPTURE(static_cast<int32>(item.Language));
        CodeEditor editor;
        editor.SetLanguage(item.Language);
        editor.setPlainText(item.Source);
        const CodeSyntaxResult& result = WaitForResult(editor);
        CHECK(HasToken(result, item.Source, item.Token, item.Role));
        CHECK(HasSymbol(result, item.Symbol));
        CHECK(result.Folds.isEmpty() == false);
        int32 end = 0;
        for (const CodeSyntaxToken& token : result.Tokens)
        {
            CHECK(token.GetStart() >= end);
            CHECK(token.GetEnd() > token.GetStart());
            CHECK(token.GetEnd() <= item.Source.size());
            end = token.GetEnd();
        }
    }
}

TEST_CASE("same-size malformed edits and language switches reject stale syntax", "[editor][code-editor][syntax]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Json);
    editor.setPlainText(QStringLiteral("{\"é😀\": [42]}"));
    const uint64 initial = WaitForResult(editor).Revision;
    const CodeSyntaxService& service = GetSyntax(editor);
    const QString token = QStringLiteral("42");
    QRandomGenerator random(1262);
    for (int32 iteration = 0; iteration < 16; ++iteration)
    {
        const int32 index = static_cast<int32>(random.bounded(2U));
        QTextCursor cursor(editor.document());
        const int32 digits = static_cast<int32>(editor.toPlainText().indexOf(u'[')) + 1;
        cursor.setPosition(digits + index);
        cursor.setPosition(digits + index + 1, QTextCursor::KeepAnchor);
        cursor.insertText(index == 0 ? QStringLiteral("x") : QStringLiteral("7"));
        CHECK(service.GetCurrentResultOrNull() == nullptr);
        cursor.setPosition(digits);
        cursor.setPosition(digits + token.size(), QTextCursor::KeepAnchor);
        cursor.insertText(token);
        if (iteration % 2 == 0)
        {
            CHECK(HasToken(WaitForResult(editor), editor.toPlainText(), token, CodeSyntaxRole::Number));
        }
    }
    editor.setPlainText(QStringLiteral("{\"x\": [\n   true,\n"));
    const CodeSyntaxResult& malformed = WaitForResult(editor);
    CHECK(malformed.Revision > initial);
    CHECK(HasToken(malformed, editor.toPlainText(), QStringLiteral("true"), CodeSyntaxRole::Constant));
    editor.SetLanguage(CodeLanguage::PlainText);
    CHECK(service.GetCurrentResultOrNull() == nullptr);
    CHECK(WaitForResult(editor).Tokens.isEmpty());
}

TEST_CASE("incremental syntax products equal clean parsing after random local edits", "[editor][code-editor][syntax]")
{
    constexpr int32 DECLARATION_COUNT = 24;
    constexpr int32 EDIT_COUNT = 12;
    QString source;
    for (int32 index = 0; index < DECLARATION_COUNT; ++index)
    {
        source += QStringLiteral("int Run%1() {\n  // Unicode é😀\n  return 1;\n}\n").arg(index);
    }
    CodeEditor incremental;
    incremental.SetLanguage(CodeLanguage::Cpp);
    incremental.setPlainText(source);
    static_cast<void>(WaitForResult(incremental));
    QRandomGenerator random(1262);
    bool hasReusedProducts = false;
    for (int32 index = 0; index < EDIT_COUNT; ++index)
    {
        const int32 declaration = static_cast<int32>(random.bounded(static_cast<uint32>(DECLARATION_COUNT)));
        const QString current = incremental.toPlainText();
        const qsizetype start = current.indexOf(QStringLiteral("int Run%1()").arg(declaration));
        const qsizetype value = current.indexOf(QStringLiteral("return "), start) + QStringView(u"return ").size();
        const qsizetype end = current.indexOf(u';', value);
        QTextCursor edit(incremental.document());
        edit.setPosition(static_cast<int32>(value));
        edit.setPosition(static_cast<int32>(end), QTextCursor::KeepAnchor);
        edit.insertText(index % 2 == 0 ? QStringLiteral("99") : QStringLiteral("1 +\n      2"));
        CHECK(GetSyntax(incremental).GetCurrentResultOrNull() == nullptr);
        const CodeSyntaxResult& updated = WaitForResult(incremental);
        hasReusedProducts = hasReusedProducts == true || updated.HasReusedProducts == true;
        CodeEditor clean;
        clean.SetLanguage(CodeLanguage::Cpp);
        clean.setPlainText(incremental.toPlainText());
        const CodeSyntaxResult& expected = WaitForResult(clean);
        CHECK(updated.Tokens == expected.Tokens);
        CHECK(updated.Outline == expected.Outline);
        CHECK(updated.Labels == expected.Labels);
        REQUIRE(updated.Nodes.size() == expected.Nodes.size());
        for (qsizetype nodeIndex = 0; nodeIndex < updated.Nodes.size(); ++nodeIndex)
        {
            CHECK(updated.Nodes.at(nodeIndex).Range == expected.Nodes.at(nodeIndex).Range);
            CHECK(updated.Nodes.at(nodeIndex).Parent == expected.Nodes.at(nodeIndex).Parent);
        }
        REQUIRE(updated.Folds.size() == expected.Folds.size());
        for (const CodeSyntaxFold& fold : expected.Folds)
        {
            CHECK(updated.Folds.contains(fold));
        }
    }
    CHECK(hasReusedProducts);
}

TEST_CASE("fold commands retain visible controls across randomized changes", "[editor][code-editor][syntax]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Cpp);
    editor.setPlainText(QStringLiteral("void Outer() {\n  if (true) {\n    Work();\n  }\n}\n"
                                       "// #region More\n/* hello\n   world */\n// #endregion\n"));
    static_cast<void>(WaitForResult(editor));
    TriggerCommand(editor, QStringLiteral("Fold Recursively"));
    CHECK(editor.document()->findBlockByNumber(2).isVisible() == false);
    TriggerCommand(editor, QStringLiteral("Unfold Recursively"));
    CHECK(editor.document()->findBlockByNumber(2).isVisible());
    TriggerCommand(editor, QStringLiteral("Fold All Comments"));
    CHECK(editor.document()->findBlockByNumber(7).isVisible() == false);
    TriggerCommand(editor, QStringLiteral("Unfold All"));
    TriggerCommand(editor, QStringLiteral("Fold All Regions"));
    CHECK(editor.document()->findBlockByNumber(8).isVisible() == false);
    TriggerCommand(editor, QStringLiteral("Unfold All"));
    TriggerCommand(editor, QStringLiteral("Fold Level 2"));
    CHECK(editor.document()->findBlockByNumber(1).isVisible());
    CHECK(editor.document()->findBlockByNumber(2).isVisible() == false);
    QRandomGenerator random(1262);
    constexpr int32 ACTION_COUNT = 24;
    for (int32 index = 0; index < ACTION_COUNT; ++index)
    {
        switch (random.bounded(4U))
        {
            case 0:
                editor.GetStructure().FoldAll();
                break;
            case 1:
                editor.GetStructure().UnfoldAll();
                break;
            case 2:
                editor.GetStructure().ToggleFold(static_cast<int32>(random.bounded(9U)));
                break;
            default: {
                QTextCursor edit(editor.document()->findBlockByNumber(2));
                edit.insertText(QStringLiteral(" "));
                static_cast<void>(WaitForResult(editor));
                break;
            }
        }
        int32 coveredThrough = -1;
        for (QTextBlock block = editor.document()->firstBlock(); block.isValid() == true; block = block.next())
        {
            const int32 line = block.blockNumber();
            if (block.isVisible() == true)
            {
                coveredThrough = editor.GetStructure().GetLastCollapsedLine(line);
            }
            else
            {
                CHECK(line <= coveredThrough);
            }
        }
        CHECK(editor.textCursor().block().isVisible());
    }
    editor.GetStructure().UnfoldAll();
    for (QTextBlock block = editor.document()->firstBlock(); block.isValid() == true; block = block.next())
    {
        CHECK(block.isVisible());
    }
}

TEST_CASE("fold controls preserve the full text and reveal a navigation destination", "[editor][code-editor][syntax]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Cpp);
    const QString source = QStringLiteral("int Run()\n{\n  if (true) {\n    return 1;\n  }\n}\n\nint next;\n");
    editor.setPlainText(source);
    static_cast<void>(WaitForResult(editor));
    CodeStructureView& structure = editor.GetStructure();
    REQUIRE(structure.HasFold(1));
    structure.ToggleFold(1);
    REQUIRE(editor.document()->findBlockByNumber(1).isVisible());
    CHECK(editor.document()->findBlockByNumber(3).isVisible() == false);
    CHECK(editor.toPlainText() == source);
    CHECK(editor.document()->isUndoAvailable() == false);
    const QJsonArray state = structure.SaveFolds();
    REQUIRE(state.size() == 1);
    editor.GoToLine(4);
    CHECK(editor.textCursor().blockNumber() == 3);
    CHECK(editor.document()->findBlockByNumber(3).isVisible());
    editor.GoToLine(2);
    structure.RestoreFolds(state);
    CHECK(editor.document()->findBlockByNumber(3).isVisible() == false);
    editor.GoToLine(2);
    QTest::keyClick(&editor, Qt::Key_Down);
    CHECK(editor.textCursor().blockNumber() == 6);
    CHECK(editor.document()->findBlockByNumber(3).isVisible() == false);
    structure.UnfoldAll();
    CHECK(editor.document()->findBlockByNumber(3).isVisible());
}

TEST_CASE("syntax navigation expands selection and restores the earlier caret", "[editor][code-editor][syntax]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Cpp);
    editor.setPlainText(QStringLiteral("int Add() {\n  return (value + 2);\n}\n"));
    static_cast<void>(WaitForResult(editor));
    QTextCursor caret = editor.document()->find(QStringLiteral("value"));
    caret.setPosition(caret.selectionStart() + 1);
    editor.setTextCursor(caret);
    const QToolButton* breadcrumbsOrNull = editor.findChild<QToolButton*>(QStringLiteral("codeSyntaxBreadcrumbs"));
    REQUIRE(breadcrumbsOrNull != nullptr);
    CHECK(breadcrumbsOrNull->text().contains(QStringLiteral("Add")));
    TriggerCommand(editor, QStringLiteral("Expand Selection"));
    CHECK(editor.textCursor().selectedText() == QStringLiteral("value"));
    TriggerCommand(editor, QStringLiteral("Expand Selection"));
    CHECK(editor.textCursor().selectedText() == QStringLiteral("value + 2"));
    TriggerCommand(editor, QStringLiteral("Shrink Selection"));
    CHECK(editor.textCursor().selectedText() == QStringLiteral("value"));
    TriggerCommand(editor, QStringLiteral("Shrink Selection"));
    CHECK(editor.textCursor() == caret);
}

TEST_CASE("disjoint format edits remap collapsed opening and closing lines", "[editor][code-editor][syntax]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Cpp);
    const QByteArray before("int top;\nvoid Run() {\n  Work();\n}\nint tail;\n");
    editor.setPlainText(QString::fromUtf8(before));
    static_cast<void>(WaitForResult(editor));
    CodeStructureView& structure = editor.GetStructure();
    structure.ToggleFold(1);
    const QByteArray inserted("// format\n");
    const QByteArray second("  ");
    const usize inside = static_cast<usize>(before.indexOf("Work"));
    constexpr usize EDIT_COUNT = 2;
    const StaticArray<CodeTextEdit, EDIT_COUNT> replacements = {{
        {0, 0, {inserted.constData(), static_cast<usize>(inserted.size())}},
        {inside, 0, {second.constData(), static_cast<usize>(second.size())}},
    }};
    CodeEditSet edits;
    edits.Edits = {replacements.GetData(), replacements.GetSize()};
    const QList<CodeFoldEditRange> mapped = structure.MapFolds(before, edits);
    REQUIRE(mapped.size() == 1);
    QByteArray after = before;
    after.insert(static_cast<qsizetype>(inside), second);
    after.prepend(inserted);
    editor.setPlainText(QString::fromUtf8(after));
    structure.RestoreMappedFolds(mapped, after);
    CHECK(editor.document()->findBlockByNumber(3).isVisible() == false);
    static_cast<void>(WaitForResult(editor));
    CHECK(editor.document()->findBlockByNumber(3).isVisible() == false);
    CHECK(editor.toPlainText().toUtf8() == after);
}

TEST_CASE("outline and sticky navigation jump to the current declaration", "[editor][code-editor][syntax]")
{
    CodeEditor editor;
    editor.resize(640, 240);
    editor.SetLanguage(CodeLanguage::Cpp);
    QString source = QStringLiteral("int First() {\n");
    source += QStringLiteral("  Work();\n").repeated(60);
    source += QStringLiteral("}\nint Second() {}\n");
    editor.setPlainText(source);
    editor.show();
    static_cast<void>(WaitForResult(editor));
    QToolButton* stickyOrNull = editor.findChild<QToolButton*>(QStringLiteral("codeSyntaxSticky"));
    REQUIRE(stickyOrNull != nullptr);
    editor.GoToLine(30);
    editor.centerCursor();
    REQUIRE(QTest::qWaitFor([stickyOrNull]() { return stickyOrNull->isVisible(); }));
    CHECK(stickyOrNull->text() == QStringLiteral("First"));
    stickyOrNull->click();
    CHECK(editor.textCursor().blockNumber() == 0);

    TriggerCommand(editor, QStringLiteral("Outline"));
    QDialog* outlineOrNull = editor.findChild<QDialog*>();
    REQUIRE(outlineOrNull != nullptr);
    QLineEdit* filterOrNull = outlineOrNull->findChild<QLineEdit*>();
    REQUIRE(filterOrNull != nullptr);
    filterOrNull->setText(QStringLiteral("Second"));
    QTest::keyClick(filterOrNull, Qt::Key_Return);
    CHECK(editor.textCursor().blockNumber() == editor.blockCount() - 2);
}

TEST_CASE("a large syntax request leaves the event loop available and folds by indentation",
          "[editor][code-editor][syntax]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Cpp);
    constexpr int32 LARGE_LINE_COUNT = 200001;
    QString source = QStringLiteral("first\n  second\nlast\n");
    source += QStringLiteral("int item;\n").repeated(LARGE_LINE_COUNT);
    editor.setPlainText(source);
    bool hasProcessedInput = false;
    QTimer::singleShot(0, &editor, [&hasProcessedInput]() { hasProcessedInput = true; });
    CHECK(GetSyntax(editor).GetCurrentResultOrNull() == nullptr);
    const CodeSyntaxResult& result = WaitForResult(editor);
    CHECK(hasProcessedInput);
    CHECK(result.Tokens.isEmpty());
    CHECK(result.Outline.isEmpty());
    REQUIRE(result.Folds.size() == 1);
    CHECK(result.Folds.constFirst().Kind == CodeFoldKind::Indentation);
    editor.GetStructure().FoldAll();
    CHECK(editor.document()->findBlockByNumber(1).isVisible() == false);
    editor.GetStructure().UnfoldAll();
    CHECK(editor.document()->findBlockByNumber(1).isVisible());
}

TEST_CASE("folded text participates in line commands and stale folds are removed", "[editor][code-editor][syntax]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Cpp);
    const QString source = QStringLiteral("void Run() {\n  Work();\n}\n");
    editor.setPlainText(source);
    static_cast<void>(WaitForResult(editor));
    editor.GetStructure().ToggleFold(0);
    editor.DuplicateLines();
    CHECK(editor.toPlainText() == source + source);
    CHECK(editor.document()->findBlockByNumber(1).isVisible() == false);
    CHECK(editor.document()->findBlockByNumber(4).isVisible() == false);
    static_cast<void>(WaitForResult(editor));
    QTextCursor edit(editor.document()->firstBlock());
    edit.select(QTextCursor::BlockUnderCursor);
    edit.removeSelectedText();
    static_cast<void>(WaitForResult(editor));
    CHECK(editor.document()->findBlockByNumber(1).isVisible());
    editor.GetStructure().UnfoldAll();
    for (QTextBlock block = editor.document()->firstBlock(); block.isValid() == true; block = block.next())
    {
        CHECK(block.isVisible());
    }
}

TEST_CASE("a 100,000-line document can publish and toggle all folds", "[editor][code-editor][.benchmark][syntax]")
{
    constexpr int32 FUNCTIONS = 20000;
    const QString source = QStringLiteral("void Run()\n{\n  Work();\n}\n\n").repeated(FUNCTIONS);
    CodeEditor editor;
    editor.resize(900, 640);
    editor.SetLanguage(CodeLanguage::Cpp);
    QElapsedTimer clock;
    clock.start();
    editor.setPlainText(source);
    editor.show();
    QCoreApplication::processEvents();
    const int64 firstPaintMilliseconds = clock.elapsed();
    const CodeSyntaxResult& result = WaitForResult(editor);
    const int64 publishMilliseconds = clock.elapsed();
    REQUIRE(result.Folds.isEmpty() == false);
    clock.restart();
    editor.GetStructure().FoldAll();
    const int64 foldMilliseconds = clock.elapsed();
    CHECK(editor.document()->findBlockByNumber(2).isVisible() == false);
    clock.restart();
    editor.GetStructure().UnfoldAll();
    const int64 unfoldMilliseconds = clock.elapsed();
    CHECK(editor.document()->findBlockByNumber(2).isVisible());
    WARN(QStringLiteral("M3 | %1 lines | open and paint %2 ms | syntax ready %3 ms (includes debounce) | "
                        "Fold All %4 ms | Unfold All %5 ms")
             .arg(editor.blockCount())
             .arg(firstPaintMilliseconds)
             .arg(publishMilliseconds)
             .arg(foldMilliseconds)
             .arg(unfoldMilliseconds)
             .toStdString());
}

TEST_CASE("syntax edit and scroll latency on the M3 corpus", "[editor][code-editor][.benchmark][syntax]")
{
    constexpr int32 ONE_MIB = 1024 * 1024;
    const QString declaration = QStringLiteral("int Sample%1() {\n  const int scale = 7;\n"
                                               "  return scale + 12;\n}\n");
    QString source;
    for (int32 index = 0; source.size() < ONE_MIB; ++index)
    {
        source += declaration.arg(index);
    }
    CodeEditor editor;
    editor.resize(900, 640);
    editor.show();
    QCoreApplication::processEvents();
    QElapsedTimer clock;
    clock.start();
    editor.SetLanguage(CodeLanguage::Cpp);
    editor.setPlainText(source);
    editor.viewport()->repaint();
    const int64 firstPaint = clock.elapsed();
    QPlainTextEdit baseline;
    baseline.setFont(editor.font());
    baseline.setLineWrapMode(editor.lineWrapMode());
    baseline.resize(editor.size());
    baseline.show();
    QCoreApplication::processEvents();
    clock.restart();
    baseline.setPlainText(source);
    baseline.viewport()->repaint();
    const int64 plainQtPaint = clock.elapsed();
    baseline.hide();
    static_cast<void>(WaitForResult(editor));
    const int32 offset = static_cast<int32>(source.indexOf(QStringLiteral("return scale + "), source.size() / 2));
    REQUIRE(offset > 0);
    QTextCursor edit(editor.document());
    edit.setPosition(offset + static_cast<int32>(QStringView(u"return scale + ").size()));
    edit.insertText(QStringLiteral("3"));
    const CodeSyntaxResult& updated = WaitForResult(editor);
    const int64 reparse = updated.WorkerMicroseconds;
    const bool hasReusedProducts = updated.HasReusedProducts;
    QScrollBar* scroll = editor.verticalScrollBar();
    constexpr int32 SAMPLES = 20;
    int64 longestPaint = 0;
    for (int32 sample = 0; sample < SAMPLES; ++sample)
    {
        clock.restart();
        scroll->setValue(scroll->maximum() * sample / SAMPLES);
        editor.viewport()->repaint();
        longestPaint = qMax(longestPaint, clock.nsecsElapsed());
    }
    constexpr int64 NANOSECONDS_PER_MICROSECOND = 1000;
    WARN(QStringLiteral("M3 | %1 units | first paint %2 ms (plain Qt %3 ms) | reparse %4 us | reused %5 | "
                        "worst sampled scroll + paint %6 us")
             .arg(source.size())
             .arg(firstPaint)
             .arg(plainQtPaint)
             .arg(reparse)
             .arg(hasReusedProducts == true ? QStringLiteral("yes") : QStringLiteral("no"))
             .arg(longestPaint / NANOSECONDS_PER_MICROSECOND)
             .toStdString());
}

TEST_CASE("first paint of the M3 8 MiB JSON corpus", "[editor][code-editor][.benchmark][syntax]")
{
    constexpr int32 CORPUS_UNITS = 8 * 1024 * 1024;
    const QString row =
        QStringLiteral("  {\"name\": \"sample\", \"id\": 7, \"description\": \"representative document row\"},\n");
    const QString source =
        QStringLiteral("[\n") + row.repeated(CORPUS_UNITS / row.size() - 1) + QStringLiteral("{}\n]\n");
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Json);
    editor.resize(900, 640);
    editor.show();
    QCoreApplication::processEvents();
    QElapsedTimer clock;
    clock.start();
    editor.setPlainText(source);
    editor.viewport()->repaint();
    const int64 firstPaint = clock.elapsed();
    CHECK(editor.document()->characterCount() == source.size() + 1);
    WARN(QStringLiteral("M3 | %1 UTF-16 units | first paint %2 ms").arg(source.size()).arg(firstPaint).toStdString());
}
