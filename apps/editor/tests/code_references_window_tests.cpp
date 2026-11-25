#include "internal/code_editor.h"
#include "internal/code_navigation.h"
#include "internal/code_references_window.h"

#include <catch2/catch_test_macros.hpp>

#include <QSignalSpy>
#include <QTextCursor>
#include <QTextDocument>
#include <QTreeWidget>
#include <QTreeWidgetItem>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
/// Every "Value" in `text` as current-buffer targets, then one read-only target.
QList<CodeNavigationTarget> FindTargets(const QString& text)
{
    QList<CodeNavigationTarget> targets;
    qsizetype start = text.indexOf(QStringLiteral("Value"));
    while (start >= 0)
    {
        CodeNavigationTarget target;
        target.Label = QStringLiteral("Value");
        target.Text = text;
        target.Start = static_cast<int32>(start);
        target.End = static_cast<int32>(start) + 5;
        targets.append(target);
        start = text.indexOf(QStringLiteral("Value"), start + 1);
    }
    CodeNavigationTarget generated;
    generated.Label = QStringLiteral("Value — shader context, line 4 (read-only)");
    generated.Text = QStringLiteral("float Value();\n");
    generated.Start = 6;
    generated.End = 11;
    generated.IsReadOnly = true;
    targets.append(generated);
    return targets;
}

/// A view host that accepts or declines references and counts what it was given.
class ReferenceHost final : public CodeNavigationViewHost
{
public:
    CodeNavigation* OpenNavigationTarget(const CodeNavigationTarget& target) override
    {
        static_cast<void>(target);
        return nullptr;
    }
    CodeNavigation* ActivateNavigationView(CodeNavigation* viewOrNull, const QString& file) override
    {
        static_cast<void>(file);
        return viewOrNull;
    }
    bool ShowReferences(CodeNavigation& inoutOrigin, const CodeReferenceResults& results) override
    {
        static_cast<void>(inoutOrigin);
        Received += static_cast<int32>(results.Targets.size());
        return IsAccepting;
    }
    bool IsAccepting = true;
    int32 Received = 0;
};
} // namespace

TEST_CASE("The References window groups results and opens them from the finding editor",
          "[editor][code-editor][code-navigation]")
{
    CodeEditor* editorOrNull = new CodeEditor;
    CodeEditor& editor = *editorOrNull;
    editor.SetLanguage(CodeLanguage::Slang);
    const QString text = QStringLiteral("float Value() { return 1; }\nfloat a = Value();\nfloat b = Value();\n");
    editor.setPlainText(text);
    editor.resize(600, 300);
    editor.show();
    QTextCursor caret(editor.document());
    caret.setPosition(static_cast<int32>(text.lastIndexOf(QStringLiteral("Value"))) + 2);
    editor.setTextCursor(caret);

    CodeReferencesWindow window;
    QSignalSpy shown(&window, &CodeReferencesWindow::ResultsShown);
    window.Show(editor.GetNavigation(),
                { .Symbol = QStringLiteral("Value"), .Targets = FindTargets(text), .Message = {} });
    CHECK(shown.size() == 1);
    CHECK(window.GetResultCount() == 4);
    CHECK(window.GetSummary() == QStringLiteral("4 references to 'Value'"));
    QTreeWidget* treeOrNull = window.findChild<QTreeWidget*>(QStringLiteral("codeReferencesResults"));
    REQUIRE(treeOrNull != nullptr);
    REQUIRE(treeOrNull->topLevelItemCount() == 2);
    CHECK(treeOrNull->topLevelItem(0)->text(0) == QStringLiteral("Current source (3)"));
    CHECK(treeOrNull->topLevelItem(0)->child(1)->text(0) == QStringLiteral("line 2, column 11   float a = Value();"));
    CHECK(treeOrNull->topLevelItem(1)->text(0) == QStringLiteral("Shader context (read-only) (1)"));

    // A row opens through the editor that found it, which keeps its own history.
    REQUIRE(window.ActivateResult(1) == true);
    CHECK(editor.textCursor().selectedText() == QStringLiteral("Value"));
    CHECK(editor.textCursor().blockNumber() == 1);
    CHECK(editor.toPlainText() == text);

    // After an edit the rows describe earlier text: say so and offer Find Again.
    QTextCursor edit(editor.document());
    edit.insertText(QStringLiteral("// note\n"));
    CHECK(window.IsOutOfDate() == true);
    const QTextCursor before = editor.textCursor();
    CHECK(window.ActivateResult(2) == false);
    CHECK(window.GetStatus().contains(QStringLiteral("Find Again")) == true);
    CHECK(editor.textCursor() == before);
    QSignalSpy requested(&editor.GetNavigation(), &CodeNavigation::Requested);
    REQUIRE(window.FindAgain() == true);
    REQUIRE(requested.size() == 1);
    CHECK(requested.first().first().value<CodeNavigationKind>() == CodeNavigationKind::References);
    // The search starts from the original symbol, moved with the edit.
    CHECK(editor.textCursor().position() == caret.position());
    CHECK(editor.textCursor().blockNumber() == 3);

    // A closed editor leaves the rows unusable and the window says what to do.
    delete editorOrNull;
    CHECK(window.ActivateResult(0) == false);
    CHECK(window.GetStatus().contains(QStringLiteral("closed")) == true);
    CHECK(window.FindAgain() == false);
}

TEST_CASE("Find References results go to the host's window or to the chooser", "[editor][code-editor][code-navigation]")
{
    CodeEditor editor;
    editor.SetLanguage(CodeLanguage::Slang);
    const QString text = QStringLiteral("float Value();\nfloat a = Value();\n");
    editor.setPlainText(text);
    editor.resize(600, 300);
    editor.show();
    ReferenceHost host;
    editor.GetNavigation().SetViewHost(&host);
    const CodeReferenceResults results
    {
        .Symbol = QStringLiteral("Value"),
        .Targets = FindTargets(text),
        .Message = {},
    };
    editor.GetNavigation().ShowReferences(results);
    CHECK(host.Received == 3);
    CHECK(editor.GetNavigation().IsChoosing() == false);
    // A host without a References window keeps the chooser beside the editor.
    host.IsAccepting = false;
    editor.GetNavigation().ShowReferences(results);
    CHECK(host.Received == 6);
    CHECK(editor.GetNavigation().IsChoosing() == true);
    editor.GetNavigation().Dismiss();
    editor.GetNavigation().SetViewHost(nullptr);
}
