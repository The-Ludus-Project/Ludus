#include "internal/content_browser.h"
#include "internal/content_import.h"
#include "internal/state_view.h"
#include "wav_fixture.h"

#include <ludus/audio/content/definitions.h>

#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHeaderView>
#include <QImage>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPersistentModelIndex>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QSortFilterProxyModel>
#include <QSysInfo>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QToolButton>
#include <QtEndian>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
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
ContentRow
MakeRow(const QString& id, const QString& path, ContentType type, const QString& key = {}, const QString& label = {})
{
    ContentRow row;
    row.Id = id;
    row.Path = path;
    row.Type = type;
    row.Key = key;
    row.Label = label;
    return row;
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
    QVector<ContentRow> rows = {MakeRow(QStringLiteral("source/a"), QStringLiteral("a.wav"), ContentType::AudioSource),
                                MakeRow(QStringLiteral("source/b"), QStringLiteral("b.wav"), ContentType::AudioSource)};
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
    ContentJobs* jobs = workspace.findChild<ContentJobs*>();
    REQUIRE(jobs != nullptr);
    ContentResult old;
    QObject::connect(jobs, &ContentJobs::Completed, &workspace, [&old](const ContentResult& result) { old = result; });
    workspace.SetProject(root.path(), 1);
    Wait(workspace);
    QTableView* table = workspace.findChild<QTableView*>(QStringLiteral("contentTable"));
    QLineEdit* search = workspace.findChild<QLineEdit*>(QStringLiteral("contentSearch"));
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
    // The failure also reads as a state: a sentence, the worker's message as the cause, a fix and a Refresh action.
    StateView* problem = workspace.findChild<StateView*>(QStringLiteral("contentProblem"));
    REQUIRE(problem != nullptr);
    CHECK(problem->isVisibleTo(&workspace) == true);
    CHECK(problem->GetContent().Message == QStringLiteral("The content catalog could not be read."));
    CHECK(problem->GetContent().Cause.contains(QStringLiteral("last valid")) == true);
    CHECK(problem->GetContent().Fix.isEmpty() == false);
    CHECK(problem->GetContent().ActionLabel == QStringLiteral("Refresh"));
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

#if defined(LUDUS_EDITOR_COOPERATIVE_CONTENT_JOBS)
TEST_CASE("Browser content cancellation before dispatch publishes no source or catalog", "[editor][content][browser]")
{
    QTemporaryDir root;
    QTemporaryDir exported;
    const QString file = Wav(exported, 500);
    ContentJobs jobs;
    ContentRequest request;
    request.Root = root.path();
    request.File = file;
    request.Id = QStringLiteral("source/cancelled");
    content::Status status = content::Status::Pending;
    QObject::connect(&jobs, &ContentJobs::Completed, &jobs, [&](const ContentResult& result) {
        status = result.Status;
    });
    REQUIRE(jobs.Start(request) == true);
    REQUIRE(jobs.Cancel() == true);
    while (jobs.Finished() == false)
    {
        QTest::qWait(10);
    }
    CHECK(status == content::Status::Cancelled);
    CHECK(QFile::exists(root.filePath(QStringLiteral("catalog.json"))) == false);
    CHECK(QDir(root.filePath(QStringLiteral("sources"))).exists() == false);
}
#endif

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
    QTableView* table = workspace.findChild<QTableView*>(QStringLiteral("contentTable"));
    REQUIRE(table != nullptr);
    CHECK(table->model()->rowCount() == 1);
    const QString path = table->model()->index(0, static_cast<int>(ContentColumn::Path)).data().toString();
    (void)Wav(exported, 700);
    REQUIRE(workspace.Import(file, QStringLiteral("source/hit")));
    Wait(workspace);
    CHECK(workspace.SelectedId() == QStringLiteral("source/hit"));
    CHECK(table->model()->index(0, static_cast<int>(ContentColumn::Path)).data().toString() != path);
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
        rows.append(MakeRow(QStringLiteral("source/%1").arg(row, 6, 10, QLatin1Char('0')),
                            QStringLiteral("sources/%1.wav").arg(row),
                            ContentType::AudioSource));
    }
    ContentCatalogModel model;
    ContentFilterProxyModel proxy;
    proxy.SetCatalogModel(&model);
    QElapsedTimer timer;
    timer.start();
    model.SetRows(std::move(rows));
    const auto populate = timer.nsecsElapsed();
    QVector<int64> samples;
    for (int query = 0; query < 20; ++query)
    {
        timer.restart();
        proxy.SetSearchText(QStringLiteral("%1").arg(query, 3, 10, QLatin1Char('0')));
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
    CHECK(model.columnCount() == CONTENT_COLUMN_COUNT);
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
    QTableView* table = workspace.findChild<QTableView*>(QStringLiteral("contentTable"));
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

TEST_CASE("Content keeps a selected group by identity through search sorting and refresh",
          "[editor][content][selection]")
{
    QTemporaryDir root;
    QTemporaryDir exported;
    const QString file = Wav(exported, 500);
    for (const QString& id : {QStringLiteral("source/first"), QStringLiteral("source/second")})
    {
        ContentImportGate gate;
        REQUIRE(ImportAudioSource({ .Root = root.path(), .File = file, .Id = id }, gate).Status == content::Status::Ok);
    }
    ContentWorkspace workspace;
    workspace.SetProject(root.path(), 1);
    Wait(workspace);
    QTableView* table = workspace.findChild<QTableView*>(QStringLiteral("contentTable"));
    QLineEdit* search = workspace.findChild<QLineEdit*>(QStringLiteral("contentSearch"));
    REQUIRE(table != nullptr);
    REQUIRE(search != nullptr);
    table->setCurrentIndex(table->model()->index(0, 0));
    table->selectAll();
    CHECK(table->selectionModel()->selectedRows().size() == 2);
    search->setText(QStringLiteral("first"));
    CHECK(table->selectionModel()->selectedRows().size() == 1);
    search->clear();
    CHECK(table->selectionModel()->selectedRows().size() == 2);
    table->sortByColumn(0, Qt::DescendingOrder);
    workspace.Refresh();
    Wait(workspace);
    CHECK(table->selectionModel()->selectedRows().size() == 2);
    CHECK(workspace.SelectedId() == QStringLiteral("source/first"));
}

namespace
{
/// Texel colours of the synthetic cooked textures, RGBA8.
constexpr uint32 OPAQUE_RED = 0xff0000ffU;
constexpr uint32 OPAQUE_BLUE = 0xffff0000U;
constexpr uint32 CLEAR_GREEN = 0x0000ff00U;
/// Bytes per texel in a cooked texture record.
constexpr uint32 TEXEL_BYTES = 4;
/// Allowed per-channel difference after filtering.
constexpr int COLOUR_TOLERANCE = 8;

/// Write a cooked texture record: width, height and byte count (little endian), then the texels.
void WriteTexture(const QString& path, uint32 width, uint32 height, const QVector<uint32>& texels)
{
    QByteArray bytes;
    for (const uint32 field : {width, height, width * height * TEXEL_BYTES})
    {
        const uint32 little = qToLittleEndian(field);
        bytes.append(reinterpret_cast<const char*>(&little), sizeof(little));
    }
    for (const uint32 texel : texels)
    {
        const uint32 little = qToLittleEndian(texel);
        bytes.append(reinterpret_cast<const char*>(&little), sizeof(little));
    }
    Write(path, bytes);
}
QJsonObject MakeRecord(const QString& kind, const QString& key, const QStringList& dependencies = {})
{
    QJsonObject record;
    record.insert(QStringLiteral("kind"), kind);
    record.insert(QStringLiteral("key"), key);
    record.insert(QStringLiteral("path"), QStringLiteral("assets/objects/%1.bin").arg(key));
    record.insert(QStringLiteral("dependencies"), QJsonArray::fromStringList(dependencies));
    return record;
}
/// A cooked scene project with two textures, two materials and one asset of each other kind. textureKey names the
/// checker texture's artifact, so a test can "recook" it.
void WriteSceneProject(const QString& root, const QString& textureKey = QStringLiteral("checker1"))
{
    REQUIRE(QDir(root).mkpath(QStringLiteral("assets/objects")));
    const auto object = [&root](const QString& key) {
        return QDir(root).filePath(QStringLiteral("assets/objects/%1.bin").arg(key));
    };
    WriteTexture(object(textureKey), 2, 2, {OPAQUE_RED, OPAQUE_BLUE, OPAQUE_BLUE, OPAQUE_RED});
    WriteTexture(object(QStringLiteral("glass1")), 1, 1, {CLEAR_GREEN});
    Write(object(QStringLiteral("brick1")),
          QByteArrayLiteral(R"({"texture":"texture/checker","metallic":0,"roughness":0.8,"emission":[0,0,0],)"
                            R"("source_material":{"name":"Brick wall"}})"));
    Write(object(QStringLiteral("gold1")), QByteArrayLiteral(R"({"metallic":1,"roughness":0.3,"emission":[0,0,0]})"));
    for (const QString& key : {QStringLiteral("quad1"), QStringLiteral("main1"), QStringLiteral("cc01")})
    {
        Write(object(key), QByteArrayLiteral("opaque"));
    }
    QJsonObject resources;
    resources.insert(QStringLiteral("texture/checker"), MakeRecord(QStringLiteral("texture"), textureKey));
    resources.insert(QStringLiteral("texture/glass"), MakeRecord(QStringLiteral("texture"), QStringLiteral("glass1")));
    resources.insert(
        QStringLiteral("material/brick"),
        MakeRecord(QStringLiteral("material"), QStringLiteral("brick1"), {QStringLiteral("texture/checker")}));
    resources.insert(QStringLiteral("material/gold"), MakeRecord(QStringLiteral("material"), QStringLiteral("gold1")));
    resources.insert(QStringLiteral("mesh/quad"), MakeRecord(QStringLiteral("mesh"), QStringLiteral("quad1")));
    resources.insert(QStringLiteral("scene/main"), MakeRecord(QStringLiteral("scene"), QStringLiteral("main1")));
    resources.insert(QStringLiteral("license/cc0"), MakeRecord(QStringLiteral("license"), QStringLiteral("cc01")));
    QJsonObject catalog;
    catalog.insert(QStringLiteral("resources"), resources);
    catalog.insert(QStringLiteral("version"), 1);
    Write(QDir(root).filePath(QStringLiteral("assets/catalog.json")), QJsonDocument(catalog).toJson());
}
bool IsNear(QRgb actual, QRgb expected)
{
    return std::abs(qRed(actual) - qRed(expected)) <= COLOUR_TOLERANCE &&
           std::abs(qGreen(actual) - qGreen(expected)) <= COLOUR_TOLERANCE &&
           std::abs(qBlue(actual) - qBlue(expected)) <= COLOUR_TOLERANCE;
}
void WaitForThumbnails(ContentThumbnailCache& inoutCache)
{
    for (uint32 attempt = 0; inoutCache.IsIdle() == false && attempt < 1000; ++attempt)
    {
        QTest::qWait(10);
    }
    REQUIRE(inoutCache.IsIdle() == true);
}
QStringList GetVisibleIds(const QTableView& table)
{
    QStringList ids;
    for (int row = 0; row < table.model()->rowCount(); ++row)
    {
        ids.append(table.model()->index(row, 0).data(Qt::UserRole).toString());
    }
    return ids;
}
} // namespace

TEST_CASE("Content search matches every term against name, type and path, and chips narrow by type",
          "[editor][content][filter]")
{
    const QVector<ContentRow> rows = {
        MakeRow(QStringLiteral("texture/brick"), QStringLiteral("assets/objects/a.bin"), ContentType::Texture),
        MakeRow(QStringLiteral("material/brick"),
                QStringLiteral("assets/objects/b.bin"),
                ContentType::Material,
                QStringLiteral("b"),
                QStringLiteral("Red brick wall")),
        MakeRow(QStringLiteral("material/gold"), QStringLiteral("assets/objects/c.bin"), ContentType::Material),
        MakeRow(QStringLiteral("mesh/wall"), QStringLiteral("assets/objects/d.bin"), ContentType::Mesh),
        MakeRow(QStringLiteral("source/hit"), QStringLiteral("sources/hit.wav"), ContentType::AudioSource),
        MakeRow(QStringLiteral("music/theme"), QStringLiteral("music.json"), ContentType::Music),
        MakeRow(QStringLiteral("license/cc0"), QStringLiteral("assets/objects/e.bin"), ContentType::License),
    };
    CHECK(SplitContentSearch(QStringLiteral("  Brick   WALL ")) ==
          QStringList{QStringLiteral("Brick"), QStringLiteral("WALL")});
    CHECK(SplitContentSearch(QStringLiteral("   ")).isEmpty() == true);
    // Every term must match somewhere: the label, the ID, the type name or the path.
    CHECK(DoesContentRowMatch(rows[1], SplitContentSearch(QStringLiteral("red wall")), 0) == true);
    CHECK(DoesContentRowMatch(rows[0], SplitContentSearch(QStringLiteral("red wall")), 0) == false);
    CHECK(DoesContentRowMatch(rows[4], SplitContentSearch(QStringLiteral(".wav")), 0) == true);
    CHECK(DoesContentRowMatch(rows[2], SplitContentSearch(QStringLiteral("MATERIAL gold")), 0) == true);
    // The British-English type name reads "Licence" while the catalog kind is "license"; both find it.
    CHECK(DoesContentRowMatch(rows[6], SplitContentSearch(QStringLiteral("licence")), 0) == true);
    CHECK(DoesContentRowMatch(rows[6], SplitContentSearch(QStringLiteral("license")), 0) == true);
    const ContentCategoryMask materials = GetContentCategoryBit(ContentCategory::Materials);
    CHECK(DoesContentRowMatch(rows[0], {}, materials) == false);
    CHECK(DoesContentRowMatch(rows[1], {}, materials) == true);

    ContentCatalogModel model;
    model.SetRows(rows);
    ContentFilterProxyModel proxy;
    proxy.SetCatalogModel(&model);
    CHECK(proxy.rowCount() == rows.size());
    proxy.SetSearchText(QStringLiteral("brick"));
    CHECK(proxy.rowCount() == 2);
    proxy.SetCategoryMask(materials);
    REQUIRE(proxy.rowCount() == 1);
    CHECK(proxy.index(0, 0).data(Qt::UserRole).toString() == QStringLiteral("material/brick"));
    // Chip counts follow the search but ignore the chips themselves, so another chip shows what it would add.
    QVector<int> counts = proxy.CountCategories();
    REQUIRE(counts.size() == CONTENT_CATEGORY_COUNT);
    CHECK(counts[static_cast<int>(ContentCategory::Textures)] == 1);
    CHECK(counts[static_cast<int>(ContentCategory::Materials)] == 1);
    CHECK(counts[static_cast<int>(ContentCategory::Meshes)] == 0);
    proxy.SetSearchText({});
    proxy.SetCategoryMask(GetContentCategoryBit(ContentCategory::Audio) |
                          GetContentCategoryBit(ContentCategory::Other));
    CHECK(proxy.rowCount() == 3);
    counts = proxy.CountCategories();
    CHECK(counts[static_cast<int>(ContentCategory::Audio)] == 2);
    CHECK(counts[static_cast<int>(ContentCategory::Materials)] == 2);
    // New rows enter the filtered view as soon as the catalog lists them.
    QVector<ContentRow> grown = rows;
    grown.append(MakeRow(QStringLiteral("sound/step"), QStringLiteral("step.json"), ContentType::Sound));
    model.SetRows(grown);
    CHECK(proxy.rowCount() == 4);
}

TEST_CASE("Cooked texture and material thumbnails are drawn from artifact bytes", "[editor][content][thumbnail]")
{
    QTemporaryDir root;
    WriteSceneProject(root.path());
    const QDir directory(root.path());
    constexpr int SIDE = 8;
    QImage image;
    QString failure;
    REQUIRE(TryMakeTextureThumbnail(directory.filePath(QStringLiteral("assets/objects/checker1.bin")),
                                    SIDE,
                                    image,
                                    failure) == true);
    REQUIRE(image.size() == QSize(SIDE, SIDE));
    // A 2 by 2 checker scales to quadrants; texel (0, 0) is red and (1, 0) blue.
    CHECK(IsNear(image.pixel(1, 1), qRgb(255, 0, 0)) == true);
    CHECK(IsNear(image.pixel(SIDE - 2, 1), qRgb(0, 0, 255)) == true);
    CHECK(IsNear(image.pixel(SIDE - 2, SIDE - 2), qRgb(255, 0, 0)) == true);
    // A fully clear texel shows the checkerboard behind it rather than black.
    REQUIRE(TryMakeTextureThumbnail(directory.filePath(QStringLiteral("assets/objects/glass1.bin")),
                                    SIDE,
                                    image,
                                    failure) == true);
    CHECK(qAlpha(image.pixel(0, 0)) == 255);
    CHECK(image.pixel(0, 0) != image.pixel(SIDE - 1, 0));

    const QString truncated = directory.filePath(QStringLiteral("truncated.bin"));
    Write(truncated, QByteArrayLiteral("\x02\x00\x00\x00\x02\x00\x00\x00\x10\x00\x00\x00\xff"));
    CHECK(TryMakeTextureThumbnail(truncated, SIDE, image, failure) == false);
    CHECK(failure.isEmpty() == false);
    const QString huge = directory.filePath(QStringLiteral("huge.bin"));
    WriteTexture(huge, 1U << 20U, 1, {});
    CHECK(TryMakeTextureThumbnail(huge, SIDE, image, failure) == false);
    CHECK(TryMakeTextureThumbnail(directory.filePath(QStringLiteral("missing.bin")), SIDE, image, failure) == false);

    // Materials draw a lit sphere: opaque in the middle, transparent in the corners, textured from the checker.
    constexpr int MATERIAL_SIDE = 32;
    const QHash<QString, QString> textures = {
        {QStringLiteral("texture/checker"), directory.filePath(QStringLiteral("assets/objects/checker1.bin"))}};
    REQUIRE(TryMakeMaterialThumbnail(directory.filePath(QStringLiteral("assets/objects/brick1.bin")),
                                     textures,
                                     MATERIAL_SIDE,
                                     image,
                                     failure) == true);
    REQUIRE(image.size() == QSize(MATERIAL_SIDE, MATERIAL_SIDE));
    CHECK(qAlpha(image.pixel(0, 0)) == 0);
    CHECK(qAlpha(image.pixel(MATERIAL_SIDE / 2, MATERIAL_SIDE / 2)) == 255);
    const QRgb centre = image.pixel(MATERIAL_SIDE / 2, MATERIAL_SIDE / 2);
    CHECK((qRed(centre) > qGreen(centre) || qBlue(centre) > qGreen(centre)) == true);
    // A missing texture falls back to the untextured albedo instead of failing the thumbnail.
    REQUIRE(TryMakeMaterialThumbnail(directory.filePath(QStringLiteral("assets/objects/brick1.bin")),
                                     {},
                                     MATERIAL_SIDE,
                                     image,
                                     failure) == true);
    const QString broken = directory.filePath(QStringLiteral("broken.bin"));
    Write(broken, QByteArrayLiteral("{not json"));
    CHECK(TryMakeMaterialThumbnail(broken, {}, MATERIAL_SIDE, image, failure) == false);
    CHECK(failure.isEmpty() == false);
}

TEST_CASE("The thumbnail cache generates off the UI thread, keys by artifact and records failures",
          "[editor][content][thumbnail]")
{
    QTemporaryDir root;
    WriteSceneProject(root.path());
    const QDir directory(root.path());
    ContentThumbnailCache cache;
    QStringList ready;
    QObject::connect(&cache, &ContentThumbnailCache::ThumbnailReady, &cache, [&ready](const QString& id) {
        ready.append(id);
    });
    ContentThumbnailRequest request;
    request.Id = QStringLiteral("texture/checker");
    request.Key = QStringLiteral("checker1");
    request.Type = ContentType::Texture;
    request.Path = directory.filePath(QStringLiteral("assets/objects/checker1.bin"));
    cache.Request(request);
    // Request returns before anything is drawn; the result arrives through the event loop.
    CHECK(cache.Find(request.Id, request.Key).isNull() == true);
    CHECK(cache.IsIdle() == false);
    WaitForThumbnails(cache);
    CHECK(ready == QStringList{request.Id});
    CHECK(cache.Find(request.Id, request.Key).isNull() == false);
    CHECK(cache.GetGeneratedCount() == 1);
    // Asking again for the same artifact is free.
    cache.Request(request);
    cache.Request(request);
    WaitForThumbnails(cache);
    CHECK(cache.GetGeneratedCount() == 1);
    // A recook changes the key: the stale thumbnail is not returned and the new artifact is drawn.
    WriteTexture(directory.filePath(QStringLiteral("assets/objects/checker2.bin")), 1, 1, {OPAQUE_BLUE});
    CHECK(cache.Find(request.Id, QStringLiteral("checker2")).isNull() == true);
    request.Key = QStringLiteral("checker2");
    request.Path = directory.filePath(QStringLiteral("assets/objects/checker2.bin"));
    cache.Request(request);
    WaitForThumbnails(cache);
    CHECK(cache.GetGeneratedCount() == 2);
    const QImage recooked = cache.Find(request.Id, request.Key).toImage();
    REQUIRE(recooked.isNull() == false);
    CHECK(IsNear(recooked.pixel(recooked.width() / 2, recooked.height() / 2), qRgb(0, 0, 255)) == true);
    cache.Invalidate(request.Id);
    CHECK(cache.Find(request.Id, request.Key).isNull() == true);
    cache.Request(request);
    WaitForThumbnails(cache);
    CHECK(cache.GetGeneratedCount() == 3);
    // A failure is remembered for that key, so a broken artifact is not decoded on every paint.
    ContentThumbnailRequest broken = request;
    broken.Id = QStringLiteral("texture/broken");
    broken.Key = QStringLiteral("broken1");
    broken.Path = directory.filePath(QStringLiteral("assets/objects/missing.bin"));
    cache.Request(broken);
    WaitForThumbnails(cache);
    CHECK(cache.HasFailed(broken.Id, broken.Key) == true);
    CHECK(cache.Find(broken.Id, broken.Key).isNull() == true);
    cache.Request(broken);
    WaitForThumbnails(cache);
    CHECK(cache.GetGeneratedCount() == 4);
    // The queue is bounded; requests beyond the limit drop the oldest waiting ones.
    for (int index = 0; index < CONTENT_THUMBNAIL_QUEUE_LIMIT * 2; ++index)
    {
        ContentThumbnailRequest flood = request;
        flood.Id = QStringLiteral("texture/flood%1").arg(index);
        cache.Request(flood);
    }
    CHECK(cache.GetQueuedCount() <= CONTENT_THUMBNAIL_QUEUE_LIMIT);
    cache.Clear();
    CHECK(cache.GetQueuedCount() == 0);
    WaitForThumbnails(cache);
    CHECK(cache.GetCachedCount() == 0);
}

TEST_CASE("A long content list asks only for the thumbnails it paints", "[editor][content][thumbnail][scale]")
{
    QTemporaryDir root;
    WriteSceneProject(root.path());
    constexpr int ROW_COUNT = 5000;
    QVector<ContentRow> rows;
    rows.reserve(ROW_COUNT);
    for (int index = 0; index < ROW_COUNT; ++index)
    {
        rows.append(MakeRow(QStringLiteral("texture/%1").arg(index, 5, 10, QLatin1Char('0')),
                            QStringLiteral("assets/objects/checker1.bin"),
                            ContentType::Texture,
                            QStringLiteral("key%1").arg(index)));
    }
    ContentThumbnailCache cache;
    ContentCatalogModel model;
    model.SetRoot(root.path());
    model.SetThumbnailCache(&cache);
    ContentFilterProxyModel proxy;
    proxy.SetCatalogModel(&model);
    QElapsedTimer timer;
    timer.start();
    model.SetRows(rows);
    QTableView table;
    table.setModel(&proxy);
    table.verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table.verticalHeader()->setDefaultSectionSize(CONTENT_THUMBNAIL_SIZE);
    table.resize(400, 400);
    table.show();
    QTest::qWait(50);
    const qint64 firstPaint = timer.elapsed();
    WaitForThumbnails(cache);
    const usize visibleGenerated = cache.GetGeneratedCount();
    // About 400 / 32 rows fit; nothing like 5000 thumbnails is drawn.
    CHECK(visibleGenerated > 0);
    CHECK(visibleGenerated < 64);
    table.scrollToBottom();
    QTest::qWait(50);
    WaitForThumbnails(cache);
    CHECK(cache.GetGeneratedCount() < visibleGenerated * 3);
    CHECK(firstPaint < 2000);
}

TEST_CASE("The content browser lists cooked scene assets with thumbnails, chips and live search",
          "[editor][content][widget]")
{
    QTemporaryDir root;
    WriteSceneProject(root.path());
    ContentWorkspace workspace;
    workspace.SetProject(QDir(root.path()).filePath(QStringLiteral("content")), 1, root.path());
    Wait(workspace);
    QTableView* table = workspace.findChild<QTableView*>(QStringLiteral("contentTable"));
    QLineEdit* search = workspace.findChild<QLineEdit*>(QStringLiteral("contentSearch"));
    QToolButton* textures = workspace.findChild<QToolButton*>(QStringLiteral("contentFilterTextures"));
    QToolButton* materials = workspace.findChild<QToolButton*>(QStringLiteral("contentFilterMaterials"));
    QToolButton* audio = workspace.findChild<QToolButton*>(QStringLiteral("contentFilterAudio"));
    QLabel* noMatches = workspace.findChild<QLabel*>(QStringLiteral("contentNoMatches"));
    REQUIRE(table != nullptr);
    REQUIRE(search != nullptr);
    REQUIRE(textures != nullptr);
    REQUIRE(materials != nullptr);
    REQUIRE(audio != nullptr);
    REQUIRE(noMatches != nullptr);
    CHECK(table->model()->rowCount() == 7);
    CHECK(textures->text() == QStringLiteral("Textures 2"));
    CHECK(materials->text() == QStringLiteral("Materials 2"));
    CHECK(audio->isEnabled() == false);
    // The material row shows its source name and its type.
    const QStringList ids = GetVisibleIds(*table);
    const int brick = static_cast<int>(ids.indexOf(QStringLiteral("material/brick")));
    REQUIRE(brick >= 0);
    CHECK(table->model()->index(brick, 0).data().toString() == QStringLiteral("Brick wall"));
    CHECK(table->model()->index(brick, static_cast<int>(ContentColumn::Type)).data().toString() ==
          QStringLiteral("Material"));

    workspace.resize(800, 600);
    workspace.show();
    QTest::qWait(50);
    WaitForThumbnails(workspace.GetThumbnails());
    QTest::qWait(10);
    CHECK(workspace.GetThumbnails().Find(QStringLiteral("texture/checker"), QStringLiteral("checker1")).isNull() ==
          false);
    CHECK(workspace.GetThumbnails().Find(QStringLiteral("material/brick"), QStringLiteral("brick1")).isNull() == false);
    // Meshes have no cheap thumbnail and are never decoded.
    CHECK(workspace.GetThumbnails().HasFailed(QStringLiteral("mesh/quad"), QStringLiteral("quad1")) == false);

    textures->click();
    CHECK(table->model()->rowCount() == 2);
    materials->click();
    CHECK(table->model()->rowCount() == 4);
    search->setText(QStringLiteral("brick"));
    REQUIRE(table->model()->rowCount() == 1);
    CHECK(table->model()->index(0, 0).data(Qt::UserRole).toString() == QStringLiteral("material/brick"));
    search->setText(QStringLiteral("nothing like this"));
    CHECK(table->model()->rowCount() == 0);
    CHECK(noMatches->isVisibleTo(&workspace) == true);
    search->clear();
    textures->click();
    materials->click();
    CHECK(table->model()->rowCount() == 7);
    CHECK(noMatches->isVisibleTo(&workspace) == false);

    // Recooking the texture publishes a new key; the browser refreshes and the old thumbnail is not shown.
    WriteSceneProject(root.path(), QStringLiteral("checker2"));
    workspace.Refresh();
    Wait(workspace);
    QTest::qWait(50);
    WaitForThumbnails(workspace.GetThumbnails());
    CHECK(workspace.GetThumbnails().Find(QStringLiteral("texture/checker"), QStringLiteral("checker2")).isNull() ==
          false);
    workspace.Shutdown();
    CHECK(workspace.Finished());
}

TEST_CASE("Import progress advances through every stage and finishes at one", "[editor][content][progress]")
{
    double previous = -1.0;
    for (int stage = 0; stage < static_cast<int>(CONTENT_IMPORT_STAGE_COUNT); ++stage)
    {
        for (uint32 completed = 0; completed <= 2; ++completed)
        {
            const double fraction = GetContentImportFraction(
                { .Stage = static_cast<ContentImportStage>(stage), .CompletedSteps = completed, .TotalSteps = 2 });
            CHECK(fraction >= previous);
            CHECK(fraction >= 0.0);
            CHECK(fraction <= 1.0);
            previous = fraction;
        }
    }
    CHECK(previous == 1.0);
    // Overreported steps never pass the stage's end.
    CHECK(GetContentImportFraction({ .Stage = ContentImportStage::Reading, .CompletedSteps = 9, .TotalSteps = 2 }) <
          GetContentImportFraction({ .Stage = ContentImportStage::Validating, .CompletedSteps = 0, .TotalSteps = 1 }) +
              1e-9);

    QTemporaryDir root;
    QTemporaryDir exported;
    ContentImportGate gate;
    CHECK(gate.GetProgress().Stage == ContentImportStage::Waiting);
    REQUIRE(
        ImportAudioSource({ .Root = root.path(), .File = Wav(exported, 500), .Id = QStringLiteral("source/hit") }, gate)
            .Status == content::Status::Ok);
    CHECK(gate.GetProgress().Stage == ContentImportStage::Publishing);
    CHECK(GetContentImportFraction(gate.GetProgress()) == 1.0);
}

TEST_CASE("Queued imports show overall progress, keep failures readable and retry them",
          "[editor][content][widget][progress]")
{
    QTemporaryDir root;
    QTemporaryDir exported;
    ContentWorkspace workspace;
    workspace.SetProject(root.path(), 3);
    Wait(workspace);
    const QString good = Wav(exported, 500);
    const QString bad = exported.filePath(QStringLiteral("broken.wav"));
    Write(bad, QByteArrayLiteral("RIFF this is not audio"));
    QWidget* progress = workspace.findChild<QWidget*>(QStringLiteral("contentProgress"));
    QLabel* progressLabel = workspace.findChild<QLabel*>(QStringLiteral("contentProgressLabel"));
    QProgressBar* progressBar = workspace.findChild<QProgressBar*>(QStringLiteral("contentProgressBar"));
    QPushButton* retry = workspace.findChild<QPushButton*>(QStringLiteral("contentRetry"));
    QTableView* table = workspace.findChild<QTableView*>(QStringLiteral("contentTable"));
    REQUIRE(progress != nullptr);
    REQUIRE(progressLabel != nullptr);
    REQUIRE(progressBar != nullptr);
    REQUIRE(retry != nullptr);
    REQUIRE(table != nullptr);

    REQUIRE(workspace.QueueImport(good, QStringLiteral("source/good")) == true);
    REQUIRE(workspace.QueueImport(bad, QStringLiteral("source/bad")) == true);
    // The same ID cannot wait twice.
    CHECK(workspace.QueueImport(bad, QStringLiteral("source/bad")) == false);
    CHECK(workspace.Importing() == true);
    CHECK(progress->isVisibleTo(&workspace) == true);
    CHECK(progressLabel->text().contains(QStringLiteral("of 2")) == true);
    // Both rows appear at once, the waiting one marked as queued.
    CHECK(table->model()->rowCount() == 2);
    Wait(workspace);
    CHECK(progress->isVisibleTo(&workspace) == false);

    const QStringList ids = GetVisibleIds(*table);
    const int badRow = static_cast<int>(ids.indexOf(QStringLiteral("source/bad")));
    const int goodRow = static_cast<int>(ids.indexOf(QStringLiteral("source/good")));
    REQUIRE(badRow >= 0);
    REQUIRE(goodRow >= 0);
    const QString status = table->model()->index(badRow, static_cast<int>(ContentColumn::Status)).data().toString();
    CHECK(status.startsWith(QStringLiteral("Failed")) == true);
    CHECK(status.size() > QStringLiteral("Failed: ").size());
    CHECK(table->model()->index(badRow, 0).data(CONTENT_FAILED_ROLE).toBool() == true);
    CHECK(table->model()->index(goodRow, 0).data(CONTENT_FAILED_ROLE).toBool() == false);
    StateView* problem = workspace.findChild<StateView*>(QStringLiteral("contentProblem"));
    REQUIRE(problem != nullptr);
    CHECK(problem->isVisibleTo(&workspace) == true);
    CHECK(problem->GetContent().Message.contains(QStringLiteral("source/bad")) == true);
    CHECK(problem->GetContent().Cause.isEmpty() == false);
    CHECK(problem->GetContent().ActionLabel == QStringLiteral("Retry import"));

    // Fix the file and retry from the failed row: the same file and ID import again.
    table->setCurrentIndex(table->model()->index(badRow, 0));
    REQUIRE(workspace.SelectedId() == QStringLiteral("source/bad"));
    CHECK(retry->isEnabled() == true);
    const Array<uint8> fixed = audio::test::MakeSineWav(44100, 1, 300);
    Write(bad, QByteArray(reinterpret_cast<const char*>(fixed.data()), static_cast<qsizetype>(fixed.size())));
    retry->click();
    CHECK(workspace.Importing() == true);
    Wait(workspace);
    CHECK(problem->isVisibleTo(&workspace) == false);
    CHECK(table->model()->rowCount() == 2);
    for (int row = 0; row < table->model()->rowCount(); ++row)
    {
        CHECK(table->model()->index(row, 0).data(CONTENT_FAILED_ROLE).toBool() == false);
    }
    CHECK(workspace.RetryImport(QStringLiteral("source/bad")) == false);
    workspace.Shutdown();
    CHECK(workspace.Finished());
}
