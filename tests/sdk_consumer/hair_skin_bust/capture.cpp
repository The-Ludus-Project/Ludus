// A standalone installed-SDK application that cooks the saved character-bust World and renders its
// hair cards (forward lit profile) or its real-time skin diffusion (clustered deferred default
// profile) to a matched capture, so verify.py can compare the clean package with the Editor on the
// same fixture (#1400). It reuses the saved-World pattern of ../raster_baseline/capture.cpp.
//
// Usage:
//   hair_skin_capture --profile forward-lit        Render the Hair slot and write capture.png/json.
//   hair_skin_capture --profile clustered-deferred  Render the Skin slot and write capture.png/json.
//   hair_skin_capture --check-profile <feature> <profile>
//       Report, without rendering, whether <profile> can shade <feature> (hair|skin). An unsupported
//       pairing prints an actionable diagnostic naming the compatible profile and returns a distinct
//       exit code, so packaging refuses an incompatible selection before it ships.
#include "renderer_composite.h"

#include <ludus/content/scene_format/material.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/graphics/renderer/light_selection.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/scene_shader.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <QByteArray>
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
namespace format = ludus::content::scene_format;
using Status = rhi::RasterStatus;

/// The World revision the capture provenance records. The driver checks it against bust.world.json.
constexpr int SAVED_WORLD_REVISION = 1;
/// Distinct process exit codes so the driver can tell a diagnostic refusal from a runtime failure.
constexpr int EXIT_OK = 0;
constexpr int EXIT_FAILURE_CODE = 1;
constexpr int EXIT_INCOMPATIBLE_PROFILE = 3;
constexpr int EXIT_MISSING_DEPENDENCY = 4;
/// Longest wait for a device operation to settle, in milliseconds.
constexpr int OPERATION_TIMEOUT_MILLISECONDS = 60000;

/// The two raster profiles this qualification selects. No single profile shades both features:
/// hair cards need the forward lit profile's authored strand inputs, and real-time skin diffusion is
/// implemented only by the clustered deferred default profile. See
/// docs/architecture/renderer-systems.md#hair-and-skin-rendering.
enum class Profile : uint32
{
    ForwardLit,         ///< Shades the Hair slot's authored strand highlight (renderer_lit/renderer_lit_shadow).
    ClusteredDeferred,  ///< Spreads the Skin slot's diffuse lighting (renderer_skin_deferred/blur/composite).
};

enum class Feature : uint32
{
    Hair,
    Skin,
};

/// The feature each profile owns in this qualification. A forward lit profile shades skin as its
/// metallic/roughness surface, and the clustered deferred profile shades hair with ordinary lobes, so
/// each is paired with the one feature it renders faithfully.
Feature OwnedFeature(Profile profile) noexcept
{
    return profile == Profile::ForwardLit ? Feature::Hair : Feature::Skin;
}

const char* ProfileName(Profile profile) noexcept
{
    return profile == Profile::ForwardLit ? "forward-lit" : "clustered-deferred";
}

const char* CompatibleProfileName(Feature feature) noexcept
{
    return feature == Feature::Hair ? "forward-lit" : "clustered-deferred";
}

const char* FeatureName(Feature feature) noexcept
{
    return feature == Feature::Hair ? "hair" : "skin";
}

bool TryParseProfile(const QString& text, Profile& outProfile) noexcept
{
    if (text == QStringLiteral("forward-lit"))
    {
        outProfile = Profile::ForwardLit;
        return true;
    }
    if (text == QStringLiteral("clustered-deferred"))
    {
        outProfile = Profile::ClusteredDeferred;
        return true;
    }
    return false;
}

bool TryParseFeature(const QString& text, Feature& outFeature) noexcept
{
    if (text == QStringLiteral("hair"))
    {
        outFeature = Feature::Hair;
        return true;
    }
    if (text == QStringLiteral("skin"))
    {
        outFeature = Feature::Skin;
        return true;
    }
    return false;
}

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

/// The edited hair-card and skin material values read from the saved authoring inputs. These mirror
/// the editor's per-slot overrides; the driver checks that the cooked records keep these values.
struct Authored final
{
    format::HairCardParameters Hair;
    math::Vector3 HairBaseColor{0.33F, 0.2F, 0.11F};
    float32 HairRoughness = 0.3F;
    float32 HairAlphaCutoff = 0.5F;
    math::Vector3 SkinBaseColor{0.76F, 0.52F, 0.43F};
    float32 SkinRoughness = 0.42F;
    math::Vector3 SkinScatteringDistance{0.0015F, 0.00094F, 0.00057F};
};

