#include "internal/render_graph_pipeline_node.h"
#include "internal/render_graph_shader_creation.h"

#include <QJsonDocument>

namespace ludus::editor
{
const foundation::int32 RenderGraphSceneQueryBudgets::MIN_SURFACE_QUERIES = LUDUS_EDITOR_MIN_SURFACE_QUERIES;
const foundation::int32 RenderGraphSceneQueryBudgets::MAX_SURFACE_QUERIES = LUDUS_EDITOR_MAX_SURFACE_QUERIES;
const foundation::int32 RenderGraphSceneQueryBudgets::MIN_VISIBILITY_QUERIES = LUDUS_EDITOR_MIN_VISIBILITY_QUERIES;
const foundation::int32 RenderGraphSceneQueryBudgets::MAX_VISIBILITY_QUERIES = LUDUS_EDITOR_MAX_VISIBILITY_QUERIES;

namespace
{
const QString RAY_TECHNIQUE = QStringLiteral("RendererTriangles.RayTracing");
const QStringList RAY_STAGES{QStringLiteral("ray_generation"), QStringLiteral("miss"), QStringLiteral("closest_hit")};

QJsonObject MakeSceneInterface()
{
    const RenderGraphSceneQueryBudgets budgets;
    return QJsonObject{{QStringLiteral("version"), 1},
                       {QStringLiteral("surface_queries"), budgets.SurfaceQueries},
                       {QStringLiteral("visibility_queries"), budgets.VisibilityQueries}};
}

qsizetype FindSceneRayTechnique(const QJsonArray& techniques)
{
    qsizetype result = -1;
    for (qsizetype index = 0; index < techniques.size(); ++index)
    {
        if (techniques.at(index).toObject().value(QStringLiteral("id")) == RAY_TECHNIQUE)
        {
            if (result >= 0)
            {
                return -1;
            }
            result = index;
        }
    }
    return result;
}

bool AreSceneQueryBudgetsValid(const RenderGraphSceneQueryBudgets& budgets)
{
    return budgets.SurfaceQueries >= RenderGraphSceneQueryBudgets::MIN_SURFACE_QUERIES &&
           budgets.SurfaceQueries <= RenderGraphSceneQueryBudgets::MAX_SURFACE_QUERIES &&
           budgets.VisibilityQueries >= RenderGraphSceneQueryBudgets::MIN_VISIBILITY_QUERIES &&
           budgets.VisibilityQueries <= RenderGraphSceneQueryBudgets::MAX_VISIBILITY_QUERIES;
}

} // namespace

bool RenderGraphDocument::GetSceneQueryBudgets(RenderGraphSceneQueryBudgets& outBudgets) const
{
    const QString manifestFile =
        mRootGraph.chopped(QStringLiteral(".lurender.json").size()) + QStringLiteral(".lushader.json");
    const QJsonObject manifest = QJsonDocument::fromJson(GetDraft(manifestFile)).object();
    const QJsonArray techniques = manifest.value(QStringLiteral("techniques")).toArray();
    const qsizetype index = FindSceneRayTechnique(techniques);
    if (index < 0)
    {
        return false;
    }
    const QJsonObject interface = techniques.at(index).toObject().value(QStringLiteral("scene_interface")).toObject();
    const QJsonValue surface = interface.value(QStringLiteral("surface_queries"));
    const QJsonValue visibility = interface.value(QStringLiteral("visibility_queries"));
    const RenderGraphSceneQueryBudgets budgets
    {
        .SurfaceQueries = surface.toInt(),
        .VisibilityQueries = visibility.toInt(),
    };
    if (interface.value(QStringLiteral("version")) != QJsonValue(1) || surface.isDouble() == false ||
        visibility.isDouble() == false ||
        surface.toDouble() != static_cast<foundation::float64>(budgets.SurfaceQueries) ||
        visibility.toDouble() != static_cast<foundation::float64>(budgets.VisibilityQueries) ||
        AreSceneQueryBudgetsValid(budgets) == false)
    {
        return false;
    }
    outBudgets = budgets;
    return true;
}

RenderGraphDocumentStatus RenderGraphDocument::SetSceneQueryBudgets(const RenderGraphSceneQueryBudgets& budgets,
                                                                    QString& outError)
{
    if (AreSceneQueryBudgetsValid(budgets) == false)
    {
        outError =
            QStringLiteral("Surface queries must be %1–%2 and visibility queries %3–%4. The draft was preserved.")
                .arg(RenderGraphSceneQueryBudgets::MIN_SURFACE_QUERIES)
                .arg(RenderGraphSceneQueryBudgets::MAX_SURFACE_QUERIES)
                .arg(RenderGraphSceneQueryBudgets::MIN_VISIBILITY_QUERIES)
                .arg(RenderGraphSceneQueryBudgets::MAX_VISIBILITY_QUERIES);
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    const QString manifestFile =
        mRootGraph.chopped(QStringLiteral(".lurender.json").size()) + QStringLiteral(".lushader.json");
    QJsonObject manifest = QJsonDocument::fromJson(GetDraft(manifestFile)).object();
    QJsonArray techniques = manifest.value(QStringLiteral("techniques")).toArray();
    const qsizetype index = FindSceneRayTechnique(techniques);
    QJsonObject technique = index >= 0 ? techniques.at(index).toObject() : QJsonObject();
    QJsonObject interface = technique.value(QStringLiteral("scene_interface")).toObject();
    if (index < 0 || interface.value(QStringLiteral("version")) != QJsonValue(1))
    {
        outError = QStringLiteral("Query budgets require one scene-aware ray technique. Enable Scene Materials first; "
                                  "repair an invalid or duplicate technique in Source. The draft was preserved.");
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    if (interface.value(QStringLiteral("surface_queries")) == QJsonValue(budgets.SurfaceQueries) &&
        interface.value(QStringLiteral("visibility_queries")) == QJsonValue(budgets.VisibilityQueries))
    {
        outError.clear();
        return RenderGraphDocumentStatus::Ok;
    }
    interface.insert(QStringLiteral("surface_queries"), budgets.SurfaceQueries);
    interface.insert(QStringLiteral("visibility_queries"), budgets.VisibilityQueries);
    technique.insert(QStringLiteral("scene_interface"), interface);
    techniques.replace(index, technique);
    manifest.insert(QStringLiteral("techniques"), techniques);
    RenderGraphSnapshot candidate = mDraft;
    candidate.Files.insert(manifestFile, EncodeCanonicalJson(manifest));
    mLastMove.clear();
    const RenderGraphDocumentStatus status = commit(candidate, false);
    outError = status == RenderGraphDocumentStatus::Ok
                   ? QString()
                   : QStringLiteral("Query budgets exceed the document's source limits. The draft was preserved.");
    return status;
}

bool HasSceneRayInterface(const QJsonObject& manifest)
{
    for (const QJsonValueConstRef value : manifest.value(QStringLiteral("techniques")).toArray())
    {
        const QJsonObject technique = value.toObject();
        if (technique.value(QStringLiteral("id")).toString() == RAY_TECHNIQUE)
        {
            return technique.value(QStringLiteral("scene_interface")).isObject();
        }
    }
    return false;
}

QByteArray MakeSceneRaySource(QJsonObject& inoutProgram)
{
    const QString stage = inoutProgram.value(QStringLiteral("stage")).toString();
    const QString function = inoutProgram.value(QStringLiteral("function")).toString();
    QString source = QStringLiteral("// Edit this shader, then Save to publish it to the project.\n"
                                    "// Scene interface types are supplied by the shader manifest.\n");
    if (stage == QStringLiteral("ray_generation"))
    {
        inoutProgram.insert(QStringLiteral("input"), QStringLiteral("RayLaunch"));
        inoutProgram.insert(QStringLiteral("output"), QStringLiteral("Radiance"));
        source += QStringLiteral("float3 %1(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)\n"
                                 "{\n    ScenePayload payload = (ScenePayload)0;\n"
                                 "    TraceAndShade(scene, queries, launch.CameraRay, payload);\n"
                                 "    return payload.Radiance;\n}\n")
                      .arg(function);
    }
    else if (stage == QStringLiteral("closest_hit"))
    {
        inoutProgram.insert(QStringLiteral("input"), QStringLiteral("SurfaceHit"));
        inoutProgram.insert(QStringLiteral("output"), QStringLiteral("ScenePayload"));
        source += QStringLiteral("void %1(SceneContext scene, SurfaceHit hit, inout RayQueryContext queries, "
                                 "inout ScenePayload payload)\n{\n"
                                 "    // MaterialSample exposes BaseColor, Metallic, Roughness and Emission.\n"
                                 "    MaterialSample material = EvaluateMaterial(scene, hit);\n"
                                 "    // Display material color; author lighting here when needed.\n"
                                 "    payload.Radiance = material.BaseColor;\n}\n")
                      .arg(function);
    }
    else if (stage == QStringLiteral("compute"))
    {
        // C03 scene compute kernel: the ray-generation signature, without hit or miss callbacks.
        inoutProgram.insert(QStringLiteral("input"), QStringLiteral("RayLaunch"));
        inoutProgram.insert(QStringLiteral("output"), QStringLiteral("Radiance"));
        source += QStringLiteral("float3 %1(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)\n"
                                 "{\n"
                                 "    // A compute pass has no hit or miss callbacks: TraceAndShade fails here.\n"
                                 "    SurfaceHit hit;\n"
                                 "    if (TraceClosest(scene, queries, launch.CameraRay, hit) != SCENE_QUERY_SURFACE)\n"
                                 "    {\n        return float3(0, 0, 0);\n    }\n"
                                 "    // MaterialSample exposes BaseColor, Metallic, Roughness and Emission.\n"
                                 "    MaterialSample material = EvaluateMaterial(scene, hit);\n"
                                 "    // Display material color; author lighting here when needed.\n"
                                 "    return material.BaseColor;\n}\n")
                      .arg(function);
    }
    else if (stage == QStringLiteral("miss"))
    {
        inoutProgram.insert(QStringLiteral("input"), QStringLiteral("Ray"));
        inoutProgram.insert(QStringLiteral("output"), QStringLiteral("ScenePayload"));
        source += QStringLiteral("void %1(SceneContext scene, Ray ray, inout RayQueryContext queries, "
                                 "inout ScenePayload payload)\n{\n    payload.Radiance = float3(0, 0, 0);\n}\n")
                      .arg(function);
    }
    else
    {
        return {};
    }
    return source.toUtf8();
}

RenderGraphDocumentStatus
PrepareSceneRayDefaults(RenderGraphSnapshot&, QJsonObject& inoutManifest, const QJsonObject& node, const QString&)
{
    const bool isSceneCompute = IsSceneComputeNode(node);
    if (node.value(QStringLiteral("kind")).toString() != QStringLiteral("RayTracingPipeline") &&
        isSceneCompute == false)
    {
        return RenderGraphDocumentStatus::Ok;
    }
    QJsonArray techniques = inoutManifest.value(QStringLiteral("techniques")).toArray();
    for (const QJsonValueConstRef technique : techniques)
    {
        if (technique.toObject().value(QStringLiteral("id")).toString() == RAY_TECHNIQUE)
        {
            return RenderGraphDocumentStatus::Ok;
        }
    }
    // Only new placeholders opt into the default. Never reinterpret authored callbacks.
    // A scene compute kernel is not a technique callback, so its pass may add the technique at any time.
    for (const QJsonValueConstRef shader : node.value(QStringLiteral("shaders")).toArray())
    {
        if (isSceneCompute == false &&
            shader.toObject().value(QStringLiteral("program")).toString() != QStringLiteral("Program"))
        {
            return RenderGraphDocumentStatus::Ok;
        }
    }
    if (techniques.size() >=
        inoutManifest.value(QStringLiteral("budgets")).toObject().value(QStringLiteral("configurations")).toInt())
    {
        return RenderGraphDocumentStatus::Capacity;
    }
    // Ray pipelines receive their triangle recipe and callbacks from technique preparation. A
    // compute-only package names no callbacks, so it declares the automatic triangle recipe here.
    techniques.append(QJsonObject{{QStringLiteral("id"), RAY_TECHNIQUE},
                                  {QStringLiteral("configuration"), QStringLiteral("RayTracing")},
                                  {QStringLiteral("recipes"),
                                   isSceneCompute == true
                                       ? QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("Triangles")}}}
                                       : QJsonArray{}},
                                  {QStringLiteral("scene_interface"), MakeSceneInterface()}});
    inoutManifest.insert(QStringLiteral("techniques"), techniques);
    return RenderGraphDocumentStatus::Ok;
}

