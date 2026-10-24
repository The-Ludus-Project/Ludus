#include "internal/render_graph_configuration_template.h"
#include "internal/render_graph_shader_creation.h"

#include <ludus/foundation/base/types.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

namespace ludus::editor
{
using namespace foundation;
namespace
{
const QString VISIBILITY_VARYING = QStringLiteral("VisibilityRasterVarying");
const QString RAY_TECHNIQUE = QStringLiteral("RendererTriangles.RayTracing");

/// One built-in visibility template: its embedded resource identity, the package directory its
/// copied sources use, and the prefix that keeps its ray programs apart from authored ones.
struct VisibilityTemplate final
{
    QString Identity;
    QString Directory;
    QString ProgramPrefix;
    QString Name;
    bool HasRays = false;
};

/// Every visibility template shares this raster producer and its sources.
VisibilityTemplate GetRasterTemplate()
{
    return {QStringLiteral("visibility-buffer"),
            QStringLiteral("visibility_buffer/"),
            QStringLiteral("Visibility"),
            QStringLiteral("Visibility"),
            false};
}

bool FindVisibilityTemplate(const QString& identity, VisibilityTemplate& outTemplate)
{
    if (identity == QStringLiteral("visibility-buffer"))
    {
        outTemplate = GetRasterTemplate();
        return true;
    }
    if (identity == QStringLiteral("visibility-secondary-rays"))
    {
        outTemplate = {identity,
                       QStringLiteral("visibility_secondary_rays/"),
                       QStringLiteral("VisibilitySecondary"),
                       QStringLiteral("Visibility"),
                       true};
        return true;
    }
    if (identity == QStringLiteral("tiled-vbuffer-nee"))
    {
        outTemplate = {identity,
                       QStringLiteral("tiled_vbuffer_nee/"),
                       QStringLiteral("TiledNee"),
                       QStringLiteral("Tiled vbuffer NEE"),
                       true};
        return true;
    }
    return false;
}

QByteArray ReadVisibilityTemplate(const QString& identity, const QString& file)
{
    QFile source(QStringLiteral(":/render-graph/templates/") + identity + QLatin1Char('/') + file);
    if (source.open(QIODevice::ReadOnly) == false || source.size() > RenderGraphDocument::MAX_FILE_BYTES)
    {
        return {};
    }
    return source.readAll();
}

bool IsRasterProgram(const QString& identity)
{
    return identity == QStringLiteral("Geometry") || identity == QStringLiteral("Color");
}

QString GetVisibilityProgramName(const QString& identity, const VisibilityTemplate& source)
{
    return (IsRasterProgram(identity) == true ? GetRasterTemplate().ProgramPrefix : source.ProgramPrefix) + identity;
}

// Capture an entry source and its literal local include closure: shared helpers such as a
// tiled table header must arrive with the callbacks that include them.
bool CopyVisibilitySources(RenderGraphSnapshot& inoutSnapshot,
                           const VisibilityTemplate& source,
                           const QString& entry,
                           QString& outError)
{
    QStringList pending{entry};
    QSet<QString> visited;
    static const QRegularExpression INCLUDE(QStringLiteral("^\\s*#\\s*include\\s+\"([^\"]+)\""),
                                            QRegularExpression::MultilineOption);
    while (pending.isEmpty() == false)
    {
        const QString relative = QDir::cleanPath(pending.takeFirst());
        if (visited.contains(relative) == true)
        {
            continue;
        }
        visited.insert(relative);
        const QString destination = source.Directory + relative;
        const bool isInside =
            relative.startsWith(QStringLiteral("../")) == false && QDir::isAbsolutePath(relative) == false;
        const QByteArray bytes = isInside == true ? ReadVisibilityTemplate(source.Identity, relative) : QByteArray{};
        if (bytes.isEmpty() == true ||
            (inoutSnapshot.Files.contains(destination) == true && inoutSnapshot.Files.value(destination) != bytes))
        {
            outError = QStringLiteral("Visibility shader source %1 is unavailable or has authored changes. "
                                      "Rename the custom source before importing this template.")
                           .arg(destination);
            return false;
        }
        inoutSnapshot.Files.insert(destination, bytes);
        QRegularExpressionMatchIterator includes = INCLUDE.globalMatch(QString::fromUtf8(bytes));
        while (includes.hasNext() == true)
        {
            pending.append(QDir(QFileInfo(relative).path()).filePath(includes.next().captured(1)));
        }
    }
    return true;
}

bool MergeVisibilitySchemas(QJsonObject& inoutManifest, const QJsonObject& source, QString& outError)
{
    QJsonArray schemas = inoutManifest.value(QStringLiteral("schemas")).toArray();
    for (const QJsonValueConstRef value : source.value(QStringLiteral("schemas")).toArray())
    {
        QJsonObject incoming = value.toObject();
        const bool isVertex = incoming.value(QStringLiteral("id")) == QStringLiteral("RendererStatic");
        if (isVertex == false)
        {
            incoming.insert(QStringLiteral("id"), VISIBILITY_VARYING);
        }
        bool hasSchema = false;
        for (qsizetype index = 0; index < schemas.size(); ++index)
        {
            QJsonObject existing = schemas.at(index).toObject();
            if (existing.value(QStringLiteral("id")) != incoming.value(QStringLiteral("id")))
            {
                continue;
            }
            hasSchema = true;
            if (existing.value(QStringLiteral("role")) != incoming.value(QStringLiteral("role")))
            {
                outError = QStringLiteral("Schema %1 has an incompatible role. Repair it in the shader manifest.")
                               .arg(incoming.value(QStringLiteral("id")).toString());
                return false;
            }
            if (isVertex == true)
            {
                const QJsonArray expected = incoming.value(QStringLiteral("fields")).toArray();
                QJsonArray fields = existing.value(QStringLiteral("fields")).toArray();
                constexpr qsizetype IDENTITY_ATTRIBUTE_COUNT = 2;
                const qsizetype originalCount = expected.size() - IDENTITY_ATTRIBUTE_COUNT;
                if (fields.size() != originalCount && fields.size() != expected.size())
                {
                    outError = QStringLiteral(
                        "RendererStatic needs the standard geometry fields with optional visibility IDs. "
                        "Repair RendererStatic.fields in the shader manifest.");
                    return false;
                }
                for (qsizetype field = 0; field < fields.size(); ++field)
                {
                    if (fields.at(field) != expected.at(field))
                    {
                        outError = QStringLiteral("RendererStatic field %1 differs from the visibility input layout. "
                                                  "Repair that field in the shader manifest.")
                                       .arg(field);
                        return false;
                    }
                }
                for (qsizetype field = fields.size(); field < expected.size(); ++field)
                {
                    fields.append(expected.at(field));
                }
                existing.insert(QStringLiteral("fields"), fields);
                schemas.replace(index, existing);
            }
            else if (existing != incoming)
            {
                outError = QStringLiteral("VisibilityRasterVarying is already defined with different fields. "
                                          "Rename that custom schema before importing visibility.");
                return false;
            }
        }
        if (hasSchema == false)
        {
            schemas.append(incoming);
        }
    }
    inoutManifest.insert(QStringLiteral("schemas"), schemas);
    return true;
}

bool MergeVisibilityPrograms(RenderGraphSnapshot& inoutSnapshot,
                             QJsonObject& inoutManifest,
                             const QJsonObject& source,
                             const VisibilityTemplate& selected,
                             QString& outError,
                             RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull)
{
    QJsonArray programs = inoutManifest.value(QStringLiteral("programs")).toArray();
    for (const QJsonValueConstRef value : source.value(QStringLiteral("programs")).toArray())
    {
        QJsonObject incoming = value.toObject();
        const QString identity = incoming.value(QStringLiteral("id")).toString();
        if (identity == QStringLiteral("Depth"))
        {
            continue;
        }
        const VisibilityTemplate owner = IsRasterProgram(identity) == true ? GetRasterTemplate() : selected;
        const QString relative = incoming.value(QStringLiteral("source")).toString();
        if (CopyVisibilitySources(inoutSnapshot, owner, relative, outError) == false)
        {
            return false;
        }
        incoming.insert(QStringLiteral("id"), GetVisibilityProgramName(identity, selected));
        incoming.insert(QStringLiteral("source"), owner.Directory + relative);
        bool hasProgram = false;
        for (const QJsonValueConstRef existing : programs)
        {
            if (existing.toObject().value(QStringLiteral("id")) == incoming.value(QStringLiteral("id")))
            {
                hasProgram = true;
                if (existing.toObject() != incoming)
                {
                    outError = QStringLiteral("Shader program %1 already has a different definition. "
                                              "Rename that custom program before importing visibility.")
                                   .arg(incoming.value(QStringLiteral("id")).toString());
                    return false;
                }
            }
        }
        if (hasProgram == false)
        {
            programs.append(incoming);
        }
    }
    if (EnsureTemplateProgramBudget(inoutManifest,
                                    static_cast<int32>(programs.size()),
                                    selected.Name,
                                    outError,
                                    inoutBudgetOrNull) == false)
    {
        return false;
    }
    inoutManifest.insert(QStringLiteral("programs"), programs);
    return true;
}

bool MergeVisibilityRayInterface(RenderGraphSnapshot& inoutSnapshot,
                                 QJsonObject& inoutManifest,
                                 const QJsonObject& source,
                                 const VisibilityTemplate& selected,
                                 QString& outError)
{
    QJsonArray techniques = inoutManifest.value(QStringLiteral("techniques")).toArray();
    for (qsizetype index = 0; index < techniques.size(); ++index)
    {
        QJsonObject technique = techniques.at(index).toObject();
        if (technique.value(QStringLiteral("id")) != RAY_TECHNIQUE)
        {
            continue;
        }
        QJsonObject interface = technique.value(QStringLiteral("scene_interface")).toObject();
        if (interface.contains(QStringLiteral("version")) == true &&
            interface.value(QStringLiteral("version")).toInt() != 1)
        {
            outError =
                QStringLiteral("The ray scene interface has an unsupported version. Use scene interface version 1.");
            return false;
        }
        const QString payloadFile = interface.value(QStringLiteral("payload")).toString();
        QByteArray payload;
        if (payloadFile.isEmpty() == true)
        {
            QFile defaults(QStringLiteral(":/shader-api/scene_payload.slang"));
            if (defaults.open(QIODevice::ReadOnly) == true)
            {
                payload = defaults.readAll();
            }
        }
        else
        {
            payload = inoutSnapshot.Files.value(payloadFile);
        }
        // Accept comments, whitespace and custom payload fields; the compiler owns full grammar validation.
        QString declaration = QString::fromUtf8(payload);
        static const QRegularExpression COMMENTS(QStringLiteral("/\\*[\\s\\S]*?\\*/|//[^\\r\\n]*"));
        declaration.remove(COMMENTS);
        static const QRegularExpression PAYLOAD(QStringLiteral("struct\\s+ScenePayload\\s*\\{([^{}]*)\\}\\s*;"));
        static const QRegularExpression RADIANCE(QStringLiteral("\\bfloat3\\s+Radiance\\s*;"));
        const QRegularExpressionMatch match = PAYLOAD.match(declaration);
        if (match.hasMatch() == false || RADIANCE.match(match.captured(1)).hasMatch() == false)
        {
            outError = QStringLiteral("Shared ray payload %1 needs a float3 Radiance field in ScenePayload. "
                                      "Add that field while retaining existing fields and retry.")
                           .arg(payloadFile);
            return false;
        }
        const QJsonObject defaults = source.value(QStringLiteral("techniques"))
                                         .toArray()
                                         .last()
                                         .toObject()
                                         .value(QStringLiteral("scene_interface"))
                                         .toObject();
        if (interface.contains(QStringLiteral("version")) == false)
        {
            interface.insert(QStringLiteral("version"), defaults.value(QStringLiteral("version")));
        }
        // Budgets are per-invocation maxima shared by every ray pass: raise them to what this
        // template's callbacks issue, and keep larger authored budgets.
        for (const QString& field : {QStringLiteral("surface_queries"), QStringLiteral("visibility_queries")})
        {
            if (interface.contains(field) == false || interface.value(field).toInt() < defaults.value(field).toInt())
            {
                interface.insert(field, defaults.value(field));
            }
        }
        interface.insert(QStringLiteral("visibility_buffer"), true);
        technique.insert(QStringLiteral("scene_interface"), interface);
        techniques.replace(index, technique);
        inoutManifest.insert(QStringLiteral("techniques"), techniques);
        return true;
    }
    QJsonObject technique = source.value(QStringLiteral("techniques")).toArray().last().toObject();
    for (const QString& stage :
         {QStringLiteral("ray_generation"), QStringLiteral("miss"), QStringLiteral("closest_hit")})
    {
        technique.insert(stage, GetVisibilityProgramName(technique.value(stage).toString(), selected));
    }
    QJsonObject interface = technique.value(QStringLiteral("scene_interface")).toObject();
    const QString relative = interface.value(QStringLiteral("payload")).toString();
    const QString destination = selected.Directory + relative;
    const QByteArray payload = ReadVisibilityTemplate(selected.Identity, relative);
    if (payload.isEmpty() == true ||
        (inoutSnapshot.Files.contains(destination) == true && inoutSnapshot.Files.value(destination) != payload))
    {
        outError = QStringLiteral("Shared visibility payload source %1 conflicts with an authored file. "
                                  "Rename that custom file and retry.")
                       .arg(destination);
        return false;
    }
    inoutSnapshot.Files.insert(destination, payload);
    interface.insert(QStringLiteral("payload"), destination);
    technique.insert(QStringLiteral("scene_interface"), interface);
    techniques.append(technique);
    inoutManifest.insert(QStringLiteral("techniques"), techniques);
    return true;
}
} // namespace

// File and configuration identify distinct authored records.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
RenderGraphDocumentStatus PrepareVisibilityConfiguration(RenderGraphSnapshot& inoutSnapshot,
                                                         const QString& rootGraph,
                                                         const QString& file,
                                                         const QString& configuration,
                                                         const QString& templateIdentity,
                                                         QString& outError,
                                                         RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull)
{
    VisibilityTemplate selected;
    if (FindVisibilityTemplate(templateIdentity, selected) == false)
    {
        outError = QStringLiteral("Select a built-in configuration template.");
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    QJsonObject graph = QJsonDocument::fromJson(inoutSnapshot.Files.value(file)).object();
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    static const QRegularExpression IDENTIFIER(QStringLiteral("\\A[A-Za-z][A-Za-z0-9_-]{0,62}\\z"));
    if (graph.isEmpty() == true || IDENTIFIER.match(configuration).hasMatch() == false)
    {
        outError = QStringLiteral("Choose a valid configuration name in an open graph.");
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    qsizetype destinationIndex = -1;
    for (qsizetype index = 0; index < configurations.size(); ++index)
    {
        const QJsonObject record = configurations.at(index).toObject();
        if (record.value(QStringLiteral("id")) != configuration)
        {
            continue;
        }
        destinationIndex = index;
        for (const QString& field :
             {QStringLiteral("nodes"), QStringLiteral("edges"), QStringLiteral("inputs"), QStringLiteral("outputs")})
        {
            if (record.value(field).toArray().isEmpty() == false)
            {
                outError = QStringLiteral("Select a new or empty configuration. Existing contents are preserved.");
                return RenderGraphDocumentStatus::InvalidEdit;
            }
        }
    }
    if (destinationIndex < 0 && configurations.size() >= RenderGraphDocument::MAX_CONFIGURATIONS)
    {
        outError = QStringLiteral("The graph has reached its configuration capacity. The draft was preserved.");
        return RenderGraphDocumentStatus::Capacity;
    }
    const QString manifestFile =
        rootGraph.chopped(QStringLiteral(".lurender.json").size()) + QStringLiteral(".lushader.json");
    QJsonObject manifest = QJsonDocument::fromJson(inoutSnapshot.Files.value(manifestFile)).object();
    const QJsonArray profiles = manifest.value(QStringLiteral("profiles")).toArray();
    if (manifest.isEmpty() == true || profiles.isEmpty() == true ||
        (selected.HasRays == true && profiles.contains(QStringLiteral("metal")) == false &&
         profiles.contains(QStringLiteral("spirv")) == false))
    {
        outError =
            QStringLiteral("Visibility needs a Renderer manifest; ray passes also need a Metal or Vulkan profile.");
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    QByteArray bytes = ReadVisibilityTemplate(templateIdentity, QStringLiteral("main.lushader.json.in"));
    bytes.replace("@LUDUS_DEFAULT_RENDER_GRAPH_PROFILES@", QJsonDocument(profiles).toJson(QJsonDocument::Compact));
    const QJsonObject source = QJsonDocument::fromJson(bytes).object();
    const QJsonArray records =
        QJsonDocument::fromJson(ReadVisibilityTemplate(templateIdentity, QStringLiteral("library.lurender.json")))
            .object()
            .value(QStringLiteral("configurations"))
            .toArray();
    if (source.isEmpty() == true || records.isEmpty() == true ||
        UpgradeDefaultTemplateBudgets(manifest, source) == false)
    {
        outError = QStringLiteral("Visibility template resources are unavailable; repair the editor installation.");
        return RenderGraphDocumentStatus::NotFound;
    }
    if (MergeVisibilitySchemas(manifest, source, outError) == false ||
        MergeVisibilityPrograms(inoutSnapshot, manifest, source, selected, outError, inoutBudgetOrNull) == false ||
        (selected.HasRays == true &&
         MergeVisibilityRayInterface(inoutSnapshot, manifest, source, selected, outError) == false))
    {
        outError += QStringLiteral(" The draft was preserved.");
        return RenderGraphDocumentStatus::Conflict;
    }
    QJsonObject record = records.first().toObject();
    record.insert(QStringLiteral("id"), configuration);
    QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
    for (qsizetype index = 0; index < nodes.size(); ++index)
    {
        QJsonObject node = nodes.at(index).toObject();
        QJsonArray shaders = node.value(QStringLiteral("shaders")).toArray();
        for (qsizetype shader = 0; shader < shaders.size(); ++shader)
        {
            QJsonObject entry = shaders.at(shader).toObject();
            entry.insert(QStringLiteral("program"),
                         GetVisibilityProgramName(entry.value(QStringLiteral("program")).toString(), selected));
            shaders.replace(shader, entry);
        }
        if (shaders.isEmpty() == false)
        {
            node.insert(QStringLiteral("shaders"), shaders);
            nodes.replace(index, node);
        }
    }
    record.insert(QStringLiteral("nodes"), nodes);
    if (destinationIndex < 0)
    {
        configurations.append(record);
    }
    else
    {
        configurations.replace(destinationIndex, record);
    }
    graph.insert(QStringLiteral("version"), 2);
    graph.insert(QStringLiteral("configurations"), configurations);
    inoutSnapshot.Files.insert(file, EncodeCanonicalJson(graph));
    inoutSnapshot.Files.insert(manifestFile, EncodeCanonicalJson(manifest));
    return PrepareRenderGraphTechniques(inoutSnapshot, rootGraph, outError);
}
} // namespace ludus::editor
