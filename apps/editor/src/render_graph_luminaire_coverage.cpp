#include "internal/render_graph_luminaire_coverage.h"
#include "internal/render_graph_canvas_feedback.h"
#include "internal/render_graph_document.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/scalar.hpp>

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>

namespace ludus::editor
{
using namespace foundation;
namespace
{
/// Subgraph import depth the search follows; the compiler rejects deeper or cyclic imports.
constexpr int32 MAX_IMPORT_DEPTH = 64;
/// Shade functions of slice_sampling_luminaire/main_shaders/shade.slang. Both import paths keep the functions and
/// the source file name; only program ids and source directories change.
constexpr const char* SHADE_SOURCE_SUFFIX = "main_shaders/shade.slang";
/// World bounds whose farthest corner lies less than this in front of the panel plane count as behind it.
constexpr float32 FRONT_TOLERANCE_WORLD = 1e-4F;
constexpr int32 AABB_CORNER_COUNT = 8;
constexpr int32 COORDINATE_DECIMALS = 2;
constexpr float32 PERCENT = 100;
/// A fitted panel hangs this fraction of its ceiling distance (or the bounds' height) below it, so the ceiling's own
/// receivers stay behind the panel, and at least MIN_CEILING_GAP_WORLD below.
constexpr float32 CEILING_GAP_FRACTION = 0.01F;
constexpr float32 MIN_CEILING_GAP_WORLD = 0.01F;
/// A fitted panel's side is at least this multiple of MIN_PANEL_WORLD_FRACTION of the bounds diagonal.
constexpr float32 FITTED_PANEL_SIZE_MARGIN = 2;

bool IsShadeFunction(const QString& function)
{
    return function == QStringLiteral("ShadeDefensiveSliceSampling") ||
           function == QStringLiteral("ShadeSliceSampling") || function == QStringLiteral("ShadeUniformArea");
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): configuration identity is resolved within the named file.
QJsonObject FindConfiguration(const RenderGraphDocument& document, const QString& file, const QString& identity)
{
    QJsonObject graph;
    if (document.GetGraph(file, graph) == true)
    {
        for (const QJsonValueConstRef value : graph.value(QStringLiteral("configurations")).toArray())
        {
            if (value.toObject().value(QStringLiteral("id")).toString() == identity)
            {
                return value.toObject();
            }
        }
    }
    return {};
}

/// Program id to its shade.slang source, for every manifest program running a template shade function.
QHash<QString, QString> FindShadePrograms(const RenderGraphDocument& document)
{
    const QString& root = document.GetRootGraph();
    const QString manifestFile =
        root.chopped(QStringLiteral(".lurender.json").size()) + QStringLiteral(".lushader.json");
    const QJsonObject manifest = QJsonDocument::fromJson(document.GetDraft(manifestFile)).object();
    QHash<QString, QString> programs;
    for (const QJsonValueConstRef value : manifest.value(QStringLiteral("programs")).toArray())
    {
        const QJsonObject program = value.toObject();
        const QString source = program.value(QStringLiteral("source")).toString();
        if (IsShadeFunction(program.value(QStringLiteral("function")).toString()) == true &&
            source.endsWith(QLatin1String(SHADE_SOURCE_SUFFIX)) == true)
        {
            programs.insert(program.value(QStringLiteral("id")).toString(), source);
        }
    }
    return programs;
}

/// Depth-first search of the retained nodes of file#configuration for a ComputePipeline running a shade program.
bool FindShadeSource(const RenderGraphDocument& document,
                     const QHash<QString, QString>& programs,
                     const QString& file,
                     const QString& configuration,
                     int32 depth,
                     QSet<QString>& inoutVisited,
                     QString& outSource)
{
    const QString key = file + QLatin1Char('#') + configuration;
    if (depth >= MAX_IMPORT_DEPTH || inoutVisited.contains(key) == true)
    {
        return false;
    }
    inoutVisited.insert(key);
    const QJsonObject record = FindConfiguration(document, file, configuration);
    // At the root only Retained nodes are on the route; Unknown (broken structure or unresolved imports) is never
    // reported. A retained Subgraph's own nodes count unless they are explicitly off the route: inspected on its own,
    // a library cannot resolve its `input` wires and reports Unknown.
    const QHash<QString, RenderGraphActivityFeedback> activity =
        InspectRenderGraphActivity(document, file, configuration);
    for (const QJsonValueConstRef value : record.value(QStringLiteral("nodes")).toArray())
    {
        const QJsonObject node = value.toObject();
        const RenderGraphActivity state = activity.value(node.value(QStringLiteral("id")).toString()).Activity;
        const bool isOnRoute = depth == 0
                                   ? state == RenderGraphActivity::Retained
                                   : state != RenderGraphActivity::Inactive && state != RenderGraphActivity::Disabled;
        if (isOnRoute == false)
        {
            continue;
        }
        const QString kind = node.value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("Subgraph"))
        {
            if (FindShadeSource(document,
                                programs,
                                node.value(QStringLiteral("asset")).toString(),
                                node.value(QStringLiteral("configuration")).toString(),
                                depth + 1,
                                inoutVisited,
                                outSource) == true)
            {
                return true;
            }
            continue;
        }
        for (const QJsonValueConstRef shaderValue : node.value(QStringLiteral("shaders")).toArray())
        {
            const QString program = shaderValue.toObject().value(QStringLiteral("program")).toString();
            if (programs.contains(program) == true)
            {
                outSource = programs.value(program);
                return true;
            }
        }
    }
    return false;
}

QString FormatPoint(math::Vector3 pointWorld)
{
    return QStringLiteral("(%1, %2, %3)")
        .arg(static_cast<float64>(pointWorld.X), 0, 'f', COORDINATE_DECIMALS)
        .arg(static_cast<float64>(pointWorld.Y), 0, 'f', COORDINATE_DECIMALS)
        .arg(static_cast<float64>(pointWorld.Z), 0, 'f', COORDINATE_DECIMALS);
}

QString FormatCount(int32 count, const QString& singular, const QString& plural)
{
    return QStringLiteral("%1 %2").arg(count).arg(count == 1 ? singular : plural);
}

bool IsInside(const math::Aabb3& bounds, math::Vector3 point)
{
    return point.X >= bounds.Min.X && point.X <= bounds.Max.X && point.Y >= bounds.Min.Y && point.Y <= bounds.Max.Y &&
           point.Z >= bounds.Min.Z && point.Z <= bounds.Max.Z;
}

/// Largest signed distance of a bounds corner in front of the panel plane.
float32 GetFrontDistance(const graphics::renderer::PanelLuminaire& luminaire, const math::Aabb3& bounds)
{
    const math::Vector3 normalWorld = math::Cross(luminaire.TangentWorld, luminaire.BitangentWorld);
    float32 front = math::Dot(bounds.Min - luminaire.CenterWorld, normalWorld);
    for (int32 corner = 1; corner < AABB_CORNER_COUNT; ++corner)
    {
        const math::Vector3 point{(corner & 1) != 0 ? bounds.Max.X : bounds.Min.X,
                                  (corner & 2) != 0 ? bounds.Max.Y : bounds.Min.Y,
                                  (corner & 4) != 0 ? bounds.Max.Z : bounds.Min.Z};
        front = math::Max(front, math::Dot(point - luminaire.CenterWorld, normalWorld));
    }
    return front;
}

/// Why one luminaire cannot light the World, or an empty string when it can.
QString DescribeLuminaire(const graphics::renderer::PanelLuminaire& luminaire, const RenderGraphLuminaireWorld& world)
{
    const math::Vector3 extent = world.BoundsWorld.Max - world.BoundsWorld.Min;
    const float32 diagonal = math::Length(extent);
    const float32 side = 2 * luminaire.HalfExtentWorld;
    if (GetFrontDistance(luminaire, world.BoundsWorld) <= FRONT_TOLERANCE_WORLD)
    {
        return QStringLiteral("the luminaire at %1 emits towards %2, away from every World object")
            .arg(FormatPoint(luminaire.CenterWorld),
                 FormatPoint(math::Cross(luminaire.TangentWorld, luminaire.BitangentWorld)));
    }
    if (IsInside(world.BoundsWorld, luminaire.CenterWorld) == false)
    {
        return QStringLiteral("the luminaire at %1 lies outside the World's bounds %2 to %3")
            .arg(FormatPoint(luminaire.CenterWorld),
                 FormatPoint(world.BoundsWorld.Min),
                 FormatPoint(world.BoundsWorld.Max));
    }
    if (side < MIN_PANEL_WORLD_FRACTION * diagonal)
    {
        return QStringLiteral("the %1 m luminaire at %2 spans %3% of the World's %4 m bounds diagonal, so it lights "
                              "at most a small patch")
            .arg(static_cast<float64>(side), 0, 'f', COORDINATE_DECIMALS)
            .arg(FormatPoint(luminaire.CenterWorld))
            .arg(static_cast<float64>(PERCENT * side / diagonal), 0, 'f', 1)
            .arg(static_cast<float64>(diagonal), 0, 'f', 1);
    }
    return {};
}
} // namespace

