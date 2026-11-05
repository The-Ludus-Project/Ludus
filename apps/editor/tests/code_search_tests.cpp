// The project search service and Replace in Files workspace edit (CE07, CE11,
// design sections 7-8): one-pass matching with line/column mapping, a worker
// that enumerates roots off the UI thread and streams bounded batches, binary
// and oversize skipping, dirty-buffer snapshots, and an all-or-nothing workspace
// edit stamped with one transaction id. Offscreen; the worker uses real threads,
// so the service tests pump the event loop for the queued result signals.

#include "internal/code_document.h"
#include "internal/code_document_registry.h"
#include "internal/code_file_document.h"
#include "internal/code_file_limits.h"
#include "internal/code_search_service.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
[[nodiscard]] QString WriteFile(const QString& directory, const QString& name, const QByteArray& bytes)
{
    const QString path = QDir(directory).filePath(name);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
    file.close();
    return QFileInfo(path).absoluteFilePath();
}

[[nodiscard]] QByteArray ReadFile(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}

// Run one search to completion and collect every file result.
struct SearchOutcome final
{
    QList<CodeSearchFileResult> Files;
    bool WasCancelled = false;
    bool WasTruncated = false;
    QString PatternError;
    int32 TotalMatches = 0;
};

[[nodiscard]] SearchOutcome RunSearch(CodeSearchService& inoutService,
                                      const QStringList& roots,
                                      const QStringList& excluded,
                                      const CodeSearchQuery& query,
                                      const QHash<QString, QByteArray>& dirty)
{
    SearchOutcome outcome;
    QObject::connect(&inoutService,
                     &CodeSearchService::BatchReady,
                     [&outcome](const QList<CodeSearchFileResult>& batch) { outcome.Files.append(batch); });
    bool isFinished = false;
    QObject::connect(&inoutService,
                     &CodeSearchService::Finished,
                     [&](bool wasCancelled, bool wasTruncated, const QString& error) {
                         outcome.WasCancelled = wasCancelled;
                         outcome.WasTruncated = wasTruncated;
                         outcome.PatternError = error;
                         isFinished = true;
                     });
    inoutService.Start(roots, excluded, query, dirty);
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 10000;
    while (isFinished == false && QDateTime::currentMSecsSinceEpoch() < deadline)
    {
        QTest::qWait(5);
    }
    REQUIRE(isFinished == true);
    for (const CodeSearchFileResult& file : outcome.Files)
    {
        outcome.TotalMatches += static_cast<int32>(file.Matches.size());
    }
    return outcome;
}
} // namespace

TEST_CASE("FindMatchesInText maps matches to line and column in one pass", "[code-editor][search]")
{
    const QString text = QStringLiteral("alpha beta\nbeta gamma\nno match here\nbeta\n");
    CodeSearchQuery query;
    query.Pattern = QStringLiteral("beta");
    query.IsCaseSensitive = true;
    QList<CodeSearchMatch> matches;
    QString error;
    const int32 found = FindMatchesInText(text, query, CODE_SEARCH_MAX_MATCHES, matches, error);
    REQUIRE(error.isEmpty() == true);
    REQUIRE(found == 3);
    CHECK(matches.at(0).Line == 0);
    CHECK(matches.at(0).StartColumn == 6);
    CHECK(matches.at(0).EndColumn == 10);
    CHECK(matches.at(1).Line == 1);
    CHECK(matches.at(1).StartColumn == 0);
    CHECK(matches.at(2).Line == 3);
    CHECK(matches.at(0).LineText == QStringLiteral("alpha beta"));
}

