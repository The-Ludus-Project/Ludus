#include "internal/content_browser.h"
#include "internal/state_view.h"

#include <ludus/foundation/filesystem/persistence.hpp>

#include <ludus/content/scene_format/luscn.h>

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <span>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::foundation;
using namespace ludus::editor;
namespace filesystem = ludus::foundation::filesystem;
const QString SHARED_TEXTURE = QStringLiteral("texture/3574cc53-63de-487e-a752-11a50b15622e");
const QString INDEPENDENT_TEXTURE = QStringLiteral("texture/2146c4b2-66f1-400d-9b3c-16aed83f7f23");
const QString FIRST_MATERIAL = QStringLiteral("material/d584cde6-60aa-4527-b520-0404ef054426");
const QString SECOND_MATERIAL = QStringLiteral("material/376f5f9c-b01c-47c1-8a27-ca481051d83c");
const QString INDEPENDENT_MATERIAL = QStringLiteral("material/ffecb829-9b21-4a14-b19a-5bc6db4a3954");
constexpr uint32 OPAQUE_RED = 0xff0000ffU;
constexpr uint32 OPAQUE_BLUE = 0xffff0000U;
constexpr uint32 OPAQUE_GREEN = 0xff00ff00U;
constexpr uint32 OPAQUE_YELLOW = 0xff00ffffU;

[[nodiscard]] std::string_view View(const QByteArray& bytes) noexcept
{
    return {bytes.constData(), static_cast<usize>(bytes.size())};
}

[[nodiscard]] QByteArray MakeTextureBytes(uint32 width, const QVector<uint32>& texels)
{
    QByteArray bytes;
    for (const uint32 word : {width, uint32{1}, width * ludus::content::scene_format::TEXEL_BYTES})
    {
        const uint32 little = qToLittleEndian(word);
        bytes.append(reinterpret_cast<const char*>(&little), sizeof(little));
    }
    for (const uint32 texel : texels)
    {
        const uint32 little = qToLittleEndian(texel);
        bytes.append(reinterpret_cast<const char*>(&little), sizeof(little));
    }
    return bytes;
}

void Wait(const ContentWorkspace& workspace)
{
    REQUIRE(QTest::qWaitFor([&workspace]() { return workspace.Busy() == false; }) == true);
}

void WaitForThumbnails(const ContentThumbnailCache& cache)
{
    REQUIRE(QTest::qWaitFor([&cache]() { return cache.IsIdle() == true; }) == true);
}

[[nodiscard]] std::span<const uint8> GetBytes(const QByteArray& bytes)
{
    return {reinterpret_cast<const uint8*>(bytes.constData()), static_cast<usize>(bytes.size())};
}

void Publish(const filesystem::WriteDirectory& writer, std::string_view path, const QByteArray& bytes)
{
    filesystem::WriteOptions options;
    options.Sync = filesystem::SyncPolicy::PublishOnly;
    const filesystem::PublicationResult result = writer.Publish(path, GetBytes(bytes), options);
    REQUIRE(result.Outcome.Succeeded() == true);
    REQUIRE(result.Published == true);
}

[[nodiscard]] QJsonObject PublishPreviewArtifact(const filesystem::WriteDirectory& writer,
                                                 const QString& kind,
                                                 const QByteArray& bytes,
                                                 const QJsonObject& dependencyKeys = {})
{
    const QString digest = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    const QJsonObject build{{QStringLiteral("profile"), QStringLiteral("content-reload-fixture")}};
    const QStringList dependencies = dependencyKeys.keys();
    const QJsonObject keyRecord{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("content"), digest},
        {QStringLiteral("dependencies"), QJsonArray::fromStringList(dependencies)},
        {QStringLiteral("dependency_keys"), dependencyKeys},
        {QStringLiteral("build"), build},
    };
    const QByteArray keyBytes = QJsonDocument(keyRecord).toJson(QJsonDocument::Compact) + '\n';
    const QString key = QString::fromLatin1(QCryptographicHash::hash(keyBytes, QCryptographicHash::Sha256).toHex());
    const QString path = QStringLiteral("assets/objects/%1.bin").arg(key);
    Publish(writer, View(path.toUtf8()), bytes);
    return {
        {QStringLiteral("kind"), kind},
        {QStringLiteral("key"), key},
        {QStringLiteral("path"), path},
        {QStringLiteral("sha256"), digest},
        {QStringLiteral("bytes"), bytes.size()},
        {QStringLiteral("dependencies"), QJsonArray::fromStringList(dependencies)},
        {QStringLiteral("dependency_keys"), dependencyKeys},
        {QStringLiteral("build"), build},
    };
}