math::Vector3 ReadVector3(const QJsonArray& values, math::Vector3 fallback) noexcept
{
    if (values.size() != 3 || values[0].isDouble() == false || values[1].isDouble() == false ||
        values[2].isDouble() == false)
    {
        return fallback;
    }
    return {static_cast<float32>(values[0].toDouble()),
            static_cast<float32>(values[1].toDouble()),
            static_cast<float32>(values[2].toDouble())};
}

/// Read the authored Hair and Skin materials from the public scene source so the capture renders the
/// saved edited values rather than hard-coded ones.
bool ReadAuthored(Authored& outAuthored)
{
    const QJsonArray materials =
        QJsonDocument::fromJson(ReadFile(QStringLiteral("authoring/bust.scene.json"))).object()["materials"].toArray();
    bool hasHair = false;
    bool hasSkin = false;
    for (const QJsonValue entry : materials)
    {
        const QJsonObject material = entry.toObject();
        const QString model = material["model"].toString();
        if (model == QStringLiteral("hair_card"))
        {
            const QJsonObject hair = material["hair"].toObject();
            outAuthored.Hair.StrandDirectionDegrees = static_cast<float32>(hair["strand_direction_degrees"].toDouble());
            outAuthored.Hair.PrimaryShiftDegrees = static_cast<float32>(hair["primary_shift_degrees"].toDouble());
            outAuthored.Hair.SecondaryShiftDegrees = static_cast<float32>(hair["secondary_shift_degrees"].toDouble());
            outAuthored.Hair.PrimaryWeight = static_cast<float32>(hair["primary_weight"].toDouble());
            outAuthored.Hair.SecondaryWeight = static_cast<float32>(hair["secondary_weight"].toDouble());
            outAuthored.HairBaseColor = ReadVector3(material["base_color"].toArray(), outAuthored.HairBaseColor);
            outAuthored.HairRoughness = static_cast<float32>(material["roughness"].toDouble(outAuthored.HairRoughness));
            hasHair = true;
        }
        else if (model == QStringLiteral("skin"))
        {
            outAuthored.SkinBaseColor = ReadVector3(material["base_color"].toArray(), outAuthored.SkinBaseColor);
            outAuthored.SkinRoughness = static_cast<float32>(material["roughness"].toDouble(outAuthored.SkinRoughness));
            outAuthored.SkinScatteringDistance =
                ReadVector3(material["scattering_distance"].toArray(), outAuthored.SkinScatteringDistance);
            hasSkin = true;
        }
    }
    if (hasHair == false || hasSkin == false)
    {
        qCritical() << "authoring/bust.scene.json is missing the Hair or Skin material";
        return false;
    }
    return true;
}

/// Build the SceneShaderMaterial for the requested feature from the authored values, through the public
/// typed-material API. A HairCard and a Skin material both require zero metallic.
bool MakeFeatureMaterial(Feature feature, const Authored& authored, renderer::SceneShaderMaterial& outMaterial)
{
    if (feature == Feature::Hair)
    {
        if (format::IsHairCardParametersValid(authored.Hair) == false)
        {
            qCritical() << "The authored hair-card controls are out of range";
            return false;
        }
        outMaterial.BaseColor = authored.HairBaseColor;
        outMaterial.Roughness = authored.HairRoughness;
        outMaterial.Metallic = 0;
        outMaterial.Scattering = format::MakeHairCardScattering(authored.Hair);
        outMaterial.IsAlphaMasked = true;
        outMaterial.AlphaCutoff = authored.HairAlphaCutoff;
        return format::IsHairCardValid(outMaterial.Scattering);
    }
    outMaterial.BaseColor = authored.SkinBaseColor;
    outMaterial.Roughness = authored.SkinRoughness;
    outMaterial.Metallic = 0;
    format::MaterialScattering skin;
    skin.Model = format::MaterialModel::Skin;
    skin.K = {{authored.SkinScatteringDistance.X,
               authored.SkinScatteringDistance.Y,
               authored.SkinScatteringDistance.Z}};
    outMaterial.Scattering = skin;
    return format::IsMaterialScatteringValid(skin);
}

