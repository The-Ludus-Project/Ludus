#include "internal/content_browser.h"
#include "internal/content_import.h"
#include "wav_fixture.h"

#include <ludus/audio/content/definitions.h>

#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QPersistentModelIndex>
#include <QPushButton>
#include <QSignalSpy>
#include <QSortFilterProxyModel>
#include <QSysInfo>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>
#include <utility>

namespace
{
using namespace ludus::foundation;
using namespace ludus::editor;
namespace content = ludus::content;
namespace audio = ludus::audio;
std::string_view View(const QByteArray& bytes)
{
    return {bytes.constData(), static_cast<usize>(bytes.size())};
}
void Write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(bytes) == bytes.size());
}
QString Wav(QTemporaryDir& directory, uint32 frames)
{
    const auto data = audio::test::MakeSineWav(44100, 1, frames);
    const auto path = directory.filePath(QStringLiteral("export.wav"));
    Write(path, QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<qsizetype>(data.size())));
    return path;
}
content::Catalog ReadCatalog(const QString& root)
{
    content::Bytes bytes;
    content::Catalog catalog;
    content::Diagnostic diagnostic;
    const auto encoded = root.toUtf8();
    REQUIRE(content::ReadFile(View(encoded), "catalog.json", content::MAX_DOCUMENT_BYTES, bytes) ==
            content::Status::Ok);
    REQUIRE(catalog.Read(bytes.String(), diagnostic) == content::Status::Ok);
    return catalog;
}
void Wait(ContentWorkspace& workspace)
{
    for (uint32 attempt = 0; workspace.Busy() && attempt < 1000; ++attempt)
    {
        QTest::qWait(10);
    }
    REQUIRE_FALSE(workspace.Busy());
}
struct HookContext final
{
    ContentImportGate* Gate = nullptr;
    QString Path;
    QByteArray Bytes;
    bool Written = false;
};
void Mutate(void* input) noexcept
{
    auto* context = static_cast<HookContext*>(input);
    QFile file(context->Path);
    context->Written = file.open(QIODevice::WriteOnly) && file.write(context->Bytes) == context->Bytes.size();
}
} // namespace

TEST_CASE("Content import switches immutable versions and preserves IDs and old bytes", "[editor][content]")
{
    QTemporaryDir root, exported;
    const auto file = Wav(exported, 500);
    ContentImportGate first;
    REQUIRE(ImportAudioSource(
                {
                    .Root = root.path(),
                    .File = file,
                    .Id = QStringLiteral("source/hit"),
                },
                first)
                .Status == content::Status::Ok);
    auto catalog = ReadCatalog(root.path());
    REQUIRE(catalog.Find("source/hit") != nullptr);
    const auto oldPath = QString::fromUtf8(catalog.Find("source/hit")->Path.Data);
    QFile oldFile(root.filePath(oldPath));
    REQUIRE(oldFile.open(QIODevice::ReadOnly));
    const auto oldBytes = oldFile.readAll();
    (void)Wav(exported, 700);
    ContentImportGate second;
    const auto result =
        ImportAudioSource({ .Root = root.path(), .File = file, .Id = QStringLiteral("source/hit") }, second);
    REQUIRE(result.Status == content::Status::Ok);
    catalog = ReadCatalog(root.path());
    REQUIRE(catalog.Entries().size() == 1);
    CHECK(QString::fromUtf8(catalog.Find("source/hit")->Path.Data) != oldPath);
    oldFile.seek(0);
    CHECK(oldFile.readAll() == oldBytes);
    QFile reopened(root.filePath(oldPath));
    REQUIRE(reopened.open(QIODevice::ReadOnly));
    CHECK(reopened.readAll() == oldBytes);
    Write(file, QByteArrayLiteral("invalid audio"));
    ContentImportGate bad;
    CHECK(ImportAudioSource({ .Root = root.path(), .File = file, .Id = QStringLiteral("source/hit") }, bad).Status ==
          content::Status::Invalid);
    CHECK(QString::fromUtf8(ReadCatalog(root.path()).Find("source/hit")->Path.Data) == result.CandidatePath);
}

