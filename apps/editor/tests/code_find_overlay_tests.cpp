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
        Find = Panel->findChild<QLineEdit*>(QStringLiteral("codeEditorFind"));
        Replacement = Panel->findChild<QLineEdit*>(QStringLiteral("codeEditorReplacement"));
        ReplaceAll = Panel->findChild<QAbstractButton*>(QStringLiteral("codeEditorReplaceAll"));
        Replace = Panel->findChild<QAbstractButton*>(QStringLiteral("codeEditorReplace"));
        FindNext = Panel->findChild<QAbstractButton*>(QStringLiteral("codeEditorFindNext"));
        FindPrevious = Panel->findChild<QAbstractButton*>(QStringLiteral("codeEditorFindPrevious"));
        Status = Panel->findChild<QLabel*>(QStringLiteral("codeEditorFindStatus"));
        Regex = Panel->findChild<QCheckBox*>(QStringLiteral("codeEditorRegex"));
        WholeWord = Panel->findChild<QCheckBox*>(QStringLiteral("codeEditorWholeWord"));
        InSelection = Panel->findChild<QCheckBox*>(QStringLiteral("codeEditorInSelection"));
        PreserveCase = Panel->findChild<QCheckBox*>(QStringLiteral("codeEditorPreserveCase"));
        // "Match case" has no object name in the original panel; find it by text.
        for (QCheckBox* candidate : Panel->findChildren<QCheckBox*>())
        {
            if (candidate->text() == QStringLiteral("Match case"))
            {
                MatchCase = candidate;
            }
        }
        REQUIRE(Find != nullptr);
        REQUIRE(Replacement != nullptr);
        REQUIRE(ReplaceAll != nullptr);
        REQUIRE(Status != nullptr);
        REQUIRE(Regex != nullptr);
        REQUIRE(WholeWord != nullptr);
        REQUIRE(InSelection != nullptr);
        REQUIRE(PreserveCase != nullptr);
        REQUIRE(MatchCase != nullptr);
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
    QLineEdit* Find = nullptr;
    QLineEdit* Replacement = nullptr;
    QAbstractButton* ReplaceAll = nullptr;
    QAbstractButton* Replace = nullptr;
    QAbstractButton* FindNext = nullptr;
    QAbstractButton* FindPrevious = nullptr;
    QLabel* Status = nullptr;
    QCheckBox* Regex = nullptr;
    QCheckBox* WholeWord = nullptr;
    QCheckBox* InSelection = nullptr;
    QCheckBox* PreserveCase = nullptr;
    QCheckBox* MatchCase = nullptr;
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
    bound.Regex->setChecked(true);
    bound.Find->setText(QStringLiteral("([a-z])([0-9]+)"));
    bound.Replacement->setText(QStringLiteral("$2$1"));
    bound.ReplaceAll->click();
    CHECK(DraftOf(host) == QByteArray("1a 22b 333c\n"));
    CHECK(bound.Status->text() == QStringLiteral("3 replacements"));
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
    bound.WholeWord->setChecked(true);
    bound.MatchCase->setChecked(true);
    bound.Find->setText(QStringLiteral("in"));
    bound.Replacement->setText(QStringLiteral("ON"));
    bound.ReplaceAll->click();
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
    bound.InSelection->setChecked(true);
    bound.MatchCase->setChecked(true);
    bound.Find->setText(QStringLiteral("x"));
    bound.Replacement->setText(QStringLiteral("y"));
    bound.ReplaceAll->click();
    // The third line is outside the selection and is untouched.
    CHECK(DraftOf(host) == QByteArray("y y\ny y\nx x\n"));
}

TEST_CASE("Preserve case matches the case of each replaced occurrence", "[code-editor][find-overlay]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("foo Foo FOO\n"));
    BoundOverlay bound(host);
    bound.PreserveCase->setChecked(true);
    bound.Find->setText(QStringLiteral("foo")); // Case-insensitive by default.
    bound.Replacement->setText(QStringLiteral("bar"));
    bound.ReplaceAll->click();
    CHECK(DraftOf(host) == QByteArray("bar Bar BAR\n"));
}

TEST_CASE("The match count reports the total and the current match", "[code-editor][find-overlay]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("a a a\n"));
    BoundOverlay bound(host);
    bound.MatchCase->setChecked(true);
    bound.Find->setText(QStringLiteral("a"));
    // Before navigating, the total is shown.
    CHECK(bound.Status->text() == QStringLiteral("3 matches"));
    bound.FindNext->click();
    CHECK(bound.Status->text() == QStringLiteral("1 of 3"));
    bound.FindNext->click();
    CHECK(bound.Status->text() == QStringLiteral("2 of 3"));
    bound.FindPrevious->click();
    CHECK(bound.Status->text() == QStringLiteral("1 of 3"));
}

TEST_CASE("An invalid regular expression is reported and nothing changes", "[code-editor][find-overlay]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("abc\n"));
    BoundOverlay bound(host);
    bound.Regex->setChecked(true);
    bound.Find->setText(QStringLiteral("(unterminated"));
    bound.Replacement->setText(QStringLiteral("x"));
    bound.ReplaceAll->click();
    CHECK(DraftOf(host) == QByteArray("abc\n"));
    CHECK(bound.Status->text().startsWith(QStringLiteral("Invalid pattern")) == true);
}

TEST_CASE("Find next wraps at the end of the document", "[code-editor][find-overlay]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("v V v\n"));
    BoundOverlay bound(host);
    bound.Find->setText(QStringLiteral("v")); // Case-insensitive: three matches.
    bound.Editor->moveCursor(QTextCursor::End);
    bound.FindNext->click();
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
    bound.Regex->setChecked(true);
    bound.Find->setText(QStringLiteral("float"));
    bound.Replacement->setText(QStringLiteral("double"));

    QElapsedTimer timer;
    timer.start();
    bound.ReplaceAll->click();
    const qint64 elapsedMs = timer.elapsed();

    CHECK(DraftOf(host) == RepeatLine(QByteArray("double value = 1.0;\n"), LINES));
    CHECK(bound.Status->text() == QStringLiteral("%1 replacements").arg(LINES));
    CHECK(elapsedMs < LINEAR_WORK_CEILING_MS);
}