RenderGraphDocumentStatus ConvertSceneRayInterface(RenderGraphSnapshot& inoutSnapshot,
                                                   const QString& manifestFile,
                                                   const QString&,
                                                   QString& outError)
{
    outError = QStringLiteral("Scene materials require a complete ray-generation, miss and closest-hit technique. "
                              "The draft was preserved.");
    QJsonObject manifest = QJsonDocument::fromJson(inoutSnapshot.Files.value(manifestFile)).object();
    QJsonArray techniques = manifest.value(QStringLiteral("techniques")).toArray();
    const QJsonArray programs = manifest.value(QStringLiteral("programs")).toArray();
    qsizetype techniqueIndex = -1;
    for (qsizetype index = 0; index < techniques.size(); ++index)
    {
        if (techniques.at(index).toObject().value(QStringLiteral("id")).toString() == RAY_TECHNIQUE)
        {
            techniqueIndex = index;
        }
    }
    if (techniqueIndex < 0)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    if (HasSceneRayInterface(manifest) == true)
    {
        outError.clear();
        return RenderGraphDocumentStatus::Ok;
    }
    QJsonObject technique = techniques.at(techniqueIndex).toObject();
    QMap<QString, QJsonObject> originals;
    for (const QString& stage : RAY_STAGES)
    {
        const QString expectedInput = stage == QStringLiteral("ray_generation") ? QStringLiteral("RayLaunch")
                                      : stage == QStringLiteral("closest_hit")  ? QStringLiteral("RayHit")
                                                                                : QStringLiteral("Ray");
        for (const QJsonValueConstRef entry : programs)
        {
            const QJsonObject program = entry.toObject();
            if (program.value(QStringLiteral("id")) == technique.value(stage) &&
                program.value(QStringLiteral("stage")).toString() == stage &&
                program.value(QStringLiteral("input")).toString() == expectedInput &&
                program.value(QStringLiteral("bindings")).toArray().isEmpty() == true &&
                program.value(QStringLiteral("function")).toString().isEmpty() == false &&
                program.value(QStringLiteral("output")).toString() ==
                    (stage == QStringLiteral("ray_generation") ? QStringLiteral("Ray") : QStringLiteral("Color")) &&
                inoutSnapshot.Files.contains(program.value(QStringLiteral("source")).toString()) == true)
            {
                originals.insert(stage, program);
            }
        }
    }
    if (originals.size() != RAY_STAGES.size())
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    // Compatibility is linked by the pipeline builder. Preserve authored sources,
    // program identities and signatures; new scene callbacks can replace each stage independently.
    technique.insert(QStringLiteral("scene_interface"), MakeSceneInterface());
    techniques.replace(techniqueIndex, technique);
    manifest.insert(QStringLiteral("techniques"), techniques);
    inoutSnapshot.Files.insert(manifestFile, EncodeCanonicalJson(manifest));
    outError.clear();
    return RenderGraphDocumentStatus::Ok;
}
} // namespace ludus::editor
