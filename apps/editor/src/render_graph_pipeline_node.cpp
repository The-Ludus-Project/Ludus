#include "internal/render_graph_pipeline_node.h"

#include <ludus/foundation/base/types.h>

#include <QJsonArray>
#include <QStringList>

namespace ludus::editor
{
namespace
{
// The builder's render_graph_compute.GROUP_SIZE: the persistent ray wrapper's workgroup width.
constexpr foundation::int32 SCENE_COMPUTE_GROUP_WIDTH = LUDUS_EDITOR_SCENE_COMPUTE_GROUP_WIDTH;

bool HasInputType(const QJsonObject& node, const QString& type)
{
    for (const QJsonValueConstRef input : node.value(QStringLiteral("inputs")).toArray())
    {
        if (input.toObject().value(QStringLiteral("type")).toString() == type)
        {
            return true;
        }
    }
    return false;
}

QString MakeUniqueInputIdentity(const QJsonObject& node, const QString& base)
{
    QStringList identities;
    for (const QString& direction : {QStringLiteral("inputs"), QStringLiteral("outputs")})
    {
        for (const QJsonValueConstRef port : node.value(direction).toArray())
        {
            identities.append(port.toObject().value(QStringLiteral("id")).toString());
        }
    }
    QString identity = base;
    for (foundation::int32 suffix = 2; identities.contains(identity) == true; ++suffix)
    {
        identity = base + QString::number(suffix);
    }
    return identity;
}
} // namespace

bool IsPipelineNode(const QString& kind) noexcept
{
    return kind == QStringLiteral("GraphicsPipeline") || kind == QStringLiteral("ComputePipeline") ||
           kind == QStringLiteral("RayTracingPipeline");
}

QStringList GetPipelineShaderStages(const QString& kind)
{
    if (kind == QStringLiteral("GraphicsPipeline"))
    {
        return {QStringLiteral("vertex"),
                QStringLiteral("fragment"),
                QStringLiteral("geometry"),
                QStringLiteral("hull"),
                QStringLiteral("domain")};
    }
    if (kind == QStringLiteral("ComputePipeline"))
    {
        return {QStringLiteral("compute"),
                QStringLiteral("ray_generation"),
                QStringLiteral("miss"),
                QStringLiteral("closest_hit")};
    }
    if (kind == QStringLiteral("RayTracingPipeline"))
    {
        return {QStringLiteral("ray_generation"),
                QStringLiteral("miss"),
                QStringLiteral("closest_hit"),
                QStringLiteral("any_hit"),
                QStringLiteral("intersection"),
                QStringLiteral("callable")};
    }
    return {};
}

QJsonObject MakePipelineNode(const QString& kind, const QString& identity)
{
    QJsonObject state{{QStringLiteral("group_size"), QJsonArray{1, 1, 1}}};
    QJsonObject invocation{{QStringLiteral("dimensions"), QJsonArray{1, 1, 1}}};
    if (kind == QStringLiteral("GraphicsPipeline"))
    {
        state = {{QStringLiteral("topology"), QStringLiteral("TriangleList")},
                 {QStringLiteral("cull"), QStringLiteral("None")},
                 {QStringLiteral("front"), QStringLiteral("CounterClockwise")},
                 {QStringLiteral("depth_compare"), QStringLiteral("Less")},
                 {QStringLiteral("depth_write"), true},
                 {QStringLiteral("blend"), QStringLiteral("Opaque")},
                 {QStringLiteral("samples"), 1}};
        invocation = {{QStringLiteral("mode"), QStringLiteral("ColorAndDepth")},
                      {QStringLiteral("vertex_layout"), QStringLiteral("RendererStatic")},
                      {QStringLiteral("material"), QStringLiteral("DefaultOpaque")},
                      {QStringLiteral("scene"), QStringLiteral("scene")},
                      {QStringLiteral("view"), QStringLiteral("view")}};
    }
    else if (kind == QStringLiteral("RayTracingPipeline"))
    {
        // Omitted limits are derived by the selected execution profile.
        state = {};
        invocation = {{QStringLiteral("dimensions"), QStringLiteral("ColorTarget")}};
    }
    return {{QStringLiteral("id"), identity},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("inputs"), QJsonArray{}},
            {QStringLiteral("outputs"), QJsonArray{}},
            {QStringLiteral("shaders"), QJsonArray{}},
            {QStringLiteral("state"), state},
            {QStringLiteral("invocation"), invocation}};
}

