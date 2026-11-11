#include "filesystem_fixture.h"
#include "internal/editor_file_write.h"

#include "internal/source_watch.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#if defined(Q_OS_WIN)
#    include <ludus/foundation/filesystem/filesystem.hpp>
#endif

using namespace ludus::editor;

namespace
{
void Save(const QString& path, const QByteArray& bytes)
{
    QString error;
    const bool isSaved = WriteEditorFileAtomically(path, bytes, error);
    INFO(error.toStdString());
    REQUIRE(isSaved == true);
}
} // namespace

#if defined(Q_OS_WIN)
TEST_CASE("editor save replaces a Windows source while a shared reader keeps its old revision",
          "[editor][watch][windows]")
{
    using ludus::foundation::uint8;
    using ludus::foundation::usize;
    namespace filesystem = ludus::foundation::filesystem;
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const QString file = project.filePath(QStringLiteral("source.slang"));
    Save(file, QByteArrayLiteral("old"));
    const QByteArray root = project.path().toUtf8();
    filesystem::Directory directory;
    REQUIRE(directory.Open({root.constData(), static_cast<usize>(root.size())}).Succeeded() == true);
    filesystem::File previous;
    REQUIRE(directory.OpenRead("source.slang", previous).Succeeded() == true);

    Save(file, QByteArrayLiteral("new"));
    uint8 first = 0;
    REQUIRE(previous.ReadAt(0, {&first, 1}).BytesRead == 1);
    CHECK(first == 'o');
    QFile current(file);
    REQUIRE(current.open(QIODevice::ReadOnly) == true);
    CHECK(current.readAll() == QByteArrayLiteral("new"));
}
#endif

TEST_CASE("source watch coalesces edits during builds and rearms atomic saves", "[editor][watch]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid());
    REQUIRE(QDir(project.path()).mkdir(QStringLiteral("src")));
    Save(project.filePath(QStringLiteral("ludus.play.json")),
         QByteArrayLiteral(
             "{\"version\":1,\"host_target\":\"host\",\"module_target\":\"module\",\"watch_roots\":[\"src\"]}"));
    const auto source = project.filePath(QStringLiteral("src/game.cpp"));
    Save(source, QByteArrayLiteral("A"));
    SourceWatch watch;
    QSignalSpy builds(&watch, &SourceWatch::BuildRequested);
    QSignalSpy errors(&watch, &SourceWatch::Failed);
    REQUIRE(watch.Start(project.path()));
    watch.SetBusy(true);
    Save(source, QByteArrayLiteral("B"));
    QTest::qWait(550);
    Save(source, QByteArrayLiteral("C"));
    QTest::qWait(550);
    REQUIRE(builds.isEmpty());
    watch.SetBusy(false);
    REQUIRE(builds.wait(2000));
    REQUIRE(builds.size() == 1);
    Save(source, QByteArrayLiteral("D"));
    REQUIRE(builds.wait(2000));
    REQUIRE(builds.size() == 2);
    REQUIRE(errors.isEmpty());
    watch.Stop();
    Save(source, QByteArrayLiteral("E"));
    QTest::qWait(550);
    REQUIRE(builds.size() == 2);
}

TEST_CASE("source watch rejects paths outside its project", "[editor][watch]")
{
    QTemporaryDir project;
    QTemporaryDir outside;
    REQUIRE(project.isValid());
    REQUIRE(outside.isValid());
    const auto sidecar = project.filePath(QStringLiteral("ludus.play.json"));
    Save(sidecar, QByteArrayLiteral("{\"watch_roots\":[\"../\"]}"));
    SourceWatch watch;
    REQUIRE_FALSE(watch.Start(project.path()));
    REQUIRE_FALSE(watch.Active());
    REQUIRE(CreateTestSymbolicLink(outside.path(), project.filePath(QStringLiteral("link"))));
    Save(sidecar, QByteArrayLiteral("{\"watch_roots\":[\"link\"]}"));
    REQUIRE_FALSE(watch.Start(project.path()));
}