TEST_CASE("Cancellation and catalog conflicts never publish a broken active catalog", "[editor][content]")
{
    QTemporaryDir root, exported;
    const auto file = Wav(exported, 500);
    ContentImportGate cancelled;
    HookContext cancelContext;
    cancelContext.Gate = &cancelled;
    ContentImportHooks cancelHooks;
    cancelHooks.Context = &cancelContext;
    cancelHooks.BeforePublish = [](void* input) noexcept { (void)static_cast<HookContext*>(input)->Gate->Cancel(); };
    REQUIRE(ImportAudioSource({ .Root = root.path(), .File = file, .Id = QStringLiteral("source/hit") },
                              cancelled,
                              nullptr,
                              cancelHooks)
                .Status == content::Status::Cancelled);
    CHECK_FALSE(QFile::exists(root.filePath(QStringLiteral("catalog.json"))));
    CHECK_FALSE(QDir(root.filePath(QStringLiteral("sources"))).exists());

    Write(root.filePath(QStringLiteral("catalog.json")), QByteArrayLiteral("{\"version\":1,\"resources\":[]}\n"));
    HookContext conflict;
    conflict.Path = root.filePath(QStringLiteral("catalog.json"));
    conflict.Bytes = QByteArrayLiteral("{\"version\":1,\"resources\":[]}\n\n");
    ContentImportHooks hooks;
    hooks.Context = &conflict;
    hooks.BeforePublish = &Mutate;
    ContentImportGate gate;
    const auto result = ImportAudioSource({ .Root = root.path(), .File = file, .Id = QStringLiteral("source/hit") },
                                          gate,
                                          nullptr,
                                          hooks);
    REQUIRE(conflict.Written);
    CHECK(result.Status == content::Status::Conflict);
    CHECK_FALSE(result.CandidatePath.isEmpty());
    CHECK(QFile::exists(root.filePath(result.CandidatePath)));
    CHECK(ReadCatalog(root.path()).Entries().empty());
}

TEST_CASE("Reimport checks saved loop constraints and changing dependencies", "[editor][content]")
{
    QTemporaryDir root, exported;
    const auto file = Wav(exported, 500);
    ContentImportGate first;
    REQUIRE(ImportAudioSource(
                {
                    .Root = root.path(),
                    .File = file,
                    .Id = QStringLiteral("source/hit"),
                },
                first)
                .Status == content::Status::Ok);
    auto catalog = ReadCatalog(root.path());
    const auto oldPath = QString::fromUtf8(catalog.Find("source/hit")->Path.Data);
    audio::content::Music music;
    REQUIRE(music.Id.Set("music/theme"));
    REQUIRE(music.Source.Set("source/hit"));
    REQUIRE(music.Bus.Set("music"));
    music.Region = {true, 10, 400};
    content::Bytes bytes;
    REQUIRE(audio::content::WriteMusic(music, bytes) == content::Status::Ok);
    Write(root.filePath(QStringLiteral("theme.json")),
          QByteArray(bytes.String().data(), static_cast<qsizetype>(bytes.String().size())));
    content::Resource entry;
    REQUIRE(entry.Id.Set("music/theme"));
    REQUIRE(entry.Path.Set("theme.json"));
    entry.Type = content::Kind::Music;
    REQUIRE(catalog.Put(entry) == content::Status::Ok);
    REQUIRE(catalog.Write(bytes) == content::Status::Ok);
    Write(root.filePath(QStringLiteral("catalog.json")),
          QByteArray(bytes.String().data(), static_cast<qsizetype>(bytes.String().size())));
    (void)Wav(exported, 100);
    ContentImportGate shortSource;
    const auto shortResult =
        ImportAudioSource({ .Root = root.path(), .File = file, .Id = QStringLiteral("source/hit") }, shortSource);
    CHECK(shortResult.Status == content::Status::Invalid);
    CHECK(shortResult.Message.contains(QStringLiteral("saved loop")));
    CHECK(QString::fromUtf8(ReadCatalog(root.path()).Find("source/hit")->Path.Data) == oldPath);
    (void)Wav(exported, 600);
    music.Region.End = 550;
    REQUIRE(audio::content::WriteMusic(music, bytes) == content::Status::Ok);
    HookContext changed;
    changed.Path = root.filePath(QStringLiteral("theme.json"));
    changed.Bytes = QByteArray(bytes.String().data(), static_cast<qsizetype>(bytes.String().size()));
    ContentImportHooks hooks;
    hooks.Context = &changed;
    hooks.BeforePublish = &Mutate;
    ContentImportGate dependency;
    CHECK(ImportAudioSource({ .Root = root.path(), .File = file, .Id = QStringLiteral("source/hit") },
                            dependency,
                            nullptr,
                            hooks)
              .Status == content::Status::Conflict);
    CHECK(changed.Written);
    CHECK(QString::fromUtf8(ReadCatalog(root.path()).Find("source/hit")->Path.Data) == oldPath);
}