bool IsSceneComputeNode(const QJsonObject& node)
{
    return node.value(QStringLiteral("kind")).toString() == QStringLiteral("ComputePipeline") &&
           HasInputType(node, QStringLiteral("Scene")) == true;
}

bool UsesRayLaunch(const QJsonObject& node)
{
    return node.value(QStringLiteral("kind")).toString() == QStringLiteral("RayTracingPipeline") ||
           IsSceneComputeNode(node) == true;
}

QJsonArray GetSceneComputeGroupSize()
{
    return QJsonArray{SCENE_COMPUTE_GROUP_WIDTH, 1, 1};
}

QJsonObject ApplyComputeProfile(const QJsonObject& previousNode, const QJsonObject& node)
{
    const bool isSceneCompute = IsSceneComputeNode(node);
    if (node.value(QStringLiteral("kind")).toString() != QStringLiteral("ComputePipeline") ||
        IsSceneComputeNode(previousNode) == isSceneCompute)
    {
        return node;
    }
    QJsonObject result = node;
    if (isSceneCompute == true)
    {
        // A generic group size or dispatch count has no meaning for a per-pixel ray launch.
        result.insert(QStringLiteral("state"), QJsonObject{});
        result.insert(QStringLiteral("invocation"),
                      QJsonObject{{QStringLiteral("dimensions"), QStringLiteral("ColorTarget")}});
        return result;
    }
    QJsonObject state = result.value(QStringLiteral("state")).toObject();
    if (state.contains(QStringLiteral("group_size")) == false)
    {
        state.insert(QStringLiteral("group_size"), QJsonArray{1, 1, 1});
    }
    QJsonObject invocation = result.value(QStringLiteral("invocation")).toObject();
    if (invocation.value(QStringLiteral("dimensions")).isArray() == false)
    {
        invocation.insert(QStringLiteral("dimensions"), QJsonArray{1, 1, 1});
    }
    result.insert(QStringLiteral("state"), state);
    result.insert(QStringLiteral("invocation"), invocation);
    return result;
}

bool MakeSceneComputeNode(const QJsonObject& node, QJsonObject& outNode)
{
    if (node.value(QStringLiteral("kind")).toString() != QStringLiteral("ComputePipeline") ||
        IsSceneComputeNode(node) == true)
    {
        return false;
    }
    QJsonObject candidate = node;
    QJsonArray inputs = candidate.value(QStringLiteral("inputs")).toArray();
    // Scene and View lead, matching the ray pipeline contract's port order.
    if (HasInputType(node, QStringLiteral("View")) == false)
    {
        inputs.prepend(QJsonObject{{QStringLiteral("id"), MakeUniqueInputIdentity(node, QStringLiteral("view"))},
                                   {QStringLiteral("type"), QStringLiteral("View")},
                                   {QStringLiteral("required"), true}});
    }
    inputs.prepend(QJsonObject{{QStringLiteral("id"), MakeUniqueInputIdentity(node, QStringLiteral("scene"))},
                               {QStringLiteral("type"), QStringLiteral("Scene")},
                               {QStringLiteral("required"), true}});
    candidate.insert(QStringLiteral("inputs"), inputs);
    outNode = ApplyComputeProfile(node, candidate);
    return true;
}