[[nodiscard]] QString PublishPreviewRevision(const filesystem::WriteDirectory& writer, const QByteArray& shared)
{
    const QJsonObject sharedRecord = PublishPreviewArtifact(writer, QStringLiteral("texture"), shared);
    // Replace the independent file with identical bytes to generate a second hint. Its binding key stays constant.
    const QJsonObject independentRecord =
        PublishPreviewArtifact(writer, QStringLiteral("texture"), MakeTextureBytes(1, {OPAQUE_BLUE}));
    QJsonObject resources{
        {SHARED_TEXTURE, sharedRecord},
        {INDEPENDENT_TEXTURE, independentRecord},
    };
    const auto publishMaterial = [&](const QString& id, const QString& texture, const QJsonObject& child) {
        const QJsonObject material{
            {QStringLiteral("texture"), texture},
            {QStringLiteral("roughness"), 0.5},
            {QStringLiteral("metallic"), 0},
            {QStringLiteral("emission"), QJsonArray{0, 0, 0}},
            {QStringLiteral("address"), id == SECOND_MATERIAL ? QStringLiteral("repeat") : QStringLiteral("clamp")},
        };
        resources.insert(id,
                         PublishPreviewArtifact(writer,
                                                QStringLiteral("material"),
                                                QJsonDocument(material).toJson(QJsonDocument::Compact),
                                                {{texture, child.value(QStringLiteral("key"))}}));
    };
    publishMaterial(FIRST_MATERIAL, SHARED_TEXTURE, sharedRecord);
    publishMaterial(SECOND_MATERIAL, SHARED_TEXTURE, sharedRecord);
    publishMaterial(INDEPENDENT_MATERIAL, INDEPENDENT_TEXTURE, independentRecord);
    const QJsonObject catalog{{QStringLiteral("version"), 1}, {QStringLiteral("resources"), resources}};
    Publish(writer, "assets/catalog.json", QJsonDocument(catalog).toJson(QJsonDocument::Compact));
    return sharedRecord.value(QStringLiteral("path")).toString();
}

[[nodiscard]] const ContentCatalogModel& GetContentModel(const ContentWorkspace& workspace)
{
    const QTableView* tableOrNull = workspace.findChild<QTableView*>(QStringLiteral("contentTable"));
    REQUIRE(tableOrNull != nullptr);
    const QSortFilterProxyModel* proxyOrNull = qobject_cast<QSortFilterProxyModel*>(tableOrNull->model());
    REQUIRE(proxyOrNull != nullptr);
    const ContentCatalogModel* modelOrNull = static_cast<const ContentCatalogModel*>(proxyOrNull->sourceModel());
    REQUIRE(modelOrNull != nullptr);
    return *modelOrNull;
}

[[nodiscard]] QPixmap ReadPreview(ContentWorkspace& inoutWorkspace, const QString& id)
{
    const QModelIndex index = GetContentModel(inoutWorkspace).FindId(id);
    REQUIRE(index.isValid() == true);
    static_cast<void>(index.data(Qt::DecorationRole));
    WaitForThumbnails(inoutWorkspace.GetThumbnails());
    const QPixmap pixmap = index.data(Qt::DecorationRole).value<QPixmap>();
    REQUIRE(pixmap.isNull() == false);
    return pixmap;
}

