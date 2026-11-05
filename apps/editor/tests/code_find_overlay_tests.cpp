// The find/replace overlay (CE07, design section 4): regex, whole word, in
// selection, a live match count and preserve-case replace, with Replace All
// publishing one CodeEditSet built in a single pass. Offscreen; the overlay is
// driven through its widgets by object name, as the find panel's tests were.

#include "internal/code_document.h"
#include "internal/code_document_controller.h"
#include "internal/code_editor.h"
#include "internal/code_editor_panel.h"
#include "internal/code_file_document.h"
#include "internal/code_find_overlay.h"

#include <catch2/catch_test_macros.hpp>

#include <QAbstractButton>
#include <QByteArray>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QString>
#include <QTemporaryDir>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
[[nodiscard]] QString WriteFile(const QTemporaryDir& directory, const QByteArray& bytes)
{
    const QString path = QDir(directory.path()).filePath(QStringLiteral("find.txt"));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
    file.close();
    return path;
}

[[nodiscard]] CodeFileDocument OpenHost(const QTemporaryDir& directory, const QByteArray& bytes)
{
    return CodeFileDocument(LoadCodeFile(WriteFile(directory, bytes)), 1);
}

[[nodiscard]] QByteArray DraftOf(const CodeFileDocument& host)
{
    const CodeDocumentSnapshot snapshot = host.GetSnapshot();
    return QByteArray(snapshot.Text.GetDataOrNull(), static_cast<qsizetype>(snapshot.Text.GetSize()));
}

constexpr qint64 LINEAR_WORK_CEILING_MS = 2000;

[[nodiscard]] QByteArray RepeatLine(const QByteArray& line, qsizetype count)
{
    QByteArray text;
    text.reserve(line.size() * count);
    for (qsizetype index = 0; index < count; ++index)
    {
        text.append(line);
    }
    return text;
}

// A panel bound to a real file host, with handles to the overlay's widgets found
// by object name. The controller is deleted first (its destructor disconnects
// from the editor), then the panel.
struct BoundOverlay final
{
    explicit BoundOverlay(CodeFileDocument& inoutHost)
        : Editor(new CodeEditor()), Controller(new CodeDocumentController(*Editor)),
          Panel(new CodeEditorPanel(Editor, Controller))
    {
        Controller->Bind(&inoutHost);
        FindOrNull = Panel->findChild<QLineEdit*>(QStringLiteral("codeEditorFind"));
        ReplacementOrNull = Panel->findChild<QLineEdit*>(QStringLiteral("codeEditorReplacement"));
        ReplaceAllOrNull = Panel->findChild<QAbstractButton*>(QStringLiteral("codeEditorReplaceAll"));
        FindNextOrNull = Panel->findChild<QAbstractButton*>(QStringLiteral("codeEditorFindNext"));
        FindPreviousOrNull = Panel->findChild<QAbstractButton*>(QStringLiteral("codeEditorFindPrevious"));
        StatusOrNull = Panel->findChild<QLabel*>(QStringLiteral("codeEditorFindStatus"));
        RegexOrNull = Panel->findChild<QCheckBox*>(QStringLiteral("codeEditorRegex"));
        WholeWordOrNull = Panel->findChild<QCheckBox*>(QStringLiteral("codeEditorWholeWord"));
        InSelectionOrNull = Panel->findChild<QCheckBox*>(QStringLiteral("codeEditorInSelection"));
        PreserveCaseOrNull = Panel->findChild<QCheckBox*>(QStringLiteral("codeEditorPreserveCase"));
        // "Match case" has no object name in the original panel; find it by text.
        for (QCheckBox* candidate : Panel->findChildren<QCheckBox*>())
        {
            if (candidate->text() == QStringLiteral("Match case"))
            {
                MatchCaseOrNull = candidate;
            }
        }
        REQUIRE(FindOrNull != nullptr);
        REQUIRE(ReplacementOrNull != nullptr);
        REQUIRE(ReplaceAllOrNull != nullptr);
        REQUIRE(FindNextOrNull != nullptr);
        REQUIRE(FindPreviousOrNull != nullptr);
        REQUIRE(StatusOrNull != nullptr);
        REQUIRE(RegexOrNull != nullptr);
        REQUIRE(WholeWordOrNull != nullptr);
        REQUIRE(InSelectionOrNull != nullptr);
        REQUIRE(PreserveCaseOrNull != nullptr);
        REQUIRE(MatchCaseOrNull != nullptr);
    }
    ~BoundOverlay()
    {
        delete Controller;
        delete Panel;
    }
    BoundOverlay(const BoundOverlay&) = delete;
    BoundOverlay& operator=(const BoundOverlay&) = delete;

