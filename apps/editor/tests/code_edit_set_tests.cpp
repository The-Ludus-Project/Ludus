// Edit sets on the document seam (CE05): the whole-set validator rejects an
// unordered, overlapping or stale set with the buffer unchanged, and one undo
// step restores both the text and the selection set. Host-level and offscreen;
// no widget is constructed. These run against the real CodeFileDocument and the
// shared ValidateAndApplyEditSet, not a mock.

#include "internal/code_document.h"
#include "internal/code_document_controller.h"
#include "internal/code_edit_set.h"
#include "internal/code_editor.h"
#include "internal/code_editor_panel.h"
#include "internal/code_file_document.h"
#include "internal/code_text.h"

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

#include <utility>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
[[nodiscard]] QString WriteFile(const QTemporaryDir& directory, const QString& name, const QByteArray& bytes)
{
    const QString path = QDir(directory.path()).filePath(name);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
    file.close();
    return path;
}

[[nodiscard]] CodeFileDocument OpenHost(const QTemporaryDir& directory, const QByteArray& bytes)
{
    const QString path = WriteFile(directory, QStringLiteral("edit.txt"), bytes);
    return CodeFileDocument(LoadCodeFile(path), 1);
}

[[nodiscard]] QByteArray DraftOf(const CodeFileDocument& host)
{
    const CodeDocumentSnapshot snapshot = host.GetSnapshot();
    return QByteArray(snapshot.Text.GetDataOrNull(), static_cast<qsizetype>(snapshot.Text.GetSize()));
}

// Build an edit set borrowing the caller-owned arrays. The caller keeps edits,
// insertedBytes, before and after alive for the duration of the call.
[[nodiscard]] CodeEditSet
MakeSet(const QList<CodeTextEdit>& edits, const QList<CodeTextSelection>& before, const QList<CodeTextSelection>& after)
{
    const CodeEditSet set =
    {
        .Edits = {edits.constData(), static_cast<usize>(edits.size())},
        .Before = {{before.constData(), static_cast<usize>(before.size())}},
        .After = {{after.constData(), static_cast<usize>(after.size())}},
        .Transaction = {},
    };
    return set;
}

// Generous wall-clock ceiling for work that is linear in the text. The
// quadratic versions these tests guard against took 8 s to 100 s on the same
// inputs, so the ceiling separates the two even on a loaded sanitizer runner.
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

// A panel bound to a real file host, as the render-graph Source pane binds its
// own. The panel owns the editor; the controller is deleted first because its
// destructor disconnects from the editor.
struct BoundPanel final
{
    explicit BoundPanel(CodeFileDocument& inoutHost)
        : Editor(new CodeEditor()), Controller(new CodeDocumentController(*Editor)),
          Panel(new CodeEditorPanel(Editor, Controller))
    {
        Controller->Bind(&inoutHost);
    }
    ~BoundPanel()
    {
        delete Controller;
        delete Panel;
    }
    BoundPanel(const BoundPanel&) = delete;
    BoundPanel& operator=(const BoundPanel&) = delete;

    CodeEditor* Editor;
    CodeDocumentController* Controller;
    CodeEditorPanel* Panel;
};
} // namespace

TEST_CASE("An overlapping edit set is rejected and the buffer is unchanged", "[code-editor][edit-set]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("abcdef"));
    const CodeDocumentSnapshot snapshot = host.GetSnapshot();

    // Two edits whose removed ranges overlap: [1,4) and [3,5).
    const char firstInsert[] = {'X'};
    const char secondInsert[] = {'Y'};
    QList<CodeTextEdit> edits;
    edits.append(CodeTextEdit{ .StartBytes = 1, .RemovedBytes = 3, .Inserted = {firstInsert, 1} });
    edits.append(CodeTextEdit{ .StartBytes = 3, .RemovedBytes = 2, .Inserted = {secondInsert, 1} });
    const QList<CodeTextSelection> before = {CodeTextSelection{0, 0}};
    const QList<CodeTextSelection> after = {CodeTextSelection{0, 0}};

    const CodeEditStatus status = host.ApplyEdits(snapshot.Identity, MakeSet(edits, before, after), false);
    CHECK(status == CodeEditStatus::Conflict);
    CHECK(DraftOf(host) == QByteArray("abcdef"));
    CHECK(host.CanUndo() == false);
}

