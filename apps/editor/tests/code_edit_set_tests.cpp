// Edit sets on the document seam (CE05): the whole-set validator rejects an
// unordered, overlapping or stale set with the buffer unchanged, and one undo
// step restores both the text and the selection set. Host-level and offscreen;
// no widget is constructed. These run against the real CodeFileDocument and the
// shared ValidateAndApplyEditSet, not a mock.

#include "internal/code_document.h"
#include "internal/code_edit_set.h"
#include "internal/code_file_document.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QDir>
#include <QFile>
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
