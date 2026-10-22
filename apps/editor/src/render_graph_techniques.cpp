#include "internal/render_graph_shader_creation.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLatin1StringView>

namespace ludus::editor
{
namespace
{
constexpr foundation::int32 TRIANGLE_ATTRIBUTE_BYTES = 8;
constexpr QLatin1StringView RAY_TECHNIQUE("RendererTriangles.RayTracing");

QStringList GetTechniqueStages(const QString& identity)
{
    return identity == RAY_TECHNIQUE
               ? QStringList{QStringLiteral("ray_generation"), QStringLiteral("miss"), QStringLiteral("closest_hit")}
               : QStringList{QStringLiteral("vertex"), QStringLiteral("fragment")};
}

QString GetTechniqueIdentity(const QJsonObject& node)
{
    const QString kind = node.value(QStringLiteral("kind")).toString();
    if (kind == QStringLiteral("RayTracingPipeline"))
    {
        return QString(RAY_TECHNIQUE);
    }
    if (kind != QStringLiteral("GraphicsPipeline") && kind != QStringLiteral("Raster"))
    {
        return {};
    }
    const QJsonObject invocation =
        kind == QStringLiteral("Raster") ? node : node.value(QStringLiteral("invocation")).toObject();
    const QString material = invocation.value(QStringLiteral("material")).toString();
    const QString mode = invocation.value(QStringLiteral("mode")).toString();
    if ((material != QStringLiteral("DefaultOpaque") && material != QStringLiteral("DefaultMasked")) ||
        (mode != QStringLiteral("ColorAndDepth") && mode != QStringLiteral("DepthOnly")))
    {
        return {};
    }
    return (material == QStringLiteral("DefaultMasked") ? QStringLiteral("DefaultStaticMasked.")
                                                        : QStringLiteral("DefaultStaticOpaque.")) +
           mode;
}

QJsonObject NormalizeTriangleRecipe(QJsonObject recipe)
{
    const QJsonObject defaults{{QStringLiteral("max_recursion"), 1},
                               {QStringLiteral("payload_bytes"), 0},
                               {QStringLiteral("attribute_bytes"), TRIANGLE_ATTRIBUTE_BYTES}};
    for (QJsonObject::const_iterator field = defaults.constBegin(); field != defaults.constEnd(); ++field)
    {
        if (recipe.contains(field.key()) == false)
        {
            recipe.insert(field.key(), field.value());
        }
    }
    return recipe;
}

QJsonObject MakeTechnique(const QJsonObject& node, const QString& identity)
{
    const QStringList stages = GetTechniqueStages(identity);
    const QJsonArray shaders = node.value(QStringLiteral("shaders")).toArray();
    if (shaders.size() != stages.size())
    {
        return {};
    }
    QJsonObject technique{{QStringLiteral("id"), identity}};
    for (const QJsonValueConstRef value : shaders)
    {
        const QJsonObject shader = value.toObject();
        const QString stage = shader.value(QStringLiteral("stage")).toString();
        const QString program = shader.value(QStringLiteral("program")).toString();
        if (stages.contains(stage) == false || technique.contains(stage) == true || program.isEmpty() == true)
        {
            return {};
        }
        technique.insert(stage, program);
    }
    QJsonObject recipe = node.value(QStringLiteral("state")).toObject();
    if (identity == RAY_TECHNIQUE)
    {
        // Match the triangle profile's automatic limits; do not change authored overrides.
        const QJsonObject defaults = NormalizeTriangleRecipe({});
        recipe = NormalizeTriangleRecipe(recipe);
        if (recipe != defaults)
        {
            return {}; // Unsupported drafts keep the execution-profile diagnostic.
        }
        technique.insert(QStringLiteral("configuration"), QStringLiteral("RayTracing"));
        recipe.insert(QStringLiteral("id"), QStringLiteral("Triangles"));
    }
    else
    {
        const QString mode =
            node.value(QStringLiteral("invocation")).toObject().value(QStringLiteral("mode")).toString();
        technique.insert(QStringLiteral("configuration"), mode);
        recipe.insert(QStringLiteral("color"), QStringLiteral("None"));
        for (const QJsonValueConstRef value : node.value(QStringLiteral("outputs")).toArray())
        {
            const QJsonObject output = value.toObject();
            const QString type = output.value(QStringLiteral("type")).toString();
            if (type == QStringLiteral("ColorTexture"))
            {
                recipe.insert(QStringLiteral("color"), output.value(QStringLiteral("format")));
            }
            else if (type == QStringLiteral("DepthTexture"))
            {
                recipe.insert(QStringLiteral("depth"), output.value(QStringLiteral("format")));
            }
        }
        if (recipe.contains(QStringLiteral("depth")) == false ||
            (mode == QStringLiteral("ColorAndDepth") &&
             recipe.value(QStringLiteral("color")) == QStringLiteral("None")))
        {
            return {};
        }
        recipe.insert(QStringLiteral("id"), QStringLiteral("Pipeline"));
    }
    technique.insert(QStringLiteral("recipes"), QJsonArray{recipe});
    return technique;
}

bool HaveMatchingPrograms(const QJsonObject& left, const QJsonObject& right)
{
    for (const QString& stage : GetTechniqueStages(left.value(QStringLiteral("id")).toString()))
    {
        if (left.value(stage) != right.value(stage))
        {
            return false;
        }
    }
    return true;
}

bool HasProgramConflict(const QString& identity,
                        const QJsonObject& request,
                        const QJsonArray& techniques,
                        const QMap<QString, QJsonObject>& requests)
{
    if (requests.contains(identity) == true && HaveMatchingPrograms(requests.value(identity), request) == false)
    {
        return true;
    }
    for (const QJsonValueConstRef existing : techniques)
    {
        const QJsonObject technique = existing.toObject();
        if (technique.value(QStringLiteral("id")).toString() == identity &&
            HaveMatchingPrograms(technique, request) == false)
        {
            return true;
        }
    }
    return false;
}

QString ResolveGraphicsTechnique(const QJsonObject& request,
                                 const QJsonArray& techniques,
                                 const QMap<QString, QJsonObject>& requests)
{
    QString family = request.value(QStringLiteral("id")).toString();
    // Preserve matching authored pairs, including stable IDs after program renames.
    for (const QJsonValueConstRef existing : techniques)
    {
        const QJsonObject technique = existing.toObject();
        const QString identity = technique.value(QStringLiteral("id")).toString();
        if ((identity == family || identity.startsWith(family + QLatin1Char('.')) == true) &&
            HaveMatchingPrograms(technique, request) == true &&
            HasProgramConflict(identity, request, techniques, requests) == false)
        {
            return identity;
        }
    }
    if (HasProgramConflict(family, request, techniques, requests) == false)
    {
        return family;
    }
    const QString base = family + QLatin1Char('.') + request.value(QStringLiteral("vertex")).toString() +
                         QLatin1Char('.') + request.value(QStringLiteral("fragment")).toString();
    QString identity = base;
    for (qsizetype suffix = 2; HasProgramConflict(identity, request, techniques, requests) == true; ++suffix)
    {
        identity = base + QLatin1Char('_') + QString::number(suffix);
    }
    return identity;
}

void AppendRecipes(QJsonObject& inoutTechnique, const QJsonArray& additions)
{
    QJsonArray recipes = inoutTechnique.value(QStringLiteral("recipes")).toArray();
    for (const QJsonValueConstRef value : additions)
    {
        QJsonObject addition = value.toObject();
        addition.remove(QStringLiteral("id"));
        bool hasRecipe = false;
        QStringList identities;
        for (const QJsonValueConstRef existing : recipes)
        {
            QJsonObject recipe = existing.toObject();
            identities.append(recipe.take(QStringLiteral("id")).toString());
            const bool isEquivalent = inoutTechnique.value(QStringLiteral("id")).toString() == RAY_TECHNIQUE
                                          ? NormalizeTriangleRecipe(recipe) == NormalizeTriangleRecipe(addition)
                                          : recipe == addition;
            hasRecipe = hasRecipe == true || isEquivalent == true;
        }
        if (hasRecipe == false)
        {
            QString identity = value.toObject().value(QStringLiteral("id")).toString();
            for (qsizetype suffix = 2; identities.contains(identity) == true; ++suffix)
            {
                identity = QStringLiteral("Pipeline_%1").arg(suffix);
            }
            addition.insert(QStringLiteral("id"), identity);
            recipes.append(addition);
        }
    }
    inoutTechnique.insert(QStringLiteral("recipes"), recipes);
}
} // namespace

RenderGraphDocumentStatus
PrepareRenderGraphTechniques(RenderGraphSnapshot& inoutSnapshot, const QString& rootGraph, QString& outError)
{
    const QString manifestFile =
        rootGraph.chopped(QStringLiteral(".lurender.json").size()) + QStringLiteral(".lushader.json");
    const QJsonDocument document = QJsonDocument::fromJson(inoutSnapshot.Files.value(manifestFile));
    if (document.isObject() == false)
    {
        return RenderGraphDocumentStatus::Ok; // Graph-only drafts do not require a build manifest.
    }
    QJsonObject manifest = document.object();
    QMap<QString, QJsonObject> requests;
    QMap<QString, QString> locations;
    QStringList legacyTechniques;
    QStringList files{rootGraph};
    for (qsizetype fileIndex = 0; fileIndex < files.size(); ++fileIndex)
    {
        const QString file = files.at(fileIndex);
        const QJsonObject graph = QJsonDocument::fromJson(inoutSnapshot.Files.value(file)).object();
        for (const QJsonValueConstRef configurationValue : graph.value(QStringLiteral("configurations")).toArray())
        {
            const QJsonObject configuration = configurationValue.toObject();
            for (const QJsonValueConstRef nodeValue : configuration.value(QStringLiteral("nodes")).toArray())
            {
                const QJsonObject node = nodeValue.toObject();
                const QString kind = node.value(QStringLiteral("kind")).toString();
                if (kind == QStringLiteral("Subgraph"))
                {
                    const QString asset = QDir::cleanPath(
                        QDir(QFileInfo(file).path()).filePath(node.value(QStringLiteral("asset")).toString()));
                    if (inoutSnapshot.Files.contains(asset) == true && files.contains(asset) == false)
                    {
                        files.append(asset);
                    }
                }
                QString identity = GetTechniqueIdentity(node);
                if (kind == QStringLiteral("Raster"))
                {
                    legacyTechniques.append(identity);
                    continue;
                }
                QJsonObject request = identity.isEmpty() == true ? QJsonObject{} : MakeTechnique(node, identity);
                if (kind == QStringLiteral("GraphicsPipeline") && request.isEmpty() == false)
                {
                    identity = ResolveGraphicsTechnique(request,
                                                        manifest.value(QStringLiteral("techniques")).toArray(),
                                                        requests);
                    request.insert(QStringLiteral("id"), identity);
                }
                if (request.isEmpty() == true)
                {
                    continue;
                }
                const QString location = file + QLatin1Char('#') +
                                         configuration.value(QStringLiteral("id")).toString() + QLatin1Char('/') +
                                         node.value(QStringLiteral("id")).toString();
                if (requests.contains(identity) == true && identity != RAY_TECHNIQUE &&
                    HaveMatchingPrograms(requests.value(identity), request) == false)
                {
                    outError =
                        QStringLiteral("Pipeline %1 and %2 select different shaders for shared technique %3. Use the "
                                       "same Program assignments or separate graph packages. The draft was preserved.")
                            .arg(location, locations.value(identity), identity);
                    return RenderGraphDocumentStatus::Conflict;
                }
                if (requests.contains(identity) == false)
                {
                    requests.insert(identity, request);
                    locations.insert(identity, location);
                }
                else
                {
                    AppendRecipes(requests[identity], request.value(QStringLiteral("recipes")).toArray());
                }
            }
        }
    }
    if (requests.isEmpty() == true)
    {
        outError.clear();
        return RenderGraphDocumentStatus::Ok;
    }
    QJsonArray techniques = manifest.value(QStringLiteral("techniques")).toArray();
    for (QMap<QString, QJsonObject>::const_iterator request = requests.constBegin(); request != requests.constEnd();
         ++request)
    {
        qsizetype matchingIndex = -1;
        for (qsizetype index = 0; index < techniques.size(); ++index)
        {
            if (techniques.at(index).toObject().value(QStringLiteral("id")).toString() == request.key())
            {
                matchingIndex = index;
                break;
            }
        }
        if (matchingIndex < 0)
        {
            techniques.append(request.value());
            continue;
        }
        QJsonObject technique = techniques.at(matchingIndex).toObject();
        if (HaveMatchingPrograms(technique, request.value()) == false)
        {
            if (legacyTechniques.contains(request.key()) == true)
            {
                outError =
                    QStringLiteral("Pipeline %1 would change shaders used by a legacy Raster node through technique "
                                   "%2. Reuse its programs or convert the legacy node first. The draft was preserved.")
                        .arg(locations.value(request.key()), request.key());
                return RenderGraphDocumentStatus::Conflict;
            }
            for (const QString& stage : GetTechniqueStages(request.key()))
            {
                technique.insert(stage, request.value().value(stage));
            }
        }
        AppendRecipes(technique, request.value().value(QStringLiteral("recipes")).toArray());
        techniques.replace(matchingIndex, technique);
    }
    qsizetype recipeCount = 0;
    for (const QJsonValueConstRef technique : techniques)
    {
        recipeCount += technique.toObject().value(QStringLiteral("recipes")).toArray().size();
    }
    const QJsonObject budgets = manifest.value(QStringLiteral("budgets")).toObject();
    if (techniques.size() > budgets.value(QStringLiteral("configurations")).toInt() ||
        recipeCount * manifest.value(QStringLiteral("profiles")).toArray().size() >
            budgets.value(QStringLiteral("pipelines")).toInt())
    {
        outError =
            QStringLiteral("Pipeline registration exceeds the configurations or pipelines budget in %1. Increase the "
                           "authoring budget before adding this technique. The draft was preserved.")
                .arg(manifestFile);
        return RenderGraphDocumentStatus::Capacity;
    }
    manifest.insert(QStringLiteral("techniques"), techniques);
    if (manifest != document.object())
    {
        inoutSnapshot.Files.insert(manifestFile, EncodeCanonicalJson(manifest));
    }
    outError.clear();
    return RenderGraphDocumentStatus::Ok;
}
} // namespace ludus::editor