    CodeEditor* Editor;
    CodeDocumentController* Controller;
    CodeEditorPanel* Panel;
    QLineEdit* FindOrNull = nullptr;
    QLineEdit* ReplacementOrNull = nullptr;
    QAbstractButton* ReplaceAllOrNull = nullptr;
    QAbstractButton* FindNextOrNull = nullptr;
    QAbstractButton* FindPreviousOrNull = nullptr;
    QLabel* StatusOrNull = nullptr;
    QCheckBox* RegexOrNull = nullptr;
    QCheckBox* WholeWordOrNull = nullptr;
    QCheckBox* InSelectionOrNull = nullptr;
    QCheckBox* PreserveCaseOrNull = nullptr;
    QCheckBox* MatchCaseOrNull = nullptr;
};
} // namespace

TEST_CASE("The overlay replaces the bottom panel with its find widgets", "[code-editor][find-overlay]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("alpha beta alpha\n"));
    BoundOverlay bound(host);
    // The overlay carries the same find/replace/status object names as the old
    // panel, so tests and status strings are unchanged, plus the new options.
    CHECK(bound.Panel->findChild<QWidget*>(QStringLiteral("codeFindOverlay")) != nullptr);
}

TEST_CASE("A regex replace all rewrites every match through one edit set", "[code-editor][find-overlay]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("a1 b22 c333\n"));
    BoundOverlay bound(host);
    bound.RegexOrNull->setChecked(true);
    bound.FindOrNull->setText(QStringLiteral("([a-z])([0-9]+)"));
    bound.ReplacementOrNull->setText(QStringLiteral("$2$1"));
    bound.ReplaceAllOrNull->click();
    CHECK(DraftOf(host) == QByteArray("1a 22b 333c\n"));
    CHECK(bound.StatusOrNull->text() == QStringLiteral("3 replacements"));
    // One undo step restores the whole file.
    QList<CodeTextSelection> undone;
    REQUIRE(host.Undo(undone) == true);
    CHECK(DraftOf(host) == QByteArray("a1 b22 c333\n"));
}

TEST_CASE("Whole word matches only complete words", "[code-editor][find-overlay]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("in inside bin in\n"));
    BoundOverlay bound(host);
    bound.WholeWordOrNull->setChecked(true);
    bound.MatchCaseOrNull->setChecked(true);
    bound.FindOrNull->setText(QStringLiteral("in"));
    bound.ReplacementOrNull->setText(QStringLiteral("ON"));
    bound.ReplaceAllOrNull->click();
    // Only the standalone "in" words change, not "inside" or "bin".
    CHECK(DraftOf(host) == QByteArray("ON inside bin ON\n"));
}

