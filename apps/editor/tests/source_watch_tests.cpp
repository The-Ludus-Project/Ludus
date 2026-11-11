#include "filesystem_fixture.h"

#include "internal/source_watch.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace ludus::editor;

namespace
{
void Save(const QString& path, const QByteArray& bytes)
{
    QSaveFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(bytes) == bytes.size());
    REQUIRE(file.commit());
}
} // namespace

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
