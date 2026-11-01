#include "internal/render_graph_luminaire_coverage.h"
#include "internal/render_graph_canvas_feedback.h"
#include "internal/render_graph_document.h"

#include <ludus/foundation/base/parse_number.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/status.hpp>

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
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
constexpr const char* PLACEMENT_SOURCE = "luminaire.slang";
/// World bounds whose farthest corner lies less than this in front of the panel plane count as behind it.
constexpr float32 FRONT_TOLERANCE_WORLD = 1e-4F;
constexpr int32 AABB_CORNER_COUNT = 8;
constexpr qsizetype VECTOR3_COMPONENT_COUNT = 3;
/// Half extent below which the panel is degenerate; luminaire.slang divides by it.
constexpr float32 MIN_HALF_EXTENT_WORLD = 1e-6F;
constexpr int32 COORDINATE_DECIMALS = 2;
constexpr float32 PERCENT = 100;

bool IsShadeFunction(const QString& function)
{
    return function == QStringLiteral("ShadeDefensiveSliceSampling") ||
           function == QStringLiteral("ShadeSliceSampling") || function == QStringLiteral("ShadeUniformArea");
}

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
    const QString root = document.GetRootGraph();
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

/// Parse one Slang float literal: JSON decimal syntax with an optional leading-zero-free fraction (`.5`, `1.`) and
/// an optional `f` suffix.
bool ParseLiteral(QString text, float32& outValue)
{
    text = text.trimmed();
    if (text.endsWith(QLatin1Char('f')) == true || text.endsWith(QLatin1Char('F')) == true)
    {
        text.chop(1);
    }
    const bool isNegative = text.startsWith(QLatin1Char('-'));
    QString magnitude = isNegative == true ? text.mid(1) : text;
    if (magnitude.startsWith(QLatin1Char('.')) == true)
    {
        magnitude.prepend(QLatin1Char('0'));
    }
    if (magnitude.endsWith(QLatin1Char('.')) == true)
    {
        magnitude.append(QLatin1Char('0'));
    }
    const QByteArray bytes = (isNegative == true ? QStringLiteral("-") : QString{}).append(magnitude).toLatin1();
    float32 value = 0;
    if (ParseFloat32(bytes.constData(), static_cast<usize>(bytes.size()), value) != NumberParseStatus::Success)
    {
        return false;
    }
    outValue = value;
    return true;
}

/// The literal arguments of the first uncommented `static const <type> <name> = <initializer>;` declaration.
bool ReadConstant(const QString& source, const QString& type, const QString& name, QStringList& outValues)
{
    const QRegularExpression declaration(
        QStringLiteral(R"(^[ \t]*static[ \t]+const[ \t]+%1[ \t]+%2[ \t]*=[ \t]*([^;\n]+);)")
            .arg(QRegularExpression::escape(type), QRegularExpression::escape(name)),
        QRegularExpression::MultilineOption);
    const QRegularExpressionMatch match = declaration.match(source);
    if (match.hasMatch() == false)
    {
        return false;
    }
    QString initializer = match.captured(1).trimmed();
    if (type == QStringLiteral("float3"))
    {
        const QString constructor = QStringLiteral("float3(");
        if (initializer.startsWith(constructor) == false || initializer.endsWith(QLatin1Char(')')) == false)
        {
            return false;
        }
        initializer = initializer.mid(constructor.size()).chopped(1);
    }
    outValues = initializer.split(QLatin1Char(','));
    return true;
}

bool ReadVector(const QString& source, const QString& name, math::Vector3& outVector)
{
    QStringList values;
    math::Vector3 vector{};
    if (ReadConstant(source, QStringLiteral("float3"), name, values) == false ||
        values.size() != VECTOR3_COMPONENT_COUNT || ParseLiteral(values[0], vector.X) == false ||
        ParseLiteral(values[1], vector.Y) == false || ParseLiteral(values[2], vector.Z) == false)
    {
        return false;
    }
    outVector = vector;
    return true;
}

QString FormatPoint(math::Vector3 point)
{
    return QStringLiteral("(%1, %2, %3)")
        .arg(point.X, 0, 'f', COORDINATE_DECIMALS)
        .arg(point.Y, 0, 'f', COORDINATE_DECIMALS)
        .arg(point.Z, 0, 'f', COORDINATE_DECIMALS);
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
float32 GetFrontDistance(const RenderGraphLuminairePanel& panel, const math::Aabb3& bounds)
{
    float32 front = math::Dot(bounds.Min - panel.CenterWorld, panel.NormalWorld);
    for (int32 corner = 1; corner < AABB_CORNER_COUNT; ++corner)
    {
        const math::Vector3 point{(corner & 1) != 0 ? bounds.Max.X : bounds.Min.X,
                                  (corner & 2) != 0 ? bounds.Max.Y : bounds.Min.Y,
                                  (corner & 4) != 0 ? bounds.Max.Z : bounds.Min.Z};
        front = math::Max(front, math::Dot(point - panel.CenterWorld, panel.NormalWorld));
    }
    return front;
}
} // namespace