TEST_CASE("An unordered edit set is rejected and the buffer is unchanged", "[code-editor][edit-set]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("abcdef"));
    const CodeDocumentSnapshot snapshot = host.GetSnapshot();

    // Descending StartBytes: the set is not ascending and is rejected whole.
    const char firstInsert[] = {'X'};
    const char secondInsert[] = {'Y'};
    QList<CodeTextEdit> edits;
    edits.append(CodeTextEdit{ .StartBytes = 4, .RemovedBytes = 0, .Inserted = {firstInsert, 1} });
    edits.append(CodeTextEdit{ .StartBytes = 1, .RemovedBytes = 0, .Inserted = {secondInsert, 1} });
    const QList<CodeTextSelection> before = {CodeTextSelection{0, 0}};
    const QList<CodeTextSelection> after = {CodeTextSelection{0, 0}};

    const CodeEditStatus status = host.ApplyEdits(snapshot.Identity, MakeSet(edits, before, after), false);
    CHECK(status == CodeEditStatus::Conflict);
    CHECK(DraftOf(host) == QByteArray("abcdef"));
    CHECK(host.CanUndo() == false);
}

TEST_CASE("A stale edit set is rejected and the buffer is unchanged", "[code-editor][edit-set]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("abcdef"));
    const CodeDocumentIdentity stale = host.GetSnapshot().Identity;

    // Advance the document so `stale` no longer matches.
    const char insert[] = {'Z'};
    QList<CodeTextEdit> firstEdits;
    firstEdits.append(CodeTextEdit{ .StartBytes = 0, .RemovedBytes = 0, .Inserted = {insert, 1} });
    const QList<CodeTextSelection> before = {CodeTextSelection{0, 0}};
    const QList<CodeTextSelection> after = {CodeTextSelection{1, 1}};
    REQUIRE(host.ApplyEdits(host.GetSnapshot().Identity, MakeSet(firstEdits, before, after), false) ==
            CodeEditStatus::Ok);
    const QByteArray afterFirst = DraftOf(host);

    // The second set carries the pre-edit (stale) identity and must be rejected.
    const char second[] = {'Q'};
    QList<CodeTextEdit> secondEdits;
    secondEdits.append(CodeTextEdit{ .StartBytes = 0, .RemovedBytes = 0, .Inserted = {second, 1} });
    const CodeEditStatus status = host.ApplyEdits(stale, MakeSet(secondEdits, before, after), false);
    CHECK(status == CodeEditStatus::Conflict);
    CHECK(DraftOf(host) == afterFirst);
}

TEST_CASE("One undo step restores both the text and the selection set", "[code-editor][edit-set]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("one two one"));
    const CodeDocumentSnapshot snapshot = host.GetSnapshot();

    // Replace both occurrences of "one" with "1" as one set: [0,3) and [8,11).
    const char replacement[] = {'1'};
    QList<CodeTextEdit> edits;
    edits.append(CodeTextEdit{ .StartBytes = 0, .RemovedBytes = 3, .Inserted = {replacement, 1} });
    edits.append(CodeTextEdit{ .StartBytes = 8, .RemovedBytes = 3, .Inserted = {replacement, 1} });
    // Before: two cursors on each "one"; After: two collapsed cursors after each "1".
    const QList<CodeTextSelection> before = {CodeTextSelection{0, 3}, CodeTextSelection{8, 11}};
    const QList<CodeTextSelection> after = {CodeTextSelection{1, 1}, CodeTextSelection{6, 6}};

    REQUIRE(host.ApplyEdits(snapshot.Identity, MakeSet(edits, before, after), false) == CodeEditStatus::Ok);
    CHECK(DraftOf(host) == QByteArray("1 two 1"));
    REQUIRE(host.CanUndo() == true);
    // The whole set is one undo step: no second undo is available from it.
    CHECK(host.CanRedo() == false);

    QList<CodeTextSelection> undoSelections;
    REQUIRE(host.Undo(undoSelections) == true);
    CHECK(DraftOf(host) == QByteArray("one two one"));
    REQUIRE(undoSelections.size() == 2);
    CHECK(undoSelections.at(0).AnchorBytes == 0);
    CHECK(undoSelections.at(0).CaretBytes == 3);
    CHECK(undoSelections.at(1).AnchorBytes == 8);
    CHECK(undoSelections.at(1).CaretBytes == 11);
    // One undo was enough: the document is back to its original state.
    CHECK(host.CanUndo() == false);

    QList<CodeTextSelection> redoSelections;
    REQUIRE(host.Redo(redoSelections) == true);
    CHECK(DraftOf(host) == QByteArray("1 two 1"));
    REQUIRE(redoSelections.size() == 2);
    CHECK(redoSelections.at(0).CaretBytes == 1);
    CHECK(redoSelections.at(1).CaretBytes == 6);
}

