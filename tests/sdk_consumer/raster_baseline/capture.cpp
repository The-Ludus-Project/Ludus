// A standalone installed-SDK application for saved, translation-only Worlds.
// With no arguments it captures the cube World's Main/output export as capture.png. Otherwise:
//   raster_capture <world.json> <package.lurgrt> <export id> <png> [<export id> <png>]...
// Every input is read from the working directory, and each PNG holds the export's raw RGBA8 bytes,
// so a label export keeps the identity it stores in alpha.
#include "renderer_composite.h"

#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/rhi/lifetime.h>

#include <QCryptographicHash>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QVector>

namespace
{
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace format = ludus::content::render_graph_format;
using Status = rhi::RasterStatus;

constexpr uint32 DEVICE_WIDTH = 96;
constexpr uint32 DEVICE_HEIGHT = 64;
constexpr int64 WAIT_LIMIT_MILLISECONDS = 60000;
constexpr int32 ARGUMENTS_BEFORE_EXPORTS = 3;

struct ExportRequest final
{
    QString Id;
    QString Image;
    int32 Resource = -1;
};

QByteArray ReadFile(const QString& path)
{
    QFile file(path);
    if (file.open(QIODevice::ReadOnly) == false)
    {
        qCritical() << path << file.errorString();
        return {};
    }
    return file.readAll();
}

// Resolve an authored Export against the names the cooker wrote for these exact package bytes.
int32 FindExport(const QByteArray& bytes, const QString& namesPath, const QString& exportId)
{
    const QJsonObject names = QJsonDocument::fromJson(ReadFile(namesPath)).object();
    if (names["runtime_sha256"].toString().toLatin1() !=
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())
    {
        return -1;
    }
    const QJsonArray resources = names["resources"].toArray();
    for (const QJsonValue operation : names["operations"].toArray())
    {
        if (operation["kind"].toString() == "Export" && operation["id"].toString() == exportId)
        {
            const QString target = operation["attachments"]["texture"]["resource"].toString();
            for (qsizetype i = 0; i < resources.size(); ++i)
            {
                if (resources[i]["name"].toString() == target && resources[i]["format"].toString() == "RGBA8Unorm")
                {
                    return static_cast<int32>(i);
                }
            }
        }
    }
    return -1;
}

struct World final
{
    QVector<math::Vector3> Positions;
    QVector<uint32> Indices;
    QVector<renderer::RenderGraphItem> Items;
    renderer::ViewDescription Camera;
};

bool ReadTranslation(const QJsonObject& transform, math::Vector3& outPosition)
{
    if (transform["rx"] != 0 || transform["ry"] != 0 || transform["rz"] != 0 || transform["sx"] != 1 ||
        transform["sy"] != 1 || transform["sz"] != 1 || transform["x"].isDouble() == false ||
        transform["y"].isDouble() == false || transform["z"].isDouble() == false)
    {
        return false;
    }
    outPosition = {static_cast<float32>(transform["x"].toDouble()),
                   static_cast<float32>(transform["y"].toDouble()),
                   static_cast<float32>(transform["z"].toDouble())};
    return true;
}

bool LoadMesh(World& inoutWorld)
{
    const QJsonObject mesh = QJsonDocument::fromJson(ReadFile("authoring/cube.mesh.json")).object();
    if (mesh["version"] != 1)
    {
        return false;
    }
    for (const QJsonValue position : mesh["positions"].toArray())
    {
        const QJsonArray xyz = position.toArray();
        if (xyz.size() != 3 || xyz[0].isDouble() == false || xyz[1].isDouble() == false || xyz[2].isDouble() == false)
        {
            return false;
        }
        inoutWorld.Positions.append(math::Vector3{static_cast<float32>(xyz[0].toDouble()),
                                                  static_cast<float32>(xyz[1].toDouble()),
                                                  static_cast<float32>(xyz[2].toDouble())});
    }
    for (const QJsonValue index : mesh["indices"].toArray())
    {
        const int32 vertex = index.toInt(-1);
        if (vertex < 0 || vertex >= inoutWorld.Positions.size())
        {
            return false;
        }
        inoutWorld.Indices.append(static_cast<uint32>(vertex));
    }
    return inoutWorld.Indices.isEmpty() == false;
}

bool ReadCamera(const QJsonObject& lens,
                const math::Vector3& positionWorld,
                uint32 width,
                uint32 height,
                renderer::ViewDescription& outCamera)
{
    const float32 halfHeight = static_cast<float32>(lens["span"].toDouble()) * 0.5F;
    const float32 halfWidth = halfHeight * static_cast<float32>(width) / static_cast<float32>(height);
    math::Matrix4 viewToClip;
    if (lens["projection"].toString() != "orthographic" || lens["follow"].toString().isEmpty() == false ||
        math::TryOrthographicReverseZ(-halfWidth,
                                      halfWidth,
                                      -halfHeight,
                                      halfHeight,
                                      static_cast<float32>(lens["near"].toDouble()),
                                      static_cast<float32>(lens["far"].toDouble()),
                                      viewToClip) != math::MathStatus::Success)
    {
        return false;
    }
    math::Affine3 worldToView;
    worldToView.Translation = -positionWorld;
    outCamera.WorldToClip = viewToClip * math::ToMatrix4(worldToView);
    for (math::Vector4& column : outCamera.WorldToClip.Columns)
    {
        column.Z = column.W - column.Z;
    }
    outCamera.Depth = renderer::DepthConvention::Conventional;
    outCamera.OrthographicCamera = true;
    outCamera.CameraPosition = positionWorld;
    return true;
}

// Submit each visible renderable in World order, as the Editor's extraction does: the item index
// is the instance a visibility label export stores.
bool LoadWorld(const QString& path, uint32 width, uint32 height, World& outWorld)
{
    const QJsonArray entities = QJsonDocument::fromJson(ReadFile(path)).object()["entities"].toArray();
    if (LoadMesh(outWorld) == false)
    {
        return false;
    }
    bool hasCamera = false;
    for (const QJsonValue entity : entities)
    {
        const QJsonObject components = entity["components"].toObject();
        math::Vector3 positionWorld;
        if (ReadTranslation(components["transform"].toObject(), positionWorld) == false)
        {
            return false;
        }
        if (components.contains("renderable") == true)
        {
            const QJsonObject surface = components["renderable"].toObject();
            if (surface["mesh"].toString() != "mesh.cube" || surface["visible"].toBool() == false ||
                surface["texture"].toString().isEmpty() == false || surface["r"].isDouble() == false ||
                surface["g"].isDouble() == false || surface["b"].isDouble() == false)
            {
                return false;
            }
            renderer::RenderGraphItem item;
            item.SourceId = static_cast<uint64>(outWorld.Items.size()) + 1;
            item.MeshToWorld.Translation = positionWorld;
            item.Material.Color = {static_cast<float32>(surface["r"].toDouble()),
                                   static_cast<float32>(surface["g"].toDouble()),
                                   static_cast<float32>(surface["b"].toDouble()),
                                   1};
            outWorld.Items.append(item);
        }
        else if (components.contains("camera") == true && hasCamera == false)
        {
            hasCamera = ReadCamera(components["camera"].toObject(), positionWorld, width, height, outWorld.Camera);
            if (hasCamera == false)
            {
                return false;
            }
        }
        else
        {
            return false;
        }
    }
    return hasCamera == true && outWorld.Items.isEmpty() == false;
}

bool IsPending(Status status)
{
    return status == Status::Pending || status == Status::NotReady;
}

template <typename Operation>
bool WaitFor(rhi::DeviceHandle device, const char* name, Operation operation)
{
    QElapsedTimer timer;
    timer.start();
    Status status;
    do
    {
        const Status lifetime = rhi::PollLifetime(device);
        if (lifetime != Status::Ready && IsPending(lifetime) == false)
        {
            qCritical() << name << "device polling failed" << static_cast<uint32>(lifetime);
            return false;
        }
        status = operation();
        if (status == Status::Ready)
        {
            return true;
        }
        QThread::msleep(1);
    } while (IsPending(status) == true && timer.elapsed() < WAIT_LIMIT_MILLISECONDS);
    qCritical() << name << "failed or timed out with raster status" << static_cast<uint32>(status);
    return false;
}

// Every pass must be a static raster ColorAndDepth draw, and each request must name one's colour.
bool ResolveExports(const QByteArray& bytes,
                    const QString& packagePath,
                    const renderer::RenderGraphPlan& plan,
                    QVector<ExportRequest>& inoutRequests)
{
    for (usize pass = 0; pass < plan.Passes.GetSize(); ++pass)
    {
        if (format::GetTechniqueFamily(plan.Passes[pass].Program) != format::Technique::ColorAndDepth)
        {
            qCritical() << "Pass" << pass << "is not a static raster colour pass";
            return false;
        }
    }
    for (ExportRequest& request : inoutRequests)
    {
        request.Resource = FindExport(bytes, packagePath + ".resources.json", request.Id);
        bool hasProducer = false;
        for (usize pass = 0; request.Resource >= 0 && pass < plan.Passes.GetSize(); ++pass)
        {
            hasProducer = hasProducer == true || plan.Passes[pass].Color == static_cast<uint32>(request.Resource);
        }
        if (hasProducer == false || static_cast<usize>(request.Resource) >= plan.Resources.GetSize())
        {
            qCritical() << request.Id << "does not select an authored raster colour pass of" << packagePath;
            return false;
        }
    }
    return true;
}

bool SaveExport(rhi::DeviceHandle device,
                renderer::RenderGraph& inoutGraph,
                const ExportRequest& request,
                const format::Resource& target)
{
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect{};
    if (WaitFor(device,
                "export",
                [&]() { return inoutGraph.GetExport(static_cast<uint32>(request.Resource), texture, aspect); }) ==
            false ||
        aspect != rhi::TextureAspect::Color)
    {
        return false;
    }
    QImage image(static_cast<int32>(target.Width), static_cast<int32>(target.Height), QImage::Format_RGBA8888);
    rhi::ReadbackTicket ticket;
    const Status requested = rhi::RequestTextureReadback(device,
                                                         texture,
                                                         { .Width = target.Width, .Height = target.Height },
                                                         static_cast<usize>(image.bytesPerLine()),
                                                         ticket);
    if (requested != Status::Ready && requested != Status::Pending)
    {
        return false;
    }
    const bool hasPixels = WaitFor(device, "readback", [&]() {
        return rhi::CopyReadback(device, ticket, image.bits(), static_cast<usize>(image.sizeInBytes()));
    });
    const Status released = rhi::Release(device, ticket);
    if (hasPixels == false || released != Status::Ready || image.save(request.Image) == false)
    {
        return false;
    }
    qInfo() << "Raster" << request.Id << "resource" << request.Resource << image.size() << "saved as" << request.Image;
    return true;
}

bool Capture(rhi::DeviceHandle device,
             rhi::SurfaceHandle surface,
             const QString& worldPath,
             const QString& packagePath,
             QVector<ExportRequest> requests)
{
    const QByteArray bytes = ReadFile(packagePath);
    renderer::RenderGraphPackage package;
    if (package.Decode({reinterpret_cast<const uint8*>(bytes.constData()), static_cast<usize>(bytes.size())}) !=
        renderer::RenderGraphPackageStatus::Ready)
    {
        qCritical() << "Could not decode" << packagePath;
        return false;
    }
    const renderer::RenderGraphPlan plan = package.GetPlan();
    if (ResolveExports(bytes, packagePath, plan, requests) == false)
    {
        return false;
    }
    const format::Resource target = plan.Resources[static_cast<usize>(requests[0].Resource)];
    for (const ExportRequest& request : requests)
    {
        const format::Resource& other = plan.Resources[static_cast<usize>(request.Resource)];
        if (other.Width != target.Width || other.Height != target.Height)
        {
            qCritical() << request.Id << "has a different extent from" << requests[0].Id;
            return false;
        }
    }
    World world;
    if (LoadWorld(worldPath, target.Width, target.Height, world) == false)
    {
        qCritical() << "Could not load the saved translation-only cubes and orthographic camera of" << worldPath;
        return false;
    }
    renderer::RenderGraph graph;
    const Status initialized = graph.Initialize(device,
                                                plan,
                                                package.GetPrograms(ludus::shaders::renderer_composite::Vertex(),
                                                                    ludus::shaders::renderer_composite::Fragment()));
    if ((initialized != Status::Ready && initialized != Status::Pending) ||
        WaitFor(device, "graph preparation", [&graph]() { return graph.Poll(); }) == false)
    {
        return false;
    }
    renderer::RenderGraphMesh mesh;
    mesh.Geometry = {world.Positions.constData(),
                     static_cast<usize>(world.Positions.size()),
                     world.Indices.constData(),
                     static_cast<usize>(world.Indices.size())};
    renderer::RenderGraphReport report;
    const Status updated = graph.Update({&mesh, 1},
                                        {world.Items.constData(), static_cast<usize>(world.Items.size())},
                                        {&world.Camera, 1},
                                        1,
                                        renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                        report);
    if ((updated != Status::Ready && updated != Status::Pending) ||
        WaitFor(device, "scene upload", [&graph]() { return graph.Poll(); }) == false ||
        rhi::SetFrameTarget(device, surface, { .Width = target.Width, .Height = target.Height }) !=
            rhi::DeviceStatus::Ready)
    {
        return false;
    }
    rhi::SubmissionToken completion;
    const Status rendered = graph.Render(surface, target.Width, target.Height, completion);
    if (rendered != Status::Ready && rendered != Status::Pending)
    {
        qCritical() << "Raster submission failed" << static_cast<uint32>(rendered);
        return false;
    }
    for (const ExportRequest& request : requests)
    {
        if (SaveExport(device, graph, request, target) == false)
        {
            return false;
        }
    }
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    QString worldPath = QStringLiteral("cube.world.json");
    QString packagePath = QStringLiteral("graph.lurgrt");
    QVector<ExportRequest> requests{{QStringLiteral("Main/output"), QStringLiteral("capture.png")}};
    if (argc > 1)
    {
        if (argc < ARGUMENTS_BEFORE_EXPORTS + 2 || (argc - ARGUMENTS_BEFORE_EXPORTS) % 2 != 0)
        {
            qCritical()
                << "Usage: raster_capture <world.json> <package.lurgrt> <export id> <png> [<export id> <png>]...";
            return 1;
        }
        worldPath = QString::fromLocal8Bit(argv[1]);
        packagePath = QString::fromLocal8Bit(argv[2]);
        requests.clear();
        for (int32 i = ARGUMENTS_BEFORE_EXPORTS; i < argc; i += 2)
        {
            requests.append({QString::fromLocal8Bit(argv[i]), QString::fromLocal8Bit(argv[i + 1])});
        }
    }
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    rhi::StartupInfo failure;
    if (rhi::CreateDevice({},
                          { .Width = DEVICE_WIDTH, .Height = DEVICE_HEIGHT },
                          description,
                          device,
                          surface,
                          &failure) != rhi::DeviceStatus::Ready)
    {
        qCritical() << "Portable raster unavailable, startup error" << static_cast<uint32>(failure.Error);
        return 1;
    }
    rhi::DeviceInfo information;
    const bool isVulkan = rhi::GetDeviceInfo(device, information) == rhi::DeviceStatus::Ready &&
                          information.Startup.SelectedBackend == rhi::Backend::Vulkan;
    qInfo() << "SDK Vulkan" << isVulkan << "adapter" << information.Adapter.Name.GetData();
    const bool hasCaptured =
        isVulkan == true && Capture(device, surface, worldPath, packagePath, Move(requests)) == true;
    rhi::Shutdown();
    return hasCaptured == true ? 0 : 1;
}
