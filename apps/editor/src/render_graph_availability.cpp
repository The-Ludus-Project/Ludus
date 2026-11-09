#include "internal/render_graph_availability.h"
#include "internal/render_graph_canvas.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_pipeline_node.h"

#include <QJsonArray>
#include <QList>
#include <QSet>
#include <QStringList>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 MAX_NESTED_STEPS = 4096;
constexpr int32 MAX_NESTED_DEPTH = 64;
/// Nested node IDs named in a Subgraph explanation; the rest are counted.
constexpr qsizetype MAX_NAMED_NESTED_NODES = 3;

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

bool HasShaderStage(const QJsonObject& node, const QString& stage)
{
    for (const QJsonValueConstRef value : node.value(QStringLiteral("shaders")).toArray())
    {
        if (value.toObject().value(QStringLiteral("stage")).toString() == stage)
        {
            return true;
        }
    }
    return false;
}

/// Mirrors render_graph_ray_invocation.resolve_traversal; empty for a hardware pipeline.
QString ResolveTraversal(const QJsonObject& node)
{
    const QJsonObject invocation = node.value(QStringLiteral("invocation")).toObject();
    if (invocation.value(QStringLiteral("type")).toString(QStringLiteral("Compute")) == QStringLiteral("RayTracing"))
    {
        return {};
    }
    if (invocation.contains(QStringLiteral("type")) == true)
    {
        return invocation.value(QStringLiteral("traversal")).toString(QStringLiteral("TriangleLoop"));
    }
    // Older documents omitted the type; keep their compiler defaults.
    const bool isLegacyLoop = node.value(QStringLiteral("kind")).toString() == QStringLiteral("ComputePipeline") &&
                              HasShaderStage(node, QStringLiteral("compute")) == false;
    return invocation.value(QStringLiteral("traversal"))
        .toString(isLegacyLoop == true ? QStringLiteral("TriangleLoop") : QStringLiteral("NativeQuery"));
}

bool IsGraphicsStage(const QString& stage)
{
    return stage == QStringLiteral("vertex") || stage == QStringLiteral("fragment");
}

bool HasCapability(const RenderGraphDeviceProfile& profile, RenderGraphRequirement requirement)
{
    switch (requirement)
    {
        case RenderGraphRequirement::None:
            return true;
        case RenderGraphRequirement::Denoiser:
            return profile.HasDenoiser;
        case RenderGraphRequirement::Compute:
            return profile.HasCompute;
        case RenderGraphRequirement::NativeRayQueries:
            return profile.HasNativeRayQueries;
        case RenderGraphRequirement::RayPipelines:
            return profile.HasNativeRayQueries == true && profile.HasRayPipelines == true;
        case RenderGraphRequirement::BeyondProfile:
            return false;
    }
    return false;
}

QString DescribeDevice(const RenderGraphDeviceProfile& profile)
{
    return profile.DeviceName.isEmpty() == true ? QStringLiteral("this GPU")
                                                : QStringLiteral("this %1 GPU").arg(profile.DeviceName);
}