TEST_CASE("Content model updates roles without resetting unchanged identities", "[editor][content][model]")
{
    ContentCatalogModel model;
    QVector<ContentRow> rows = {{QStringLiteral("source/a"), QStringLiteral("a.wav"), content::Kind::AudioSource},
                                {QStringLiteral("source/b"), QStringLiteral("b.wav"), content::Kind::AudioSource}};
    model.SetRows(rows);
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
    QPersistentModelIndex selected(model.FindId(QStringLiteral("source/b")));
    model.SetRows(rows);
    CHECK(resets.count() == 0);
    CHECK(changes.count() == 0);
    rows[1].Path = QStringLiteral("new-b.wav");
    model.SetRows(rows);
    CHECK(changes.count() == 1);
    CHECK(resets.count() == 0);
    CHECK(selected.data(Qt::UserRole).toString() == QStringLiteral("source/b"));
    std::swap(rows[0], rows[1]);
    model.SetRows(rows);
    CHECK_FALSE(selected.isValid());
    CHECK(model.FindId(QStringLiteral("source/b")).row() == 0);
}

TEST_CASE("Content workspace preserves focused search and selection and rejects stale results",
          "[editor][content][widget]")
{
    QTemporaryDir root, exported;
    const auto file = Wav(exported, 500);
    ContentImportGate gate;
    REQUIRE(ImportAudioSource({ .Root = root.path(), .File = file, .Id = QStringLiteral("source/hit") }, gate).Status ==
            content::Status::Ok);
    ContentWorkspace workspace;
    auto* jobs = workspace.findChild<ContentJobs*>();
    REQUIRE(jobs != nullptr);
    ContentResult old;
    QObject::connect(jobs, &ContentJobs::Completed, &workspace, [&old](const ContentResult& result) { old = result; });
    workspace.SetProject(root.path(), 1);
    Wait(workspace);
    auto* table = workspace.findChild<QTableView*>(QStringLiteral("contentTable"));
    auto* search = workspace.findChild<QLineEdit*>(QStringLiteral("contentSearch"));
    REQUIRE(table != nullptr);
    REQUIRE(search != nullptr);
    workspace.show();
    workspace.activateWindow();
    table->setCurrentIndex(table->model()->index(0, 0));
    search->setFocus();
    search->setText(QStringLiteral("source"));
    search->setSelection(1, 3);
    const auto selectedText = search->selectedText();
    workspace.SetProject(root.path(), 1);
    workspace.Refresh();
    Wait(workspace);
    CHECK(search->selectedText() == selectedText);
    CHECK(workspace.SelectedId() == QStringLiteral("source/hit"));
    search->setText(QStringLiteral("hidden"));
    CHECK(workspace.SelectedId().isEmpty());
    search->clear();
    CHECK(workspace.SelectedId() == QStringLiteral("source/hit"));
    auto stale = old;
    workspace.SetProject(root.path(), 2);
    Wait(workspace);
    stale.Rows.clear();
    stale.Message = QStringLiteral("stale project");
    Q_EMIT jobs->Completed(stale);
    CHECK(table->model()->rowCount() == 1);
    auto obsoleteOperation = old;
    workspace.Refresh();
    Wait(workspace);
    obsoleteOperation.Rows.clear();
    Q_EMIT jobs->Completed(obsoleteOperation);
    CHECK(table->model()->rowCount() == 1);
    Write(root.filePath(QStringLiteral("catalog.json")), QByteArrayLiteral("broken catalog"));
    workspace.Refresh();
    Wait(workspace);
    CHECK(table->model()->rowCount() == 1);
    CHECK(workspace.findChild<QLabel*>(QStringLiteral("contentStatus"))->text().contains(QStringLiteral("last valid")));
    workspace.Shutdown();
    CHECK(workspace.Finished());
}