bool MigrateRasterNode(const QJsonObject& node, const QJsonObject& manifest, QJsonObject& outNode)
{
    if (node.value(QStringLiteral("kind")).toString() != QStringLiteral("Raster"))
    {
        return false;
    }
    const QString material = node.value(QStringLiteral("material")).toString();
    if (material != QStringLiteral("DefaultOpaque") && material != QStringLiteral("DefaultMasked"))
    {
        return false;
    }
    const QString mode = node.value(QStringLiteral("mode")).toString();
    const QString techniqueId = (material == QStringLiteral("DefaultMasked") ? QStringLiteral("DefaultStaticMasked.")
                                                                             : QStringLiteral("DefaultStaticOpaque.")) +
                                mode;
    QJsonObject technique;
    const QJsonArray techniques = manifest.value(QStringLiteral("techniques")).toArray();
    for (const QJsonValueConstRef value : techniques)
    {
        if (value.toObject().value(QStringLiteral("id")).toString() == techniqueId)
        {
            if (technique.isEmpty() == false)
            {
                return false;
            }
            technique = value.toObject();
        }
    }
    const QJsonArray recipes = technique.value(QStringLiteral("recipes")).toArray();
    if (recipes.size() != 1 || (mode != QStringLiteral("ColorAndDepth") && mode != QStringLiteral("DepthOnly")))
    {
        return false;
    }
    QJsonObject candidate =
        MakePipelineNode(QStringLiteral("GraphicsPipeline"), node.value(QStringLiteral("id")).toString());
    QJsonObject invocation = candidate.value(QStringLiteral("invocation")).toObject();
    for (const QString& key : {QStringLiteral("mode"), QStringLiteral("vertex_layout"), QStringLiteral("material")})
    {
        invocation.insert(key, node.value(key));
    }
    candidate.insert(QStringLiteral("invocation"), invocation);
    QJsonObject state = candidate.value(QStringLiteral("state")).toObject();
    const QJsonObject recipe = recipes.at(0).toObject();
    for (QJsonObject::iterator field = state.begin(); field != state.end(); ++field)
    {
        if (recipe.contains(field.key()) == false)
        {
            return false;
        }
        field.value() = recipe.value(field.key());
    }
    candidate.insert(QStringLiteral("state"), state);
    QJsonArray inputs{QJsonObject{{QStringLiteral("id"), QStringLiteral("scene")},
                                  {QStringLiteral("type"), QStringLiteral("Scene")},
                                  {QStringLiteral("required"), true}},
                      QJsonObject{{QStringLiteral("id"), QStringLiteral("view")},
                                  {QStringLiteral("type"), QStringLiteral("View")},
                                  {QStringLiteral("required"), true}}};
    QJsonArray outputs;
    const QStringList aspects = mode == QStringLiteral("ColorAndDepth")
                                    ? QStringList{QStringLiteral("depth"), QStringLiteral("color")}
                                    : QStringList{QStringLiteral("depth")};
    for (const QString& aspect : aspects)
    {
        const QString type =
            aspect == QStringLiteral("color") ? QStringLiteral("ColorTexture") : QStringLiteral("DepthTexture");
        inputs.append(QJsonObject{{QStringLiteral("id"), aspect},
                                  {QStringLiteral("type"), type},
                                  {QStringLiteral("required"), true}});
        outputs.append(QJsonObject{
            {QStringLiteral("id"), aspect},
            {QStringLiteral("type"), type},
            {QStringLiteral("target"), aspect},
            {QStringLiteral("format"),
             aspect == QStringLiteral("color") ? QStringLiteral("RGBA8Unorm") : QStringLiteral("Depth32Float")},
            {QStringLiteral("load"), node.value(aspect + QStringLiteral("_load"))},
            {QStringLiteral("store"), node.value(aspect + QStringLiteral("_store"))}});
    }
    candidate.insert(QStringLiteral("inputs"), inputs);
    candidate.insert(QStringLiteral("outputs"), outputs);
    QJsonArray shaders;
    for (const QString& stage : {QStringLiteral("vertex"), QStringLiteral("fragment")})
    {
        if (technique.value(stage).toString().isEmpty() == true)
        {
            return false;
        }
        shaders.append(QJsonObject{{QStringLiteral("id"), stage},
                                   {QStringLiteral("stage"), stage},
                                   {QStringLiteral("program"), technique.value(stage)}});
    }
    candidate.insert(QStringLiteral("shaders"), shaders);
    outNode = candidate;
    return true;
}

} // namespace ludus::editor