/// Label, cause and repair for one node's own settings.
RenderGraphAvailabilityFeedback DescribeNode(const QJsonObject& node, const RenderGraphDeviceProfile& profile)
{
    RenderGraphAvailabilityFeedback feedback;
    feedback.Requirement = GetRenderGraphNodeRequirement(node);
    const QString kind = node.value(QStringLiteral("kind")).toString();
    if (feedback.Requirement == RenderGraphRequirement::BeyondProfile)
    {
        feedback.Availability = RenderGraphAvailability::Unsupported;
        feedback.Label = QStringLiteral("Not supported yet");
        if (kind == QStringLiteral("ComputePipeline"))
        {
            feedback.Repair = RenderGraphAvailabilityRepair::AddSceneInputs;
            feedback.Explanation = QStringLiteral(
                "Not supported yet: the graph build runs a ComputePipeline only as a scene compute pass, so Build "
                "reports a capability error for this node on every device. Fix: right-click › Add Scene and View "
                "Inputs, or keep it as an unused draft.");
        }
        else
        {
            feedback.Explanation = QStringLiteral(
                "Not supported yet: the graph build runs a GraphicsPipeline only with vertex and fragment shaders and "
                "the RendererStatic vertex layout, so Build reports a capability error for this node on every device. "
                "Fix: remove the other stages in Shaders and select the RendererStatic vertex layout.");
        }
        return feedback;
    }
    if (profile.Kind == RenderGraphDeviceKind::Unknown || feedback.Requirement == RenderGraphRequirement::None)
    {
        return feedback;
    }
    if (profile.Kind == RenderGraphDeviceKind::Browser)
    {
        feedback.Availability = RenderGraphAvailability::Unavailable;
        feedback.Label = QStringLiteral("Desktop editor only");
        feedback.Explanation =
            feedback.Requirement == RenderGraphRequirement::Denoiser
                ? QStringLiteral("Unavailable in the browser editor: denoising runs only in the desktop editor and "
                                 "native games.")
                : QStringLiteral("Unavailable in the browser editor: ray tracing and compute passes run only on "
                                 "desktop Metal or Vulkan devices. Open the project in the desktop editor to preview "
                                 "this pass.");
        return feedback;
    }
    if (HasCapability(profile, feedback.Requirement) == true)
    {
        return feedback;
    }
    feedback.Availability = RenderGraphAvailability::Unavailable;
    const QString device = DescribeDevice(profile);
    const bool canUseComputeBvh = profile.HasCompute == true;
    const QString computeRepair = QStringLiteral(
        " Fix: right-click › Use Compute BVH Traversal… to run the same callbacks in ordinary GPU compute, or preview "
        "on a GPU with hardware ray tracing.");
    switch (feedback.Requirement)
    {
        case RenderGraphRequirement::Denoiser:
            feedback.Label = QStringLiteral("Bypassed · no denoiser");
            feedback.Explanation = QStringLiteral(
                "Bypassed: this editor was set up without the optional OIDN denoiser, so the preview skips this node "
                "and shows the unfiltered image. Fix: run init with --with-denoiser (Include OIDN denoiser) and "
                "rebuild the editor.");
            break;
        case RenderGraphRequirement::Compute:
            feedback.Label = QStringLiteral("Needs GPU compute");
            feedback.Explanation =
                QStringLiteral("Unavailable on %1: this pass traverses the scene in GPU compute with storage buffers, "
                               "which this device does not provide, so the viewport uses the raster graph instead. "
                               "Preview on a GPU with compute support.")
                    .arg(device);
            break;
        case RenderGraphRequirement::NativeRayQueries:
            feedback.Label = QStringLiteral("Needs RT hardware");
            feedback.Explanation =
                QStringLiteral("Unavailable on %1: this pass uses native ray queries, which are not enabled here, so "
                               "the viewport uses the raster graph instead.")
                    .arg(device) +
                (canUseComputeBvh == true ? computeRepair
                                          : QStringLiteral(" Preview on a GPU with hardware ray tracing."));
            break;
        case RenderGraphRequirement::RayPipelines:
            feedback.Label = QStringLiteral("Needs RT pipeline");
            feedback.Explanation =
                QStringLiteral("Unavailable on %1: this pass uses the hardware RT pipeline (native ray stages and "
                               "function tables), which is not available here, so the viewport uses the raster graph "
                               "instead.")
                    .arg(device) +
                (canUseComputeBvh == true ? computeRepair
                                          : QStringLiteral(" Preview on a GPU with hardware ray tracing."));
            break;
        case RenderGraphRequirement::None:
        case RenderGraphRequirement::BeyondProfile:
            break;
    }
    feedback.Repair = canUseComputeBvh == true && (feedback.Requirement == RenderGraphRequirement::NativeRayQueries ||
                                                   feedback.Requirement == RenderGraphRequirement::RayPipelines)
                          ? RenderGraphAvailabilityRepair::UseComputeBvh
                          : RenderGraphAvailabilityRepair::None;
    return feedback;
}

/// One unavailable node inside an imported configuration.
struct NestedProblem final
{
    QString Path;
    RenderGraphAvailabilityFeedback Feedback;
};