bool IsRenderGraphLuminaireRoute(const RenderGraphDocument& document, const QString& configuration)
{
    if (document.IsLoaded() == false)
    {
        return false;
    }
    const QHash<QString, QString> programs = FindShadePrograms(document);
    if (programs.isEmpty() == true)
    {
        return false;
    }
    QSet<QString> visited;
    QString shadeSource;
    return FindShadeSource(document, programs, document.GetRootGraph(), configuration, 0, visited, shadeSource);
}

WorldLuminairePlacement FitRenderGraphLuminaire(const RenderGraphLuminaireWorld& world)
{
    const math::Aabb3& bounds = world.BoundsWorld;
    const math::Vector3 extent = bounds.Max - bounds.Min;
    const bool isAnchorInside = world.HasAnchor == true && IsInside(bounds, world.AnchorWorld) == true;
    const math::Vector3 center = (bounds.Min + bounds.Max) * 0.5F;
    WorldLuminairePlacement placement;
    placement.RotationDegrees = {WORLD_LUMINAIRE_DOWNWARD_DEGREES, 0, 0};
    placement.PositionWorld = isAnchorInside == true ? world.AnchorWorld : center;
    if (isAnchorInside == true && world.HasCeiling == true)
    {
        placement.PositionWorld.Y += world.CeilingHeightWorld -
                                     math::Max(MIN_CEILING_GAP_WORLD, CEILING_GAP_FRACTION * world.CeilingHeightWorld);
    }
    else
    {
        placement.PositionWorld.Y = bounds.Max.Y - math::Max(MIN_CEILING_GAP_WORLD, CEILING_GAP_FRACTION * extent.Y);
    }
    const float32 side = math::Max(FITTED_PANEL_WORLD_FRACTION * math::Min(extent.X, extent.Z),
                                   FITTED_PANEL_SIZE_MARGIN * MIN_PANEL_WORLD_FRACTION * math::Length(extent));
    placement.HalfExtentWorld = math::Clamp(side * 0.5F,
                                            graphics::renderer::MIN_PANEL_LUMINAIRE_HALF_EXTENT,
                                            graphics::renderer::MAX_PANEL_LUMINAIRE_HALF_EXTENT);
    return placement;
}

