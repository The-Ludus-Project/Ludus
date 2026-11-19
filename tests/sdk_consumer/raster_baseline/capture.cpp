// A standalone installed-SDK application for the saved, translation-only cube World.
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

// Resolve the authored Export against the names for these exact package bytes.
int32 FindExport(const QByteArray& bytes)
{
    const QJsonObject names = QJsonDocument::fromJson(ReadFile("graph.lurgrt.resources.json")).object();
    if (names["runtime_sha256"].toString().toLatin1() !=
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())
    {
        return -1;
    }
    const QJsonArray resources = names["resources"].toArray();
    for (const QJsonValue operation : names["operations"].toArray())
    {
        if (operation["kind"].toString() == "Export" && operation["id"].toString() == "Main/output")
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
    renderer::RenderGraphItem Item;
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

bool LoadWorld(uint32 width, uint32 height, World& outWorld)
{
    const QJsonArray entities = QJsonDocument::fromJson(ReadFile("cube.world.json")).object()["entities"].toArray();
    const QJsonObject mesh = QJsonDocument::fromJson(ReadFile("authoring/cube.mesh.json")).object();
    if (entities.size() != 2 || mesh["version"] != 1)
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
        outWorld.Positions.append(math::Vector3{static_cast<float32>(xyz[0].toDouble()),
                                                static_cast<float32>(xyz[1].toDouble()),
                                                static_cast<float32>(xyz[2].toDouble())});
    }
    for (const QJsonValue index : mesh["indices"].toArray())
    {
        const int32 vertex = index.toInt(-1);
        if (vertex < 0 || vertex >= outWorld.Positions.size())
        {
            return false;
        }
        outWorld.Indices.append(static_cast<uint32>(vertex));
    }
    bool hasCamera = false;
    bool hasMesh = false;
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
            outWorld.Item.SourceId = 1;
            outWorld.Item.MeshToWorld.Translation = positionWorld;
            outWorld.Item.Material.Color = {static_cast<float32>(surface["r"].toDouble()),
                                            static_cast<float32>(surface["g"].toDouble()),
                                            static_cast<float32>(surface["b"].toDouble()),
                                            1};
            hasMesh = true;
        }
        else
        {
            const QJsonObject lens = components["camera"].toObject();
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
            outWorld.Camera.WorldToClip = viewToClip * math::ToMatrix4(worldToView);
            for (math::Vector4& column : outWorld.Camera.WorldToClip.Columns)
            {
                column.Z = column.W - column.Z;
            }
            outWorld.Camera.Depth = renderer::DepthConvention::Conventional;
            outWorld.Camera.OrthographicCamera = true;
            outWorld.Camera.CameraPosition = positionWorld;
            hasCamera = true;
        }
    }
    return hasMesh == true && hasCamera == true && outWorld.Indices.isEmpty() == false;
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
    } while (IsPending(status) == true && timer.elapsed() < 60000);
    qCritical() << name << "failed or timed out with raster status" << static_cast<uint32>(status);
    return false;
}

bool Capture(rhi::DeviceHandle device, rhi::SurfaceHandle surface)
{
    const QByteArray bytes = ReadFile("graph.lurgrt");
    const int32 resource = FindExport(bytes);
    renderer::RenderGraphPackage package;
    if (resource < 0 || package.Decode({reinterpret_cast<const uint8*>(bytes.constData()),
                                        static_cast<usize>(bytes.size())}) != renderer::RenderGraphPackageStatus::Ready)
    {
        qCritical() << "Could not decode graph.lurgrt and resolve Main/output";
        return false;
    }
    const renderer::RenderGraphPlan plan = package.GetPlan();
    if (static_cast<usize>(resource) >= plan.Resources.GetSize() || plan.Passes.GetSize() != 1 ||
        plan.Passes[0].Program != format::Technique::ColorAndDepth ||
        plan.Passes[0].Color != static_cast<uint32>(resource))
    {
        qCritical() << "Main/output does not select the authored raster color pass";
        return false;
    }
    const format::Resource target = plan.Resources[static_cast<usize>(resource)];
    World world;
    if (LoadWorld(target.Width, target.Height, world) == false)
    {
        qCritical() << "Could not load the saved cube and its orthographic camera";
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
                                        {&world.Item, 1},
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
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect{};
    if (WaitFor(device, "export", [&]() { return graph.GetExport(static_cast<uint32>(resource), texture, aspect); }) ==
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
    if (hasPixels == false || released != Status::Ready || image.save("capture.png") == false)
    {
        return false;
    }
    qInfo() << "Raster ColorAndDepth Main/output resource" << resource << image.size();
    return true;
}
} // namespace

int main()
{
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    rhi::StartupInfo failure;
    if (rhi::CreateDevice({}, { .Width = 96, .Height = 64 }, description, device, surface, &failure) !=
        rhi::DeviceStatus::Ready)
    {
        qCritical() << "Portable raster unavailable, startup error" << static_cast<uint32>(failure.Error);
        return 1;
    }
    rhi::DeviceInfo information;
    const bool isVulkan = rhi::GetDeviceInfo(device, information) == rhi::DeviceStatus::Ready &&
                          information.Startup.SelectedBackend == rhi::Backend::Vulkan;
    qInfo() << "SDK Vulkan" << isVulkan << "adapter" << information.Adapter.Name.GetData();
    const bool hasCaptured = isVulkan == true && Capture(device, surface) == true;
    rhi::Shutdown();
    return hasCaptured == true ? 0 : 1;
}