/// Bounded, cycle-safe walk of a Subgraph's imports. No state survives one inspection.
class NestedInspector final
{
public:
    NestedInspector(const RenderGraphDocument& document, const RenderGraphDeviceProfile& profile)
        : mDocument(document), mProfile(profile)
    {
    }
    void Inspect(const QJsonObject& instance, const QString& path, int32 depth, QVector<NestedProblem>& inoutProblems)
    {
        const QString key = instance.value(QStringLiteral("asset")).toString() + QLatin1Char('#') +
                            instance.value(QStringLiteral("configuration")).toString();
        if (--mRemainingSteps <= 0 || depth >= MAX_NESTED_DEPTH || mVisiting.contains(key) == true)
        {
            return;
        }
        const QJsonObject record = FindConfiguration(mDocument,
                                                     instance.value(QStringLiteral("asset")).toString(),
                                                     instance.value(QStringLiteral("configuration")).toString());
        mVisiting.insert(key);
        for (const QJsonValueConstRef value : record.value(QStringLiteral("nodes")).toArray())
        {
            const QJsonObject node = value.toObject();
            if (IsRenderGraphNodeEnabled(node) == false)
            {
                continue;
            }
            const QString nested = path + QLatin1Char('/') + node.value(QStringLiteral("id")).toString();
            if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Subgraph"))
            {
                Inspect(node, nested, depth + 1, inoutProblems);
                continue;
            }
            const RenderGraphAvailabilityFeedback feedback = DescribeNode(node, mProfile);
            if (feedback.Availability != RenderGraphAvailability::Available)
            {
                inoutProblems.append({nested, feedback});
            }
        }
        mVisiting.remove(key);
    }

private:
    const RenderGraphDocument& mDocument;
    const RenderGraphDeviceProfile& mProfile;
    QSet<QString> mVisiting;
    int32 mRemainingSteps = MAX_NESTED_STEPS;
};

RenderGraphAvailabilityFeedback DescribeSubgraph(const QVector<NestedProblem>& problems)
{
    RenderGraphAvailabilityFeedback feedback;
    if (problems.isEmpty() == true)
    {
        return feedback;
    }
    bool isDeviceLimited = false;
    QStringList named;
    for (const NestedProblem& problem : problems)
    {
        isDeviceLimited =
            isDeviceLimited == true || problem.Feedback.Availability == RenderGraphAvailability::Unavailable;
        if (named.size() < MAX_NAMED_NESTED_NODES)
        {
            named.append(QStringLiteral("%1 (%2)").arg(problem.Path, problem.Feedback.Label));
        }
        if (static_cast<uint8>(problem.Feedback.Requirement) > static_cast<uint8>(feedback.Requirement))
        {
            feedback.Requirement = problem.Feedback.Requirement;
        }
    }
    feedback.Availability =
        isDeviceLimited == true ? RenderGraphAvailability::Unavailable : RenderGraphAvailability::Unsupported;
    feedback.Label = problems.size() == 1 ? QStringLiteral("1 nested node can't run")
                                          : QStringLiteral("%1 nested nodes can't run").arg(problems.size());
    feedback.Explanation =
        QStringLiteral("This subgraph contains nodes that cannot run here: %1").arg(named.join(QStringLiteral(", ")));
    if (problems.size() > named.size())
    {
        feedback.Explanation += QStringLiteral(" and %1 more").arg(problems.size() - named.size());
    }
    feedback.Explanation += QStringLiteral(". Open Subgraph to see each node's cause and repair.");
    return feedback;
}

QString DescribeSummary(const RenderGraphConfigurationAvailability& availability,
                        const RenderGraphDeviceProfile& profile)
{
    if (profile.Kind == RenderGraphDeviceKind::Browser)
    {
        return QStringLiteral("Render graphs do not run in the browser editor; the viewport shows its built-in "
                              "renderer. Open the project in the desktop editor to preview this graph.");
    }
    if (availability.UnavailableCount == 0)
    {
        return {};
    }
    bool hasDevice = false;
    bool hasDenoiser = false;
    bool hasProfile = false;
    for (const RenderGraphAvailabilityFeedback& feedback : availability.Nodes)
    {
        hasProfile = hasProfile == true || feedback.Availability == RenderGraphAvailability::Unsupported;
        hasDenoiser = hasDenoiser == true || (feedback.Availability == RenderGraphAvailability::Unavailable &&
                                              feedback.Requirement == RenderGraphRequirement::Denoiser);
        hasDevice = hasDevice == true || (feedback.Availability == RenderGraphAvailability::Unavailable &&
                                          feedback.Requirement != RenderGraphRequirement::Denoiser);
    }
    QString summary = availability.UnavailableCount == 1
                          ? QStringLiteral("1 node cannot run on %1.").arg(DescribeDevice(profile))
                          : QStringLiteral("%1 nodes cannot run on %2.")
                                .arg(availability.UnavailableCount)
                                .arg(DescribeDevice(profile));
    if (profile.Kind == RenderGraphDeviceKind::Unknown)
    {
        summary = availability.UnavailableCount == 1
                      ? QStringLiteral("1 node uses settings the graph build does not support yet.")
                      : QStringLiteral("%1 nodes use settings the graph build does not support yet.")
                            .arg(availability.UnavailableCount);
    }
    if (hasDevice == true)
    {
        summary += QStringLiteral(" The viewport uses the raster graph instead.");
    }
    if (hasDenoiser == true)
    {
        summary += QStringLiteral(" The preview skips Denoise nodes.");
    }
    if (hasProfile == true)
    {
        summary += QStringLiteral(" Build reports a capability error until those settings change.");
    }
    return summary + QStringLiteral(" Hover a greyed node for its cause and fix.");
}
} // namespace