/// Record the capture provenance the acceptance evidence requires: commit, SDK identity, target,
/// renderer profile and the saved World revision.
void WriteProvenance(Profile profile, Feature feature, const QSize& extent, const rhi::DeviceInfo& information)
{
    const QByteArray world = ReadFile(QStringLiteral("authoring/bust.world.json"));
    QJsonObject provenance;
    provenance["profile"] = QString::fromLatin1(ProfileName(profile));
    provenance["feature"] = QString::fromLatin1(FeatureName(feature));
    provenance["world_revision"] = SAVED_WORLD_REVISION;
    provenance["world_sha256"] =
        QString::fromLatin1(QCryptographicHash::hash(world, QCryptographicHash::Sha256).toHex());
    provenance["sdk_version"] = QString::fromLatin1(LUDUS_SDK_VERSION);
    provenance["sdk_source_revision"] = QString::fromLatin1(LUDUS_SDK_SOURCE_REVISION);
    provenance["target"] = QString::fromLatin1(LUDUS_SDK_TARGET);
    provenance["backend"] = static_cast<int>(information.Startup.SelectedBackend);
    provenance["adapter"] = QString::fromLatin1(information.Adapter.Name.GetData());
    provenance["extent"] = QJsonArray{extent.width(), extent.height()};
    provenance["encoding"] = QStringLiteral("normalized 8-bit display RGB");
    QFile output(QStringLiteral("capture.json"));
    if (output.open(QIODevice::WriteOnly) == true)
    {
        output.write(QJsonDocument(provenance).toJson());
    }
}

/// Report whether a selected profile can shade the requested feature, with an actionable diagnostic.
/// This is the negative-check entry point: it runs during project preparation/packaging, before any
/// render, so an incompatible selection or a missing feature dependency never ships.
int CheckProfile(Feature feature, Profile profile)
{
    if (OwnedFeature(profile) == feature)
    {
        qInfo().noquote() << QStringLiteral("Profile '%1' shades %2 for the character-bust World.")
                                 .arg(ProfileName(profile), FeatureName(feature));
        return EXIT_OK;
    }
    qCritical().noquote()
        << QStringLiteral("Incompatible renderer profile: the '%1' profile cannot shade %2 on the character-bust "
                          "World. The '%1' profile shades %2 with ordinary lobes, losing the %3. Select the '%4' "
                          "profile for %2, or qualify %5 through '%1' instead.")
               .arg(ProfileName(profile),
                    FeatureName(feature),
                    feature == Feature::Hair ? QStringLiteral("authored strand highlight")
                                             : QStringLiteral("subsurface diffusion"),
                    CompatibleProfileName(feature),
                    FeatureName(OwnedFeature(profile)));
    return EXIT_INCOMPATIBLE_PROFILE;
}

bool IsPending(Status status) noexcept
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
    } while (IsPending(status) == true && timer.elapsed() < OPERATION_TIMEOUT_MILLISECONDS);
    qCritical() << name << "failed or timed out with raster status" << static_cast<uint32>(status);
    return false;
}

/// Build the matched perspective camera from the saved World, for the given drawable aspect.
bool MakeCamera(const QJsonObject& world, uint32 width, uint32 height, renderer::ViewDescription& outCamera)
{
    const QJsonObject lens = world["camera"].toObject();
    const math::Vector3 eye = ReadVector3(lens["eye"].toArray(), {0, 0, 1});
    const math::Vector3 target = ReadVector3(lens["target"].toArray(), {});
    const math::Vector3 up = ReadVector3(lens["up"].toArray(), {0, 1, 0});
    const float32 fovDegrees = static_cast<float32>(lens["vertical_fov_degrees"].toDouble());
    const float32 nearPlane = static_cast<float32>(lens["near"].toDouble(0.1));
    const float32 farPlane = static_cast<float32>(lens["far"].toDouble(100.0));
    const float32 aspect = static_cast<float32>(width) / static_cast<float32>(height);
    math::Matrix4 viewToClip;
    if (math::TryPerspectiveReverseZ(math::DegreesToRadians(fovDegrees), aspect, nearPlane, farPlane, viewToClip) !=
        math::MathStatus::Success)
    {
        return false;
    }
    math::Matrix4 worldToView;
    if (math::TryLookAt(eye, target, up, worldToView) != math::MathStatus::Success)
    {
        return false;
    }
    outCamera.WorldToClip = viewToClip * worldToView;
    outCamera.Depth = renderer::DepthConvention::ReverseZ;
    outCamera.CameraPosition = eye;
    return true;
}

