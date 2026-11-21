#include "internal/preview_shading.h"
#include "internal/render_scene.h"
#include "internal/scene_archive.h"
#include "internal/software_viewport.h"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/math/projection.hpp>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <array>
#include <utility>

#include <catch2/catch_test_macros.hpp>

using namespace ludus;
using namespace ludus::editor;
using foundation::float32;
using foundation::uint16;
using foundation::uint32;
using foundation::uint8;
namespace
{
constexpr uint16 METHOD_STORED = 0;
constexpr uint16 METHOD_DEFLATED = 8;
constexpr uint32 LOCAL_SIGNATURE = 0x04034b50U;
constexpr uint32 DIRECTORY_SIGNATURE = 0x02014b50U;
constexpr uint32 END_SIGNATURE = 0x06054b50U;
constexpr foundation::uint64 UNLIMITED_BYTES = 1ULL << 40U;

struct ArchiveEntry final
{
    QByteArray Name;
    QByteArray Data;
    uint16 Method = METHOD_STORED;
    uint32 ExpandedSize = 0;
};

template <typename ValueType>
void append(QByteArray& outBytes, ValueType value)
{
    std::array<uint8, sizeof(ValueType)> bytes{};
    REQUIRE(foundation::TryWriteLittleEndian(value, bytes) == true);
    outBytes.append(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
}

QByteArray buildArchive(const QVector<ArchiveEntry>& entries)
{
    QByteArray archive;
    QByteArray directory;
    for (const ArchiveEntry& entry : entries)
    {
        const uint32 offset = static_cast<uint32>(archive.size());
        append<uint32>(archive, LOCAL_SIGNATURE);
        append<uint16>(archive, 20);
        append<uint16>(archive, 0);
        append<uint16>(archive, entry.Method);
        append<uint32>(archive, 0);
        append<uint32>(archive, 0);
        append<uint32>(archive, static_cast<uint32>(entry.Data.size()));
        append<uint32>(archive, entry.ExpandedSize);
        append<uint16>(archive, static_cast<uint16>(entry.Name.size()));
        append<uint16>(archive, 0);
        archive.append(entry.Name);
        archive.append(entry.Data);

        append<uint32>(directory, DIRECTORY_SIGNATURE);
        append<uint16>(directory, 20);
        append<uint16>(directory, 20);
        append<uint16>(directory, 0);
        append<uint16>(directory, entry.Method);
        append<uint32>(directory, 0);
        append<uint32>(directory, 0);
        append<uint32>(directory, static_cast<uint32>(entry.Data.size()));
        append<uint32>(directory, entry.ExpandedSize);
        append<uint16>(directory, static_cast<uint16>(entry.Name.size()));
        append<uint16>(directory, 0);
        append<uint16>(directory, 0);
        append<uint16>(directory, 0);
        append<uint16>(directory, 0);
        append<uint32>(directory, 0);
        append<uint32>(directory, offset);
        directory.append(entry.Name);
    }
    const uint32 directoryOffset = static_cast<uint32>(archive.size());
    archive.append(directory);
    append<uint32>(archive, END_SIGNATURE);
    append<uint16>(archive, 0);
    append<uint16>(archive, 0);
    append<uint16>(archive, static_cast<uint16>(entries.size()));
    append<uint16>(archive, static_cast<uint16>(entries.size()));
    append<uint32>(archive, static_cast<uint32>(directory.size()));
    append<uint32>(archive, directoryOffset);
    append<uint16>(archive, 0);
    return archive;
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}
} // namespace

TEST_CASE("Browser scene archives expand stored and deflated entries", "[editor][browser]")
{
    const QByteArray scene = "WorldBegin\n";
    // Raw deflate of "hello hello hello hello".
    const QByteArray deflated = QByteArray::fromHex("cb48cdc9c957c8402701");
    const QByteArray archive = buildArchive({
        {QByteArrayLiteral("scenes/"), {}, METHOD_STORED, 0},
        {QByteArrayLiteral("scenes/main.pbrt"), scene, METHOD_STORED, static_cast<uint32>(scene.size())},
        {QByteArrayLiteral("scenes/textures/note.txt"), deflated, METHOD_DEFLATED, 23},
    });
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    QString error;
    foundation::uint64 expanded = 0;
    REQUIRE(ExpandSceneArchive(archive, directory.path(), UNLIMITED_BYTES, expanded, error) == true);
    CHECK(readFile(directory.filePath(QStringLiteral("scenes/main.pbrt"))) == scene);
    CHECK(readFile(directory.filePath(QStringLiteral("scenes/textures/note.txt"))) ==
          QByteArrayLiteral("hello hello hello hello"));
    CHECK(expanded == static_cast<foundation::uint64>(scene.size()) + 23U);
}

TEST_CASE("Browser scene archives reject escaping and damaged entries", "[editor][browser]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    QString error;
    foundation::uint64 expanded = 0;
    const QByteArray escaping =
        buildArchive({{QByteArrayLiteral("../outside.pbrt"), QByteArrayLiteral("x"), METHOD_STORED, 1}});
    CHECK(ExpandSceneArchive(escaping, directory.path(), UNLIMITED_BYTES, expanded, error) == false);
    CHECK(error.contains(QStringLiteral("unsafe")) == true);
    CHECK(QFile::exists(QDir(directory.path()).absoluteFilePath(QStringLiteral("../outside.pbrt"))) == false);
    CHECK(ExpandSceneArchive(QByteArrayLiteral("not a zip"), directory.path(), UNLIMITED_BYTES, expanded, error) ==
          false);
    QByteArray truncated =
        buildArchive({{QByteArrayLiteral("a.pbrt"), QByteArrayLiteral("abcdef"), METHOD_DEFLATED, 6}});
    CHECK(ExpandSceneArchive(truncated, directory.path(), UNLIMITED_BYTES, expanded, error) == false);
}

TEST_CASE("Browser scene archives charge declared expanded bytes before writing", "[editor][browser]")
{
    // A tiny deflated entry that declares more expanded bytes than the budget allows.
    const QByteArray deflated = QByteArray::fromHex("cb48cdc9c957c8402701");
    const QByteArray archive = buildArchive({
        {QByteArrayLiteral("first.pbrt"), QByteArrayLiteral("abc"), METHOD_STORED, 3},
        {QByteArrayLiteral("bomb.bin"), deflated, METHOD_DEFLATED, 23},
    });
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    QString error;
    foundation::uint64 expanded = 0;
    CHECK(ExpandSceneArchive(archive, directory.path(), 20, expanded, error) == false);
    CHECK(error.contains(QStringLiteral("limit")) == true);
    CHECK(QFile::exists(directory.filePath(QStringLiteral("first.pbrt"))) == false);
    CHECK(ExpandSceneArchive(archive, directory.path(), 26, expanded, error) == true);
    CHECK(expanded == 26U);
}

TEST_CASE("Software viewport rasterizes a visible triangle with depth", "[editor][browser]")
{
    RenderScene scene;
    PreviewMesh mesh;
    mesh.Positions = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
    mesh.Normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    mesh.Indices = {0, 1, 2};
    scene.Meshes.push_back(mesh);
    PreviewObject object;
    object.Tint = {1, 0, 0, 1};
    scene.Objects.push_back(object);
    scene.Radius = 1;
    PreviewViewOptions options;
    options.OrbitDegrees = {0, 0};
    options.Distance = 3;
    options.Width = 64;
    options.Height = 48;
    graphics::renderer::ViewDescription view;
    QString error;
    REQUIRE(scene.View(options, view, error) == true);
    const QImage image = RenderSoftwareView(scene, view, options.Width, options.Height, PreviewShading::Lit);
    REQUIRE(image.isNull() == false);
    CHECK(image.width() == 64);
    CHECK(image.height() == 48);
    const QColor centre = image.pixelColor(32, 24);
    CHECK(centre.red() > centre.green());
    CHECK(centre.red() > centre.blue());
    const QColor corner = image.pixelColor(0, 0);
    CHECK(corner.red() < 30);
    CHECK(RenderSoftwareView(scene, view, 0, 0, PreviewShading::Lit).isNull() == true);
}

namespace
{
PreviewMesh makeTriangle(float32 depth)
{
    PreviewMesh mesh;
    mesh.Positions = {{-1, -1, depth}, {1, -1, depth}, {0, 1, depth}};
    mesh.Indices = {0, 1, 2};
    return mesh;
}
PreviewObject makeObject(foundation::int32 mesh, foundation::math::Vector4 tint)
{
    PreviewObject object;
    object.Mesh = mesh;
    object.Tint = tint;
    return object;
}
} // namespace

TEST_CASE("Software viewport orders orthographic depth and clips outside the depth range", "[editor][browser]")
{
    RenderScene scene;
    scene.Meshes = {makeTriangle(-5), makeTriangle(-2), makeTriangle(1)};
    // Far red first, then near green, then blue behind the camera (outside near).
    scene.Objects = {makeObject(0, {1, 0, 0, 1}), makeObject(1, {0, 1, 0, 1}), makeObject(2, {0, 0, 1, 1})};
    graphics::renderer::ViewDescription view;
    constexpr float32 NEAR_PLANE = .1F;
    constexpr float32 FAR_PLANE = 10.0F;
    REQUIRE(foundation::math::TryOrthographicReverseZ(-2, 2, -2, 2, NEAR_PLANE, FAR_PLANE, view.WorldToClip) ==
            foundation::math::MathStatus::Success);
    constexpr uint32 SIDE = 32;
    QImage image = RenderSoftwareView(scene, view, SIDE, SIDE, PreviewShading::Albedo);
    QColor centre = image.pixelColor(SIDE / 2, SIDE / 2);
    CHECK(centre.green() > 200);
    CHECK(centre.red() < 30);
    CHECK(centre.blue() < 30);
    // Reversing the draw order must give the same nearest surface.
    std::swap(scene.Objects[0], scene.Objects[1]);
    image = RenderSoftwareView(scene, view, SIDE, SIDE, PreviewShading::Albedo);
    centre = image.pixelColor(SIDE / 2, SIDE / 2);
    CHECK(centre.green() > 200);
    CHECK(centre.red() < 30);
}

TEST_CASE("Software viewport Solid mode ignores tint", "[editor][browser]")
{
    RenderScene scene;
    scene.Meshes = {makeTriangle(-2)};
    scene.Objects = {makeObject(0, {1, 0, 0, 1})};
    graphics::renderer::ViewDescription view;
    REQUIRE(foundation::math::TryOrthographicReverseZ(-2, 2, -2, 2, .1F, 10.0F, view.WorldToClip) ==
            foundation::math::MathStatus::Success);
    const QColor centre = RenderSoftwareView(scene, view, 32, 32, PreviewShading::Solid).pixelColor(16, 16);
    CHECK(centre.red() == centre.green());
    CHECK(centre.green() == centre.blue());
    CHECK(centre.red() > 0);
}