RenderGraphRequirement GetRenderGraphNodeRequirement(const QJsonObject& node)
{
    const QString kind = node.value(QStringLiteral("kind")).toString();
    if (kind == QStringLiteral("Denoise"))
    {
        return RenderGraphRequirement::Denoiser;
    }
    if (kind == QStringLiteral("GraphicsPipeline"))
    {
        for (const QJsonValueConstRef value : node.value(QStringLiteral("shaders")).toArray())
        {
            if (IsGraphicsStage(value.toObject().value(QStringLiteral("stage")).toString()) == false)
            {
                return RenderGraphRequirement::BeyondProfile;
            }
        }
        const QJsonValue layout =
            node.value(QStringLiteral("invocation")).toObject().value(QStringLiteral("vertex_layout"));
        return layout.isUndefined() == false && layout.toString() != QStringLiteral("RendererStatic")
                   ? RenderGraphRequirement::BeyondProfile
                   : RenderGraphRequirement::None;
    }
    if (kind == QStringLiteral("ComputePipeline") && IsSceneComputeNode(node) == false)
    {
        return RenderGraphRequirement::BeyondProfile;
    }
    if (UsesRayLaunch(node) == false)
    {
        return RenderGraphRequirement::None;
    }
    const QString traversal = ResolveTraversal(node);
    return traversal.isEmpty() == true                  ? RenderGraphRequirement::RayPipelines
           : traversal == QStringLiteral("NativeQuery") ? RenderGraphRequirement::NativeRayQueries
                                                        : RenderGraphRequirement::Compute;
}

RenderGraphConfigurationAvailability InspectRenderGraphAvailability(const RenderGraphDocument& document,
                                                                    const QString& file,
                                                                    const QString& configuration,
                                                                    const RenderGraphDeviceProfile& profile)
{
    RenderGraphConfigurationAvailability availability;
    const QJsonArray nodes = FindConfiguration(document, file, configuration).value(QStringLiteral("nodes")).toArray();
    if (nodes.size() > RenderGraphDocument::MAX_NODES)
    {
        return availability;
    }
    NestedInspector nested(document, profile);
    for (const QJsonValueConstRef value : nodes)
    {
        const QJsonObject node = value.toObject();
        if (IsRenderGraphNodeEnabled(node) == false)
        {
            continue;
        }
        RenderGraphAvailabilityFeedback feedback;
        if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Subgraph"))
        {
            QVector<NestedProblem> problems;
            nested.Inspect(node, node.value(QStringLiteral("id")).toString(), 0, problems);
            feedback = DescribeSubgraph(problems);
        }
        else
        {
            feedback = DescribeNode(node, profile);
        }
        if (feedback.Availability != RenderGraphAvailability::Available)
        {
            ++availability.UnavailableCount;
            availability.Nodes.insert(node.value(QStringLiteral("id")).toString(), feedback);
        }
    }
    availability.Summary = DescribeSummary(availability, profile);
    return availability;
}

bool MakeComputeBvhRayNode(const QJsonObject& node, QJsonObject& outNode)
{
    const RenderGraphRequirement requirement = GetRenderGraphNodeRequirement(node);
    if (UsesRayLaunch(node) == false || (requirement != RenderGraphRequirement::NativeRayQueries &&
                                         requirement != RenderGraphRequirement::RayPipelines))
    {
        return false;
    }
    QJsonObject candidate = node;
    QJsonObject invocation = candidate.value(QStringLiteral("invocation")).toObject();
    invocation.insert(QStringLiteral("type"), QStringLiteral("Compute"));
    invocation.insert(QStringLiteral("traversal"), QStringLiteral("ComputeBvh"));
    invocation.remove(QStringLiteral("traversal_source"));
    candidate.insert(QStringLiteral("invocation"), invocation);
    outNode = candidate;
    return true;
}
} // namespace ludus::editor