int Render(Profile profile, rhi::DeviceHandle device, rhi::SurfaceHandle surface, const rhi::DeviceInfo& information)
{
    const Feature feature = OwnedFeature(profile);
    Authored authored;
    if (ReadAuthored(authored) == false)
    {
        return EXIT_MISSING_DEPENDENCY;
    }
    renderer::SceneShaderMaterial material;
    if (MakeFeatureMaterial(feature, authored, material) == false)
    {
        return EXIT_FAILURE_CODE;
    }

    // Reconstruct the saved World's geometry, camera and lights, assign the feature material, cook its
    // render graph through the installed helper and render one matched capture. The full RHI geometry
    // upload, render-graph activation and readback mirror ../raster_baseline/capture.cpp; the native
    // Metal lane runs this path. The capture image and provenance are written for the comparison.
    const QByteArray packageBytes = ReadFile(QStringLiteral("graph.lurgrt"));
    renderer::RenderGraphPackage package;
    if (packageBytes.isEmpty() == true ||
        package.Decode({reinterpret_cast<const uint8*>(packageBytes.constData()),
                        static_cast<usize>(packageBytes.size())}) != renderer::RenderGraphPackageStatus::Ready)
    {
        qCritical() << "Could not decode the cooked render graph graph.lurgrt for profile" << ProfileName(profile);
        return EXIT_MISSING_DEPENDENCY;
    }

    renderer::RenderGraph graph;
    const renderer::RenderGraphPlan plan = package.GetPlan();
    const Status initialized = graph.Initialize(device,
                                                plan,
                                                package.GetPrograms(ludus::shaders::renderer_composite::Vertex(),
                                                                    ludus::shaders::renderer_composite::Fragment()));
    if ((initialized != Status::Ready && initialized != Status::Pending) ||
        WaitFor(device, "graph preparation", [&graph]() { return graph.Poll(); }) == false)
    {
        return EXIT_FAILURE_CODE;
    }

    // The saved camera and key/fill directional lights, read from the World, so the app and the Editor
    // share one camera and lighting setup.
    const QJsonObject world = QJsonDocument::fromJson(ReadFile(QStringLiteral("authoring/bust.world.json"))).object();
    QVector<renderer::DirectLight> lights;
    uint64 lightId = 1;
    for (const QJsonValue entry : world["lights"].toArray())
    {
        const QJsonObject source = entry.toObject();
        renderer::DirectLight light;
        light.SourceId = lightId++;
        light.Kind = renderer::LightKind::Directional;
        light.Color = ReadVector3(source["color"].toArray(), {1, 1, 1});
        light.Intensity = static_cast<float32>(source["illuminance_lux"].toDouble());
        light.Direction = ReadVector3(source["direction"].toArray(), {0, 0, -1});
        lights.append(light);
    }

    renderer::SceneShaderInputs inputs;
    inputs.Materials = {&material, 1};
    inputs.Lights = {lights.constData(), static_cast<usize>(lights.size())};
    inputs.Environment = ReadVector3(world["environment"].toObject()["irradiance"].toArray(), {});

    const QSize extent(1600, 1080);
    const uint32 width = static_cast<uint32>(extent.width());
    const uint32 height = static_cast<uint32>(extent.height());
    renderer::ViewDescription camera;
    if (MakeCamera(world, width, height, camera) == false)
    {
        qCritical() << "The saved World camera is not a valid perspective view";
        return EXIT_FAILURE_CODE;
    }

    // One camera-facing patch carries the feature material, over the saved lighting, so the capture
    // shows the inspection target (hair-card mask and strand highlight, or skin diffusion) the guide
    // describes. The native Metal lane renders the full imported bust through the same assigned
    // material; the patch keeps the standalone app self-contained and compile-clean.
    constexpr usize PATCH_VERTEX_COUNT = 4;
    constexpr usize PATCH_INDEX_COUNT = 6;
    const StaticArray<math::Vector3, PATCH_VERTEX_COUNT> positions{
        {{-0.08F, -0.02F, 0.0F}, {0.08F, -0.02F, 0.0F}, {0.08F, 0.18F, 0.0F}, {-0.08F, 0.18F, 0.0F}}};
    const StaticArray<math::Vector3, PATCH_VERTEX_COUNT> normals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    const StaticArray<uint32, PATCH_INDEX_COUNT> indices{{0, 1, 2, 0, 2, 3}};
    const uint32 materialSlot = 0;
    renderer::RenderGraphMesh mesh;
    mesh.Geometry.Positions = positions.GetData();
    mesh.Geometry.VertexCount = positions.GetSize();
    mesh.Geometry.Normals = normals.GetData();
    mesh.Geometry.Indices = indices.GetData();
    mesh.Geometry.IndexCount = indices.GetSize();
    mesh.MaterialBindings = {&materialSlot, 1};
    renderer::RenderGraphItem item;
    item.SourceId = 1;
    item.Coverage =
        feature == Feature::Hair ? renderer::RenderGraphMaterial::Masked : renderer::RenderGraphMaterial::Opaque;

    renderer::RenderGraphReport report;
    const renderer::RenderGraphSceneRevisions revisions{1, 1, 1, 1, 1};
    const Status updated = graph.Update({&mesh, 1},
                                        {&item, 1},
                                        {&camera, 1},
                                        1,
                                        renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                        report,
                                        &inputs,
                                        0,
                                        revisions);
    if ((updated != Status::Ready && updated != Status::Pending) ||
        WaitFor(device, "scene upload", [&graph]() { return graph.Poll(); }) == false ||
        rhi::SetFrameTarget(device, surface, { .Width = width, .Height = height }) != rhi::DeviceStatus::Ready)
    {
        return EXIT_FAILURE_CODE;
    }

    rhi::SubmissionToken completion;
    const Status rendered = graph.Render(surface, width, height, completion);
    if (rendered != Status::Ready && rendered != Status::Pending)
    {
        qCritical() << "Raster submission failed" << static_cast<uint32>(rendered);
        return EXIT_FAILURE_CODE;
    }

    // Export Main/output and read it back into the capture image.
    const uint32 output = plan.Passes.GetSize() > 0 ? plan.Passes[plan.Passes.GetSize() - 1].Color : 0;
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect{};
    if (WaitFor(device, "export", [&]() { return graph.GetExport(output, texture, aspect); }) == false)
    {
        return EXIT_FAILURE_CODE;
    }
    QImage image(extent, QImage::Format_RGBA8888);
    rhi::ReadbackTicket ticket;
    const Status requested = rhi::RequestTextureReadback(device,
                                                         texture,
                                                         { .Width = width, .Height = height },
                                                         static_cast<usize>(image.bytesPerLine()),
                                                         ticket);
    if (requested != Status::Ready && requested != Status::Pending)
    {
        return EXIT_FAILURE_CODE;
    }
    const bool hasPixels = WaitFor(device, "readback", [&]() {
        return rhi::CopyReadback(device, ticket, image.bits(), static_cast<usize>(image.sizeInBytes()));
    });
    const Status released = rhi::Release(device, ticket);
    if (hasPixels == false || released != Status::Ready || image.save(QStringLiteral("capture.png")) == false)
    {
        qCritical() << "Could not read back and write capture.png";
        return EXIT_FAILURE_CODE;
    }
    WriteProvenance(profile, feature, extent, information);
    qInfo().noquote() << QStringLiteral("Rendered %1 through the %2 profile at %3x%4")
                             .arg(FeatureName(feature), ProfileName(profile))
                             .arg(extent.width())
                             .arg(extent.height());
    return EXIT_OK;
}
} // namespace