bool FindRenderGraphLuminairePanel(const RenderGraphDocument& document,
                                   const QString& configuration,
                                   RenderGraphLuminairePanel& outPanel)
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
    if (FindShadeSource(document, programs, document.GetRootGraph(), configuration, 0, visited, shadeSource) == false)
    {
        return false;
    }
    RenderGraphLuminairePanel panel;
    panel.Source = shadeSource.chopped(QStringLiteral("shade.slang").size()) + QLatin1String(PLACEMENT_SOURCE);
    panel.IsPlacementKnown = ReadRenderGraphLuminairePlacement(document.GetDraft(panel.Source), panel);
    outPanel = panel;
    return true;
}

bool ReadRenderGraphLuminairePlacement(const QByteArray& source, RenderGraphLuminairePanel& outPanel)
{
    const QString text = QString::fromUtf8(source);
    math::Vector3 center{};
    math::Vector3 normal{};
    QStringList halfExtentValues;
    float32 halfExtent = 0;
    if (ReadVector(text, QStringLiteral("LUMINAIRE_CENTER_WORLD"), center) == false ||
        ReadVector(text, QStringLiteral("LUMINAIRE_NORMAL_WORLD"), normal) == false ||
        ReadConstant(text, QStringLiteral("float"), QStringLiteral("LUMINAIRE_HALF_EXTENT_WORLD"), halfExtentValues) ==
            false ||
        halfExtentValues.size() != 1 || ParseLiteral(halfExtentValues[0], halfExtent) == false ||
        halfExtent < MIN_HALF_EXTENT_WORLD || math::TryNormalize(normal, normal) != math::MathStatus::Success)
    {
        return false;
    }
    outPanel.CenterWorld = center;
    outPanel.HalfExtentWorld = halfExtent;
    outPanel.NormalWorld = normal;
    return true;
}

QString DescribeRenderGraphLuminaireCoverage(const RenderGraphLuminairePanel& panel,
                                             const RenderGraphLuminaireWorld& world)
{
    const bool hasWorldLighting = world.EmissiveObjectCount > 0 || world.LightCount > 0;
    QString reason;
    if (panel.IsPlacementKnown == false)
    {
        if (hasWorldLighting == false)
        {
            return {};
        }
        reason = QStringLiteral("its placement in %1 is not a set of plain literals, so the editor cannot check it")
                     .arg(panel.Source);
    }
    else if (world.HasBounds == true)
    {
        const math::Vector3 extent = world.BoundsWorld.Max - world.BoundsWorld.Min;
        const float32 diagonal = math::Length(extent);
        const float32 side = 2 * panel.HalfExtentWorld;
        if (GetFrontDistance(panel, world.BoundsWorld) <= FRONT_TOLERANCE_WORLD)
        {
            reason = QStringLiteral("the panel at %1 emits towards %2, away from every World object")
                         .arg(FormatPoint(panel.CenterWorld), FormatPoint(panel.NormalWorld));
        }
        else if (IsInside(world.BoundsWorld, panel.CenterWorld) == false)
        {
            reason = QStringLiteral("the panel at %1 lies outside the World's bounds %2 to %3")
                         .arg(FormatPoint(panel.CenterWorld),
                              FormatPoint(world.BoundsWorld.Min),
                              FormatPoint(world.BoundsWorld.Max));
        }
        else if (side < MIN_PANEL_WORLD_FRACTION * diagonal)
        {
            reason = QStringLiteral("the %1 m panel at %2 spans %3% of the World's %4 m bounds diagonal, so it lights "
                                    "at most a small patch")
                         .arg(side, 0, 'f', COORDINATE_DECIMALS)
                         .arg(FormatPoint(panel.CenterWorld))
                         .arg(PERCENT * side / diagonal, 0, 'f', 1)
                         .arg(diagonal, 0, 'f', 1);
        }
    }
    if (reason.isEmpty() == true)
    {
        return {};
    }
    QString text = QStringLiteral("The slice-sampling luminaire panel cannot light this World: %1.").arg(reason);
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
        text += QStringLiteral(" Only the panel lights surfaces in this template, not the World's %1.")
                    .arg(ignored.join(QStringLiteral(" or ")));
    }
    text += QStringLiteral(" Move the panel with LUMINAIRE_CENTER_WORLD, LUMINAIRE_HALF_EXTENT_WORLD and its frame in "
                           "%1, or route the output from a template that samples the World's emitters, such as "
                           "ReSTIR DI.")
                .arg(panel.Source);
    return text;
}
} // namespace ludus::editor