TEST_CASE("Content completion runs on the GUI owner and shutdown acknowledges cancellation",
          "[editor][content][concurrency]")
{
    QTemporaryDir root;
    Write(root.filePath(QStringLiteral("catalog.json")), QByteArrayLiteral("{\"version\":1,\"resources\":[]}\n"));
    ContentJobs jobs;
    ContentRequest request;
    request.Root = root.path();
    request.Operation = 42;
    request.ProjectEpoch = 5;
    bool delivered = false;
    bool onGui = false;
    ContentResult result;
    QObject::connect(&jobs, &ContentJobs::Completed, &jobs, [&](const ContentResult& value) {
        delivered = true;
        onGui = QThread::currentThread() == QApplication::instance()->thread();
        result = value;
    });
    REQUIRE(jobs.Start(request));
    CHECK_FALSE(jobs.Start(request));
    jobs.Shutdown();
    for (uint32 attempt = 0; jobs.Finished() == false && attempt < 1000; ++attempt)
    {
        QTest::qWait(10);
    }
    REQUIRE(jobs.Finished());
    CHECK(delivered);
    CHECK(onGui);
    CHECK(result.Request.Operation == 42);
    CHECK(result.Request.ProjectEpoch == 5);
    CHECK_FALSE(jobs.Start(request));
}

TEST_CASE("Destroying active content owners retires copied work without late delivery",
          "[editor][content][concurrency]")
{
    QTemporaryDir root;
    const auto catalog = QByteArrayLiteral("{\"version\":1,\"resources\":[]}\n");
    Write(root.filePath(QStringLiteral("catalog.json")), catalog);
    bool delivered = false;
    for (uint32 attempt = 0; attempt < 32; ++attempt)
    {
        ContentJobs jobs;
        ContentRequest request;
        request.Root = root.path();
        request.Operation = attempt;
        QObject::connect(&jobs, &ContentJobs::Completed, QApplication::instance(), [&](const ContentResult&) {
            delivered = true;
        });
        REQUIRE(jobs.Start(request));
        // Destroy before polling completion; detached retirement must not borrow
        // the GUI owner's implementation or emit a later completion to it.
    }
    QTest::qWait(100);
    CHECK_FALSE(delivered);
    QFile file(root.filePath(QStringLiteral("catalog.json")));
    REQUIRE(file.open(QIODevice::ReadOnly));
    CHECK(file.readAll() == catalog);
}

TEST_CASE("Browser imports acknowledge publication and refresh the selected stable ID", "[editor][content][widget]")
{
    QTemporaryDir root, exported;
    ContentWorkspace workspace;
    workspace.SetProject(root.path(), 9);
    CHECK_FALSE(workspace.Importing());
    Wait(workspace);
    const auto file = Wav(exported, 500);
    REQUIRE(workspace.Import(file, QStringLiteral("source/hit")));
    CHECK(workspace.Importing());
    CHECK_FALSE(workspace.Import(file, QStringLiteral("source/other")));
    Wait(workspace);
    CHECK(workspace.SelectedId() == QStringLiteral("source/hit"));
    auto* table = workspace.findChild<QTableView*>(QStringLiteral("contentTable"));
    REQUIRE(table != nullptr);
    CHECK(table->model()->rowCount() == 1);
    const auto path = table->model()->index(0, 2).data().toString();
    (void)Wav(exported, 700);
    REQUIRE(workspace.Import(file, QStringLiteral("source/hit")));
    Wait(workspace);
    CHECK(workspace.SelectedId() == QStringLiteral("source/hit"));
    CHECK(table->model()->index(0, 2).data().toString() != path);
    workspace.Shutdown();
    CHECK(workspace.Finished());
}