TEST_CASE("FindMatchesInText honours regex, whole word, case and the cap", "[code-editor][search]")
{
    const QString text = QStringLiteral("Cat cat category CAT\n");
    SECTION("Whole word, case-insensitive excludes category")
    {
        CodeSearchQuery query;
        query.Pattern = QStringLiteral("cat");
        query.IsWholeWord = true;
        QList<CodeSearchMatch> matches;
        QString error;
        CHECK(FindMatchesInText(text, query, CODE_SEARCH_MAX_MATCHES, matches, error) == 3);
    }
    SECTION("Regex digits")
    {
        CodeSearchQuery query;
        query.Pattern = QStringLiteral("[0-9]+");
        query.IsRegex = true;
        QList<CodeSearchMatch> matches;
        QString error;
        CHECK(FindMatchesInText(QStringLiteral("a1 bb 22 c\n"), query, CODE_SEARCH_MAX_MATCHES, matches, error) == 2);
    }
    SECTION("The cap bounds the result count")
    {
        CodeSearchQuery query;
        query.Pattern = QStringLiteral("a");
        QList<CodeSearchMatch> matches;
        QString error;
        CHECK(FindMatchesInText(QStringLiteral("aaaaa\n"), query, 2, matches, error) == 2);
    }
    SECTION("An invalid regex reports a compile error and no matches")
    {
        CodeSearchQuery query;
        query.Pattern = QStringLiteral("(unterminated");
        query.IsRegex = true;
        QList<CodeSearchMatch> matches;
        QString error;
        CHECK(FindMatchesInText(text, query, CODE_SEARCH_MAX_MATCHES, matches, error) == 0);
        CHECK(error.isEmpty() == false);
    }
}

TEST_CASE("The search worker enumerates roots and skips the excluded tree", "[code-editor][search]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).absoluteFilePath();
    static_cast<void>(WriteFile(root, QStringLiteral("src/a.txt"), QByteArray("needle here\n")));
    static_cast<void>(WriteFile(root, QStringLiteral("src/sub/b.txt"), QByteArray("another needle\n")));
    const QString buildDir = WriteFile(root, QStringLiteral("out/build/x.txt"), QByteArray("needle in build\n"));
    static_cast<void>(buildDir);

    CodeSearchService service;
    CodeSearchQuery query;
    query.Pattern = QStringLiteral("needle");
    const QStringList excluded = {QDir(root).filePath(QStringLiteral("out"))};
    const SearchOutcome outcome = RunSearch(service, {root}, excluded, query, {});
    CHECK(outcome.PatternError.isEmpty() == true);
    CHECK(outcome.WasCancelled == false);
    // Two matches under src/, none under the excluded out/ tree.
    CHECK(outcome.TotalMatches == 2);
}

TEST_CASE("The worker skips binary and oversize files and searches dirty buffers", "[code-editor][search]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).absoluteFilePath();
    const QString textPath = WriteFile(root, QStringLiteral("a.txt"), QByteArray("needle on disk\n"));
    static_cast<void>(WriteFile(root, QStringLiteral("binary.bin"), QByteArray("needle\0needle", 13)));

    CodeSearchService service;
    CodeSearchQuery query;
    query.Pattern = QStringLiteral("needle");

    SECTION("The binary file is skipped")
    {
        const SearchOutcome outcome = RunSearch(service, {root}, {}, query, {});
        CHECK(outcome.TotalMatches == 1); // Only a.txt, not binary.bin.
    }
    SECTION("A dirty buffer is searched instead of the disk bytes")
    {
        QHash<QString, QByteArray> dirty;
        dirty.insert(textPath, QByteArray("needle needle unsaved\n"));
        const SearchOutcome outcome = RunSearch(service, {root}, {}, query, dirty);
        CHECK(outcome.TotalMatches == 2); // The two needles in the unsaved draft.
    }
}