TEST_CASE("The validator rejects an out-of-range or boundary-splitting set", "[code-editor][edit-set]")
{
    // Two-byte scalar (U+00E9) so an interior offset is not a scalar boundary.
    const QByteArray original = QByteArray("a\xC3\xA9z");
    const usize limit = 4096;

    SECTION("An edit past the end is InvalidText")
    {
        const char insert[] = {'X'};
        QList<CodeTextEdit> edits;
        edits.append(CodeTextEdit{ .StartBytes = 99, .RemovedBytes = 0, .Inserted = {insert, 1} });
        const CodeEditSet set =
        {
            .Edits = {edits.constData(), static_cast<usize>(edits.size())},
            .Before = {},
            .After = {},
            .Transaction = {},
        };
        QByteArray result = QByteArray("sentinel");
        CHECK(
            ValidateAndApplyEditSet({original.constData(), static_cast<usize>(original.size())}, limit, set, result) ==
            CodeEditStatus::InvalidText);
        CHECK(result == QByteArray("sentinel"));
    }

    SECTION("An edit that splits a scalar is InvalidText")
    {
        const char insert[] = {'X'};
        QList<CodeTextEdit> edits;
        // Offset 2 is inside the two-byte U+00E9.
        edits.append(CodeTextEdit{ .StartBytes = 2, .RemovedBytes = 0, .Inserted = {insert, 1} });
        const CodeEditSet set =
        {
            .Edits = {edits.constData(), static_cast<usize>(edits.size())},
            .Before = {},
            .After = {},
            .Transaction = {},
        };
        QByteArray result = QByteArray("sentinel");
        CHECK(
            ValidateAndApplyEditSet({original.constData(), static_cast<usize>(original.size())}, limit, set, result) ==
            CodeEditStatus::InvalidText);
        CHECK(result == QByteArray("sentinel"));
    }
}

TEST_CASE("Forward byte-offset mapping agrees with the walk from the start", "[code-editor][edit-set]")
{
    // ASCII, two-, three- and four-byte scalars, CRLF and a lone CR. Positions
    // inside the surrogate pair or the CRLF have no byte offset.
    const QByteArray text = QByteArray("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\r\nb\rc");
    CodePositionOffset from;
    for (int32 position = 0; position <= text.size(); ++position)
    {
        usize expected = 0;
        const bool canMap = GetCodeByteOffset(text, position, expected);
        usize actual = 0;
        CodePositionOffset probe = from;
        CHECK(AdvanceCodeByteOffset(text, position, probe, actual) == canMap);
        if (canMap == true)
        {
            CHECK(actual == expected);
            CHECK(probe.Position == position);
            CHECK(probe.OffsetBytes == expected);
            from = probe;
        }
        else
        {
            CHECK(probe.Position == from.Position);
            CHECK(probe.OffsetBytes == from.OffsetBytes);
        }
    }

    SECTION("A position before the starting pair is rejected and outputs survive")
    {
        usize offset = 7;
        CodePositionOffset late = from;
        REQUIRE(late.Position > 0);
        CHECK(AdvanceCodeByteOffset(text, late.Position - 1, late, offset) == false);
        CHECK(offset == 7);
        CHECK(late.Position == from.Position);
        CHECK(late.OffsetBytes == from.OffsetBytes);
    }
}

TEST_CASE("Mapping and validating 65,536 Replace All edits stays linear", "[code-editor][edit-set]")
{
    const QByteArray text = RepeatLine(QByteArray("float value = 1.0;\n"), 65536);
    const QString decoded = QString::fromUtf8(text);
    const QByteArray replacement("double");

    QElapsedTimer timer;
    timer.start();
    QList<CodeTextEdit> edits;
    CodePositionOffset mapped;
    qsizetype from = 0;
    while (true)
    {
        const qsizetype at = decoded.indexOf(QStringLiteral("float"), from);
        if (at < 0)
        {
            break;
        }
        usize startBytes = 0;
        usize endBytes = 0;
        REQUIRE(AdvanceCodeByteOffset(text, static_cast<int32>(at), mapped, startBytes) == true);
        REQUIRE(AdvanceCodeByteOffset(text, static_cast<int32>(at + 5), mapped, endBytes) == true);
        edits.append(CodeTextEdit
        {
            .StartBytes = startBytes,
            .RemovedBytes = endBytes - startBytes,
            .Inserted = {replacement.constData(), static_cast<usize>(replacement.size())},
        });
        from = at + 5;
    }
    REQUIRE(edits.size() == 65536);
    const QList<CodeTextSelection> none;
    QByteArray result;
    REQUIRE(ValidateAndApplyEditSet({text.constData(), static_cast<usize>(text.size())},
                                    usize{8} << 20,
                                    MakeSet(edits, none, none),
                                    result) == CodeEditStatus::Ok);
    const qint64 elapsedMs = timer.elapsed();
    CHECK(result == RepeatLine(QByteArray("double value = 1.0;\n"), 65536));
    CHECK(elapsedMs < LINEAR_WORK_CEILING_MS);
}