RenderGraphLuminaireCoverage DescribeRenderGraphLuminaireCoverage(const RenderGraphLuminaireWorld& world)
{
    RenderGraphLuminaireCoverage result;
    QString reason;
    if (world.Luminaires.Count == 0)
    {
        reason = QStringLiteral("this World has no enabled luminaire");
    }
    else if (world.HasBounds == true)
    {
        for (uint32 i = 0; i < world.Luminaires.Count; ++i)
        {
            const QString luminaireReason = DescribeLuminaire(world.Luminaires.Luminaires[i], world);
            if (luminaireReason.isEmpty() == true)
            {
                return result;
            }
            if (i == 0)
            {
                reason = luminaireReason;
            }
        }
    }
    if (reason.isEmpty() == true)
    {
        return result;
    }
    const bool hasWorldLighting = world.EmissiveObjectCount > 0 || world.LightCount > 0;
    result.Warning = QStringLiteral("The slice-sampling-luminaire template cannot light this World: %1.").arg(reason);
    if (hasWorldLighting == true)
    {
        QStringList ignored;
        if (world.EmissiveObjectCount > 0)
        {
            ignored.append(FormatCount(world.EmissiveObjectCount,
                                       QStringLiteral("emissive object"),
                                       QStringLiteral("emissive objects")));
        }
        if (world.LightCount > 0)
        {
            ignored.append(FormatCount(world.LightCount, QStringLiteral("light"), QStringLiteral("lights")));
        }
        result.Warning += QStringLiteral(" Only luminaires light surfaces in this template, not the World's %1.")
                              .arg(ignored.join(QStringLiteral(" or ")));
    }
    if (world.CanRepair == true && world.HasBounds == true)
    {
        result.Placement = FitRenderGraphLuminaire(world);
        result.Repair = world.Luminaires.Count == 0 ? RenderGraphLuminaireRepair::AddLuminaire
                                                    : RenderGraphLuminaireRepair::MoveLuminaire;
        result.LuminaireSourceId = world.Luminaires.Count == 0 ? 0 : world.Luminaires.Luminaires[0].SourceId;
        result.Warning +=
            QStringLiteral(" Open this warning from the Error List to %1 a %2 m luminaire at %3, or "
                           "route the output from a template that samples the World's emitters, such "
                           "as ReSTIR DI.")
                .arg(world.Luminaires.Count == 0 ? QStringLiteral("add") : QStringLiteral("fit"))
                .arg(static_cast<float64>(2 * result.Placement.HalfExtentWorld), 0, 'f', COORDINATE_DECIMALS)
                .arg(FormatPoint(result.Placement.PositionWorld));
    }
    else
    {
        result.Warning += QStringLiteral(" Attach a luminaire component to an entity and place it with its Transform, "
                                         "or route the output from a template that samples the World's emitters, "
                                         "such as ReSTIR DI.");
    }
    return result;
}
} // namespace ludus::editor