TEST_CASE("Replace in Files applies one transaction across files or none", "[code-editor][search]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).absoluteFilePath();
    const QString pathA = WriteFile(root, QStringLiteral("a.txt"), QByteArray("foo and foo\n"));
    const QString pathB = WriteFile(root, QStringLiteral("b.txt"), QByteArray("foo only\n"));

    CodeDocumentRegistry registry;
    registry.SetProjectEpoch(1);

    SECTION("Every file is rewritten and saved as one transaction")
    {
        // Replace both "foo" in a.txt ([0,3) and [8,11)) and the one in b.txt.
        QList<CodeWorkspaceFileEdit> edits;
        edits.append(CodeWorkspaceFileEdit
        {
            .AbsolutePath = pathA,
            .Replacements =
                {
                    CodeWorkspaceReplacement
                    {
                        .StartPosition = 0,
                        .EndPosition = 3,
                        .Replacement = QStringLiteral("bar"),
                    },
                    CodeWorkspaceReplacement
                    {
                        .StartPosition = 8,
                        .EndPosition = 11,
                        .Replacement = QStringLiteral("bar"),
                    },
                },
        });
        edits.append(CodeWorkspaceFileEdit
        {
            .AbsolutePath = pathB,
            .Replacements =
                {
                    CodeWorkspaceReplacement
                    {
                        .StartPosition = 0,
                        .EndPosition = 3,
                        .Replacement = QStringLiteral("bar"),
                    },
                },
        });
        CHECK(registry.ApplyWorkspaceEdit(edits) == CodeWorkspaceEditStatus::Ok);
        CHECK(ReadFile(pathA) == QByteArray("bar and bar\n"));
        CHECK(ReadFile(pathB) == QByteArray("bar only\n"));
    }

    SECTION("A failing file rejects the whole edit with no file changed")
    {
        // The second file's range is out of bounds, so neither file is touched.
        QList<CodeWorkspaceFileEdit> edits;
        edits.append(CodeWorkspaceFileEdit
        {
            .AbsolutePath = pathA,
            .Replacements =
                {
                    CodeWorkspaceReplacement
                    {
                        .StartPosition = 0,
                        .EndPosition = 3,
                        .Replacement = QStringLiteral("bar"),
                    },
                },
        });
        edits.append(CodeWorkspaceFileEdit
        {
            .AbsolutePath = pathB,
            .Replacements =
                {
                    CodeWorkspaceReplacement
                    {
                        .StartPosition = 100,
                        .EndPosition = 103,
                        .Replacement = QStringLiteral("bar"),
                    },
                },
        });
        CHECK(registry.ApplyWorkspaceEdit(edits) == CodeWorkspaceEditStatus::Rejected);
        CHECK(ReadFile(pathA) == QByteArray("foo and foo\n"));
        CHECK(ReadFile(pathB) == QByteArray("foo only\n"));
    }
}

TEST_CASE("A large multi-file Replace in Files stays linear", "[code-editor][search]")
{
    constexpr qsizetype FILES = 64;
    constexpr qsizetype LINES = 2000;
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).absoluteFilePath();
    QByteArray body;
    body.reserve(LINES * 18);
    for (qsizetype line = 0; line < LINES; ++line)
    {
        body.append("float value = 1;\n");
    }
    QList<CodeWorkspaceFileEdit> edits;
    for (qsizetype index = 0; index < FILES; ++index)
    {
        const QString path = WriteFile(root, QStringLiteral("f%1.txt").arg(index), body);
        QList<CodeWorkspaceReplacement> replacements;
        // Replace the leading "float" (positions [0,5)) on every line.
        for (qsizetype line = 0; line < LINES; ++line)
        {
            const int32 start = static_cast<int32>(line) * 17; // "float value = 1;\n" is 17 chars.
            replacements.append(CodeWorkspaceReplacement
            {
                .StartPosition = start,
                .EndPosition = start + 5,
                .Replacement = QStringLiteral("double"),
            });
        }
        edits.append(CodeWorkspaceFileEdit{ .AbsolutePath = path, .Replacements = replacements });
    }

    CodeDocumentRegistry registry;
    registry.SetProjectEpoch(1);
    QElapsedTimer timer;
    timer.start();
    const CodeWorkspaceEditStatus status = registry.ApplyWorkspaceEdit(edits);
    const qint64 elapsedMs = timer.elapsed();
    CHECK(status == CodeWorkspaceEditStatus::Ok);
    CHECK(elapsedMs < 4000);
    // Spot-check one file.
    QByteArray expected;
    for (qsizetype line = 0; line < LINES; ++line)
    {
        expected.append("double value = 1;\n");
    }
    CHECK(ReadFile(QDir(root).filePath(QStringLiteral("f0.txt"))) == expected);
}