TEST_CASE("Content projection records a 100000 row search and selection baseline", "[editor][content][scale]")
{
    constexpr int ROWS = 100000;
    QVector<ContentRow> rows;
    rows.reserve(ROWS);
    for (int row = 0; row < ROWS; ++row)
    {
        rows.append({QStringLiteral("source/%1").arg(row, 6, 10, QLatin1Char('0')),
                     QStringLiteral("sources/%1.wav").arg(row),
                     content::Kind::AudioSource});
    }
    ContentCatalogModel model;
    QSortFilterProxyModel proxy;
    proxy.setSourceModel(&model);
    proxy.setFilterKeyColumn(-1);
    proxy.setFilterCaseSensitivity(Qt::CaseInsensitive);
    QElapsedTimer timer;
    timer.start();
    model.SetRows(std::move(rows));
    const auto populate = timer.nsecsElapsed();
    QVector<int64> samples;
    for (int query = 0; query < 20; ++query)
    {
        timer.restart();
        proxy.setFilterFixedString(QStringLiteral("%1").arg(query, 3, 10, QLatin1Char('0')));
        REQUIRE(proxy.rowCount() > 0);
        samples.append(timer.nsecsElapsed());
    }
    std::sort(samples.begin(), samples.end());
    timer.restart();
    REQUIRE(model.FindId(QStringLiteral("source/099999")).isValid());
    const auto selection = timer.nsecsElapsed();
    INFO("Synthetic view dataset: " << ROWS << " rows, 20 fixed-string queries; catalog admission remains 4096.");
    INFO("Host: " << QSysInfo::machineHostName().toStdString() << " / " << QSysInfo::prettyProductName().toStdString()
                  << " / " << QSysInfo::currentCpuArchitecture().toStdString() << "; Qt " << qVersion());
    INFO("Nanoseconds: populate=" << populate << ", filter p50=" << samples[10] << ", p95=" << samples[19]
                                  << ", ID lookup=" << selection);
    CHECK(model.rowCount() == ROWS);
    CHECK(model.columnCount() == 3);
}

TEST_CASE("Mesh catalog entries retain identity during audio import and never activate audio",
          "[editor][content][mesh]")
{
    QTemporaryDir root, exported;
    content::Catalog catalog;
    content::Resource mesh;
    mesh.Type = content::Kind::Mesh;
    REQUIRE(mesh.Id.Set("mesh/quad"));
    REQUIRE(mesh.Path.Set("meshes/quad"));
    REQUIRE(catalog.Put(mesh) == content::Status::Ok);
    content::Bytes bytes;
    REQUIRE(catalog.Write(bytes) == content::Status::Ok);
    Write(root.filePath(QStringLiteral("catalog.json")),
          QByteArray(bytes.String().data(), static_cast<qsizetype>(bytes.String().size())));
    const auto file = Wav(exported, 500);
    ContentImportGate gate;
    REQUIRE(ImportAudioSource({ .Root = root.path(), .File = file, .Id = QStringLiteral("source/hit") }, gate).Status ==
            content::Status::Ok);
    const auto saved = ReadCatalog(root.path());
    REQUIRE(saved.Find("mesh/quad") != nullptr);
    CHECK(saved.Find("mesh/quad")->Type == content::Kind::Mesh);
    CHECK(saved.Find("mesh/quad")->Path.View() == "meshes/quad");
    ContentImportGate conflict;
    CHECK(ImportAudioSource(
              {
                  .Root = root.path(),
                  .File = file,
                  .Id = QStringLiteral("mesh/quad"),
              },
              conflict)
              .Status == content::Status::Conflict);

    ContentWorkspace workspace;
    QSignalSpy opened(&workspace, &ContentWorkspace::OpenAudio);
    workspace.SetProject(root.path(), 1);
    Wait(workspace);
    auto* table = workspace.findChild<QTableView*>(QStringLiteral("contentTable"));
    REQUIRE(table != nullptr);
    QModelIndex selected;
    for (int row = 0; row < table->model()->rowCount(); ++row)
    {
        const auto index = table->model()->index(row, 0);
        if (index.data(Qt::UserRole).toString() == QStringLiteral("mesh/quad"))
        {
            selected = index;
        }
    }
    REQUIRE(selected.isValid());
    CHECK(table->model()->index(selected.row(), 1).data().toString() == QStringLiteral("Mesh"));
    table->setCurrentIndex(selected);
    Q_EMIT table->activated(selected);
    CHECK(opened.count() == 0);
    CHECK(workspace.findChild<QLabel*>(QStringLiteral("contentStatus"))->text() ==
          QStringLiteral("Mesh editing is not available yet."));
    CHECK_FALSE(workspace.findChild<QPushButton*>(QStringLiteral("contentReimport"))->isEnabled());
    workspace.Shutdown();
}