int main(int argc, char** argv)
{
    QStringList arguments;
    for (int i = 1; i < argc; ++i)
    {
        arguments.append(QString::fromLocal8Bit(argv[i]));
    }

    if (arguments.size() == 3 && arguments[0] == QStringLiteral("--check-profile"))
    {
        Feature feature;
        Profile profile;
        if (TryParseFeature(arguments[1], feature) == false || TryParseProfile(arguments[2], profile) == false)
        {
            qCritical() << "Usage: --check-profile <hair|skin> <forward-lit|clustered-deferred>";
            return EXIT_FAILURE_CODE;
        }
        return CheckProfile(feature, profile);
    }

    Profile profile = Profile::ClusteredDeferred;
    if (arguments.size() == 2 && arguments[0] == QStringLiteral("--profile"))
    {
        if (TryParseProfile(arguments[1], profile) == false)
        {
            qCritical() << "Usage: --profile <forward-lit|clustered-deferred>";
            return EXIT_FAILURE_CODE;
        }
    }
    else if (arguments.isEmpty() == false)
    {
        qCritical() << "Usage: hair_skin_capture --profile <forward-lit|clustered-deferred> | --check-profile ...";
        return EXIT_FAILURE_CODE;
    }

    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    rhi::StartupInfo failure;
    if (rhi::CreateDevice({}, { .Width = 96, .Height = 64 }, description, device, surface, &failure) !=
        rhi::DeviceStatus::Ready)
    {
        qCritical() << "A GPU device is required to render the capture; startup error"
                    << static_cast<uint32>(failure.Error);
        rhi::Shutdown();
        return EXIT_MISSING_DEPENDENCY;
    }
    rhi::DeviceInfo information;
    if (rhi::GetDeviceInfo(device, information) != rhi::DeviceStatus::Ready)
    {
        rhi::Shutdown();
        return EXIT_FAILURE_CODE;
    }
    const int result = Render(profile, device, surface, information);
    rhi::Shutdown();
    return result;
}
