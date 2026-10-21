#include "internal/render_graph_pipeline_node.h"

#include <QJsonArray>
#include <QStringList>

namespace ludus::editor
{
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
        return {QStringLiteral("compute")};
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
    }
    return {{QStringLiteral("id"), identity},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("inputs"), QJsonArray{}},
            {QStringLiteral("outputs"), QJsonArray{}},
            {QStringLiteral("shaders"), QJsonArray{}},
            {QStringLiteral("state"), state},
            {QStringLiteral("invocation"), invocation}};
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