TEST_CASE("In selection limits replace all to the frozen range", "[code-editor][find-overlay]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("x x\nx x\nx x\n"));
    BoundOverlay bound(host);
    // Select the first two lines (positions 0..7: "x x\nx x\n" ends before line 3).
    QTextCursor selection(bound.Editor->document());
    selection.setPosition(0);
    selection.setPosition(8, QTextCursor::KeepAnchor);
    bound.Editor->setTextCursor(selection);
    bound.InSelectionOrNull->setChecked(true);
    bound.MatchCaseOrNull->setChecked(true);
    bound.FindOrNull->setText(QStringLiteral("x"));
    bound.ReplacementOrNull->setText(QStringLiteral("y"));
    bound.ReplaceAllOrNull->click();
    // The third line is outside the selection and is untouched.
    CHECK(DraftOf(host) == QByteArray("y y\ny y\nx x\n"));
}

TEST_CASE("Preserve case matches the case of each replaced occurrence", "[code-editor][find-overlay]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("foo Foo FOO\n"));
    BoundOverlay bound(host);
    bound.PreserveCaseOrNull->setChecked(true);
    bound.FindOrNull->setText(QStringLiteral("foo")); // Case-insensitive by default.
    bound.ReplacementOrNull->setText(QStringLiteral("bar"));
    bound.ReplaceAllOrNull->click();
    CHECK(DraftOf(host) == QByteArray("bar Bar BAR\n"));
}

TEST_CASE("The match count reports the total and the current match", "[code-editor][find-overlay]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("a a a\n"));
    BoundOverlay bound(host);
    bound.MatchCaseOrNull->setChecked(true);
    bound.FindOrNull->setText(QStringLiteral("a"));
    // Before navigating, the total is shown.
    CHECK(bound.StatusOrNull->text() == QStringLiteral("3 matches"));
    bound.FindNextOrNull->click();
    CHECK(bound.StatusOrNull->text() == QStringLiteral("1 of 3"));
    bound.FindNextOrNull->click();
    CHECK(bound.StatusOrNull->text() == QStringLiteral("2 of 3"));
    bound.FindPreviousOrNull->click();
    CHECK(bound.StatusOrNull->text() == QStringLiteral("1 of 3"));
}

TEST_CASE("An invalid regular expression is reported and nothing changes", "[code-editor][find-overlay]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("abc\n"));
    BoundOverlay bound(host);
    bound.RegexOrNull->setChecked(true);
    bound.FindOrNull->setText(QStringLiteral("(unterminated"));
    bound.ReplacementOrNull->setText(QStringLiteral("x"));
    bound.ReplaceAllOrNull->click();
    CHECK(DraftOf(host) == QByteArray("abc\n"));
    CHECK(bound.StatusOrNull->text().startsWith(QStringLiteral("Invalid pattern")) == true);
}

TEST_CASE("Find next wraps at the end of the document", "[code-editor][find-overlay]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("v V v\n"));
    BoundOverlay bound(host);
    bound.FindOrNull->setText(QStringLiteral("v")); // Case-insensitive: three matches.
    bound.Editor->moveCursor(QTextCursor::End);
    bound.FindNextOrNull->click();
    CHECK(bound.Editor->textCursor().selectionStart() == 0);
    CHECK(bound.Editor->textCursor().selectedText() == QStringLiteral("v"));
}

TEST_CASE("A large regex replace all through the overlay stays linear", "[code-editor][find-overlay]")
{
    constexpr qsizetype LINES = 16384;
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QByteArray original = RepeatLine(QByteArray("float value = 1.0;\n"), LINES);
    CodeFileDocument host = OpenHost(directory, original);
    BoundOverlay bound(host);
    bound.RegexOrNull->setChecked(true);
    bound.FindOrNull->setText(QStringLiteral("float"));
    bound.ReplacementOrNull->setText(QStringLiteral("double"));

    QElapsedTimer timer;
    timer.start();
    bound.ReplaceAllOrNull->click();
    const qint64 elapsedMs = timer.elapsed();

    CHECK(DraftOf(host) == RepeatLine(QByteArray("double value = 1.0;\n"), LINES));
    CHECK(bound.StatusOrNull->text() == QStringLiteral("%1 replacements").arg(LINES));
    CHECK(elapsedMs < LINEAR_WORK_CEILING_MS);
}