void RequireMatchesRescan(ContentWorkspace& inoutWorkspace, const QString& audioRoot, const QString& projectRoot)
{
    ContentWorkspace oracle;
    oracle.SetProject(audioRoot, 1, projectRoot);
    Wait(oracle);
    CHECK(GetContentModel(inoutWorkspace).GetRows() == GetContentModel(oracle).GetRows());
    for (const QString& id : {FIRST_MATERIAL, SECOND_MATERIAL, INDEPENDENT_MATERIAL})
    {
        CAPTURE(id.toStdString());
        const ContentCatalogModel& model = GetContentModel(inoutWorkspace);
        const ContentRow* rowOrNull = model.Row(model.FindId(id));
        REQUIRE(rowOrNull != nullptr);
        CHECK(rowOrNull->Dependencies ==
              QStringList{id == INDEPENDENT_MATERIAL ? INDEPENDENT_TEXTURE : SHARED_TEXTURE});
        CHECK(ReadPreview(inoutWorkspace, id).toImage() == ReadPreview(oracle, id).toImage());
    }
}
} // namespace

TEST_CASE("Cooked content overflow retries validation and observation while retaining drawable materials",
          "[editor][content][cooked-reload]")
{
    CAPTURE(SHARED_TEXTURE.toStdString(), FIRST_MATERIAL.toStdString(), SECOND_MATERIAL.toStdString());
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    REQUIRE(QDir(project.path()).mkpath(QStringLiteral("assets/objects")) == true);
    filesystem::WriteDirectory writer;
    REQUIRE(writer.Open(View(project.path().toUtf8())).Succeeded() == true);
    static_cast<void>(PublishPreviewRevision(writer, MakeTextureBytes(1, {OPAQUE_RED})));

    constexpr usize WATCH_PATHS = 3;
    ContentWorkspace workspace(nullptr, { .MaxPaths = WATCH_PATHS, .HintCapacity = 1, .DebounceNanoseconds = 0 });
    const QString audioRoot = project.filePath(QStringLiteral("content"));
    workspace.SetProject(audioRoot, 1, project.path());
    QTimer* timerOrNull = workspace.findChild<QTimer*>(QStringLiteral("cookedContentWatch"));
    REQUIRE(timerOrNull != nullptr);
    REQUIRE(timerOrNull->isActive() == true);
    timerOrNull->stop();
    const auto poll = [timerOrNull]() {
        REQUIRE(QMetaObject::invokeMethod(timerOrNull, "timeout", Qt::DirectConnection) == true);
    };
    Wait(workspace);

    const QPixmap heldPreview = ReadPreview(workspace, FIRST_MATERIAL);
    const QImage originalImage = heldPreview.toImage();
    const qint64 independentIdentity = ReadPreview(workspace, INDEPENDENT_MATERIAL).cacheKey();

    const QByteArray green = MakeTextureBytes(1, {OPAQUE_GREEN});
    const QString greenPath = PublishPreviewRevision(writer, green);
    INFO("Publish a valid shared texture and lose incremental hints.");
    poll();
    CHECK(workspace.Busy() == false);
    poll();
    REQUIRE(workspace.Busy() == true);
    Wait(workspace);
    RequireMatchesRescan(workspace, audioRoot, project.path());
    const QPixmap first = ReadPreview(workspace, FIRST_MATERIAL);
    const QPixmap second = ReadPreview(workspace, SECOND_MATERIAL);
    CHECK(first.toImage() != originalImage);
    CHECK(first.toImage() == second.toImage());
    CHECK(ReadPreview(workspace, INDEPENDENT_MATERIAL).cacheKey() == independentIdentity);

    filesystem::Directory readerRoot;
    REQUIRE(readerRoot.Open(View(project.path().toUtf8())).Succeeded() == true);
    filesystem::File heldReader;
    REQUIRE(readerRoot.OpenRead(View(greenPath.toUtf8()), heldReader).Succeeded() == true);
    const auto checkRetained = [&]() {
        CHECK(ReadPreview(workspace, FIRST_MATERIAL).cacheKey() == first.cacheKey());
        CHECK(ReadPreview(workspace, SECOND_MATERIAL).cacheKey() == second.cacheKey());
        CHECK(ReadPreview(workspace, INDEPENDENT_MATERIAL).cacheKey() == independentIdentity);
        QByteArray retained(green.size(), Qt::Uninitialized);
        const filesystem::ReadResult read =
            heldReader.ReadAt(0, {reinterpret_cast<uint8*>(retained.data()), static_cast<usize>(retained.size())});
        REQUIRE(read.Outcome.Succeeded() == true);
        CHECK(retained == green);
        CHECK(heldPreview.toImage() == originalImage);
    };
    const StateView* problemOrNull = workspace.findChild<StateView*>(QStringLiteral("contentProblem"));
    REQUIRE(problemOrNull != nullptr);

    INFO("A malformed candidate passes the digest check and fails cooked texture validation.");
    const QString malformed = PublishPreviewRevision(writer, QByteArrayLiteral("invalid cooked texture"));
    poll();
    CHECK(workspace.Busy() == false);
    poll();
    REQUIRE(workspace.Busy() == true);
    Wait(workspace);
    CHECK(problemOrNull->isHidden() == false);
    CHECK(problemOrNull->GetContent().Cause.contains(malformed) == true);
    checkRetained();

    INFO("Start another overflow, then keep an unsafe leaf present for a complete rescan attempt.");
    static_cast<void>(PublishPreviewRevision(writer, MakeTextureBytes(1, {OPAQUE_YELLOW})));
    poll();
    CHECK(workspace.Busy() == false);
    REQUIRE(QFile::remove(project.filePath(greenPath)) == true);
    REQUIRE(QFile::link(project.filePath(QStringLiteral("assets")), project.filePath(greenPath)) == true);
    poll();
    CHECK(workspace.Busy() == false);
    CHECK(problemOrNull->GetContent().Cause.contains(greenPath) == true);
    CHECK(problemOrNull->GetContent().Cause.contains(QStringLiteral("recovery is pending")) == true);
    workspace.Refresh();
    CHECK(workspace.Busy() == false);
    checkRetained();

    INFO("Repair the observation, publish a newer valid revision, and complete the pending retry.");
    REQUIRE(QFile::remove(project.filePath(greenPath)) == true);
    Publish(writer, View(greenPath.toUtf8()), green);
    static_cast<void>(PublishPreviewRevision(writer, MakeTextureBytes(2, {OPAQUE_YELLOW, OPAQUE_YELLOW})));
    poll();
    REQUIRE(workspace.Busy() == true);
    Wait(workspace);
    CHECK(problemOrNull->isHidden() == true);
    RequireMatchesRescan(workspace, audioRoot, project.path());
    CHECK(ReadPreview(workspace, FIRST_MATERIAL).toImage() != first.toImage());
    CHECK(ReadPreview(workspace, SECOND_MATERIAL).toImage() != second.toImage());
    CHECK(ReadPreview(workspace, INDEPENDENT_MATERIAL).cacheKey() == independentIdentity);
    QByteArray retained(green.size(), Qt::Uninitialized);
    CHECK(heldReader.ReadAt(0, {reinterpret_cast<uint8*>(retained.data()), static_cast<usize>(retained.size())})
              .Outcome.Succeeded() == true);
    CHECK(retained == green);
    CHECK(heldReader.Close().Succeeded() == true);
    CHECK(heldReader.IsOpen() == false);
    CHECK(heldPreview.toImage() == originalImage);
}