TEST_CASE("Replace All in a large file applies every match as one undo step", "[code-editor][edit-set]")
{
    constexpr qsizetype LINES = 16384;
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QByteArray original = RepeatLine(QByteArray("float value = 1.0;\n"), LINES);
    CodeFileDocument host = OpenHost(directory, original);
    BoundPanel bound(host);
    REQUIRE(bound.Editor->toPlainText().size() == original.size());

    QLineEdit* find = bound.Panel->findChild<QLineEdit*>(QStringLiteral("codeEditorFind"));
    QLineEdit* replacement = bound.Panel->findChild<QLineEdit*>(QStringLiteral("codeEditorReplacement"));
    QAbstractButton* replaceAll = bound.Panel->findChild<QAbstractButton*>(QStringLiteral("codeEditorReplaceAll"));
    QLabel* status = bound.Panel->findChild<QLabel*>(QStringLiteral("codeEditorFindStatus"));
    REQUIRE(find != nullptr);
    REQUIRE(replacement != nullptr);
    REQUIRE(replaceAll != nullptr);
    REQUIRE(status != nullptr);
    find->setText(QStringLiteral("float"));
    replacement->setText(QStringLiteral("double"));

    QElapsedTimer timer;
    timer.start();
    replaceAll->click();
    const qint64 elapsedMs = timer.elapsed();

    const QByteArray expected = RepeatLine(QByteArray("double value = 1.0;\n"), LINES);
    CHECK(DraftOf(host) == expected);
    CHECK(status->text() == QStringLiteral("%1 replacements").arg(LINES));
    CHECK(elapsedMs < LINEAR_WORK_CEILING_MS);

    QList<CodeTextSelection> selections;
    REQUIRE(host.Undo(selections) == true);
    CHECK(DraftOf(host) == original);
    CHECK(host.CanUndo() == false);
}

TEST_CASE("Replace All matches like the editor across CRLF, case and non-ASCII text", "[code-editor][edit-set]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    // A four-byte scalar (two UTF-16 units), a two-byte scalar and CRLF line
    // endings all shift byte offsets away from editor positions.
    const QByteArray original("\xF0\x9F\x98\x80 Float a;\r\n\xC3\xA9 float b; FLOAT c;\r\n");
    CodeFileDocument host = OpenHost(directory, original);
    BoundPanel bound(host);

    QLineEdit* find = bound.Panel->findChild<QLineEdit*>(QStringLiteral("codeEditorFind"));
    QLineEdit* replacement = bound.Panel->findChild<QLineEdit*>(QStringLiteral("codeEditorReplacement"));
    QAbstractButton* replaceAll = bound.Panel->findChild<QAbstractButton*>(QStringLiteral("codeEditorReplaceAll"));
    QCheckBox* matchCaseOrNull = nullptr;
    for (QCheckBox* candidate : bound.Panel->findChildren<QCheckBox*>())
    {
        if (candidate->text() == QStringLiteral("Match case"))
        {
            matchCaseOrNull = candidate;
        }
    }
    REQUIRE(find != nullptr);
    REQUIRE(replacement != nullptr);
    REQUIRE(replaceAll != nullptr);
    REQUIRE(matchCaseOrNull != nullptr);
    find->setText(QStringLiteral("float"));
    replacement->setText(QStringLiteral("double"));

    SECTION("Ignoring case replaces every spelling")
    {
        matchCaseOrNull->setChecked(false);
        replaceAll->click();
        CHECK(DraftOf(host) == QByteArray("\xF0\x9F\x98\x80 double a;\r\n\xC3\xA9 double b; double c;\r\n"));
    }
    SECTION("Matching case replaces only the exact spelling")
    {
        matchCaseOrNull->setChecked(true);
        replaceAll->click();
        CHECK(DraftOf(host) == QByteArray("\xF0\x9F\x98\x80 Float a;\r\n\xC3\xA9 double b; FLOAT c;\r\n"));
    }
    QList<CodeTextSelection> selections;
    REQUIRE(host.Undo(selections) == true);
    CHECK(DraftOf(host) == original);
}
