// Import renderer-owned scene compute templates and their shader closure into a draft.
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
struct ComputeTemplate
{
    QString ResourceRoot;
    QString SourceRoot;
    QString ProgramPrefix;
    QString Name;
};
QString GetRayTechniqueName()
{
    return QStringLiteral("RendererTriangles.RayTracing");
}

QByteArray ReadComputeTemplate(const ComputeTemplate& selected, const QString& file)
{
    QFile source(selected.ResourceRoot + file);
    if (source.open(QIODevice::ReadOnly) == false || source.size() > RenderGraphDocument::MAX_FILE_BYTES)
    {
        return {};
    }
    return source.readAll();
}

QString GetComputeProgramName(const ComputeTemplate& selected, const QString& identity)
{
    return selected.ProgramPrefix + identity;
}

// Capture the kernels and their literal local includes without replacing authored bytes.
RenderGraphDocumentStatus CopyComputeSources(RenderGraphSnapshot& inoutSnapshot,
                                             const ComputeTemplate& selected,
                                             QStringList pending,
                                             QString& outError)
{
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
        const bool isInside =
            relative.startsWith(QStringLiteral("../")) == false && QDir::isAbsolutePath(relative) == false;
        const QByteArray bytes = isInside == true ? ReadComputeTemplate(selected, relative) : QByteArray{};
        const QString destination = selected.SourceRoot + relative;
        if (bytes.isEmpty() == true)
        {
            outError = QStringLiteral("Compute ray-tracing source %1 is unavailable; repair the editor installation.")
                           .arg(relative);
            return RenderGraphDocumentStatus::NotFound;
        }
        if (inoutSnapshot.Files.contains(destination) == true && inoutSnapshot.Files.value(destination) != bytes)
        {
            outError = QStringLiteral("Shader source %1 already has different contents. Rename the custom source "
                                      "before importing compute ray tracing.")
                           .arg(destination);
            return RenderGraphDocumentStatus::Conflict;
        }
        // PrepareConfigurationTemplate checks the package file limit once the whole template is in.
        inoutSnapshot.Files.insert(destination, bytes);
        QRegularExpressionMatchIterator includes = INCLUDE.globalMatch(QString::fromUtf8(bytes));
        while (includes.hasNext() == true)
        {
            pending.append(QDir(QFileInfo(relative).path()).filePath(includes.next().captured(1)));
        }
    }
    return RenderGraphDocumentStatus::Ok;
}

// Only scene compute kernels are imported; the raster programs that a New Graph package
// carries for raster fallback already exist in the destination manifest.
RenderGraphDocumentStatus MergeComputePrograms(RenderGraphSnapshot& inoutSnapshot,
                                               QJsonObject& inoutManifest,
                                               const QJsonObject& source,
                                               const ComputeTemplate& selected,
                                               QString& outError,
                                               RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull)
{
    QJsonArray programs = inoutManifest.value(QStringLiteral("programs")).toArray();
    QStringList sources;
    for (const QJsonValueConstRef value : source.value(QStringLiteral("programs")).toArray())
    {
        QJsonObject program = value.toObject();
        if (program.value(QStringLiteral("stage")).toString() != QStringLiteral("compute"))
        {
            continue;
        }
        sources.append(program.value(QStringLiteral("source")).toString());
        program.insert(QStringLiteral("id"),
                       GetComputeProgramName(selected, program.value(QStringLiteral("id")).toString()));
        program.insert(QStringLiteral("source"),
                       selected.SourceRoot + program.value(QStringLiteral("source")).toString());
        bool hasProgram = false;
        for (const QJsonValueConstRef existing : programs)
        {
            if (existing.toObject().value(QStringLiteral("id")) != program.value(QStringLiteral("id")))
            {
                continue;
            }
            hasProgram = true;
            if (existing.toObject() != program)
            {
                outError = QStringLiteral("Shader program %1 already has a different definition. Rename that custom "
                                          "program before importing compute ray tracing.")
                               .arg(program.value(QStringLiteral("id")).toString());
                return RenderGraphDocumentStatus::Conflict;
            }
        }
        if (hasProgram == false)
        {
            programs.append(program);
        }
    }
    if (EnsureTemplateProgramBudget(inoutManifest,
                                    static_cast<int32>(programs.size()),
                                    selected.Name,
                                    outError,
                                    inoutBudgetOrNull) == false)
    {
        return RenderGraphDocumentStatus::Capacity;
    }
    inoutManifest.insert(QStringLiteral("programs"), programs);
    return CopyComputeSources(inoutSnapshot, selected, sources, outError);
}

// Scene compute passes need scene_interface on the shared ray technique. An existing technique
// keeps its callbacks, payload and larger budgets; its query budgets are raised to the template's.
RenderGraphDocumentStatus
MergeComputeRayTechnique(QJsonObject& inoutManifest, const QJsonObject& source, QString& outError)
{
    QJsonObject defaults;
    for (const QJsonValueConstRef value : source.value(QStringLiteral("techniques")).toArray())
    {
        if (value.toObject().value(QStringLiteral("id")) == GetRayTechniqueName())
        {
            defaults = value.toObject();
        }
    }
    const QJsonObject defaultInterface = defaults.value(QStringLiteral("scene_interface")).toObject();
    QJsonArray techniques = inoutManifest.value(QStringLiteral("techniques")).toArray();
    for (qsizetype index = 0; index < techniques.size(); ++index)
    {
        QJsonObject technique = techniques.at(index).toObject();
        if (technique.value(QStringLiteral("id")) != GetRayTechniqueName())
        {
            continue;
        }
        QJsonObject interface = technique.value(QStringLiteral("scene_interface")).toObject();
        if (interface.contains(QStringLiteral("version")) == true &&
            interface.value(QStringLiteral("version")).toInt() !=
                defaultInterface.value(QStringLiteral("version")).toInt())
        {
            outError =
                QStringLiteral("The ray scene interface has an unsupported version. Use scene interface version 1.");
            return RenderGraphDocumentStatus::Conflict;
        }
        interface.insert(QStringLiteral("version"), defaultInterface.value(QStringLiteral("version")));
        // Budgets are per-invocation maxima shared by every ray pass of the package. The optional line and
        // capture budgets are raised only by templates that declare them, so other imports keep their manifests.
        for (const QString& field : {QStringLiteral("surface_queries"),
                                     QStringLiteral("visibility_queries"),
                                     QStringLiteral("line_queries"),
                                     QStringLiteral("capture_visibility_queries")})
        {
            if (defaultInterface.contains(field) == true &&
                (interface.contains(field) == false ||
                 interface.value(field).toInt() < defaultInterface.value(field).toInt()))
            {
                interface.insert(field, defaultInterface.value(field));
            }
        }
        // A capture budget never falls below the interactive one the loop above may have raised.
        const QString capture = QStringLiteral("capture_visibility_queries");
        if (interface.contains(capture) == true &&
            interface.value(capture).toInt() < interface.value(QStringLiteral("visibility_queries")).toInt())
        {
            interface.insert(capture, interface.value(QStringLiteral("visibility_queries")));
        }
        // Helper requirements are additive: a template that needs the polygonal-light helpers enables them,
        // and a technique that already has them keeps them.
        const QString polygonalLightSampling = QStringLiteral("polygonal_light_sampling");
        if (defaultInterface.value(polygonalLightSampling).toBool() == true)
        {
            interface.insert(polygonalLightSampling, true);
        }
        technique.insert(QStringLiteral("scene_interface"), interface);
        techniques.replace(index, technique);
        inoutManifest.insert(QStringLiteral("techniques"), techniques);
        return RenderGraphDocumentStatus::Ok;
    }
    // No ray technique yet: the template's has no default callbacks, because node-owned passes
    // select their own programs.
    techniques.append(defaults);
    const qsizetype techniqueBudget =
        inoutManifest.value(QStringLiteral("budgets")).toObject().value(QStringLiteral("configurations")).toInt();
    if (techniques.size() > techniqueBudget)
    {
        outError = QStringLiteral("Adding the shared ray-tracing technique needs %1 techniques, but "
                                  "budgets.configurations in the shader manifest is %2. Increase "
                                  "budgets.configurations and apply the template again. The draft was preserved.")
                       .arg(techniques.size())
                       .arg(techniqueBudget);
        return RenderGraphDocumentStatus::Capacity;
    }
    inoutManifest.insert(QStringLiteral("techniques"), techniques);
    return RenderGraphDocumentStatus::Ok;
}
} // namespace

// File and configuration identify distinct authored records.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
RenderGraphDocumentStatus PrepareSceneComputeTemplateConfiguration(
    RenderGraphSnapshot& inoutSnapshot,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): root path, library path and configuration identity
    const QString& rootGraph,
    const QString& file,
    const QString& configuration,
    QString& outError,
    const SceneComputeTemplate& selectedTemplate,
    RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull)
{
    const QString& templateIdentity = selectedTemplate.Identity;
    const QString& programPrefix = selectedTemplate.ProgramPrefix;
    const QString& name = selectedTemplate.Name;
    QString directory = templateIdentity;
    directory.replace(QLatin1Char('-'), QLatin1Char('_'));
    const ComputeTemplate selected{
        QStringLiteral(":/render-graph/templates/") + templateIdentity + QLatin1Char('/'),
        directory + QLatin1Char('/'),
        programPrefix,
        name,
    };
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
        outError = DescribeConfigurationCapacity(name, file);
        return RenderGraphDocumentStatus::Capacity;
    }
    const QString manifestFile =
        rootGraph.chopped(QStringLiteral(".lurender.json").size()) + QStringLiteral(".lushader.json");
    QJsonObject manifest = QJsonDocument::fromJson(inoutSnapshot.Files.value(manifestFile)).object();
    const QJsonArray profiles = manifest.value(QStringLiteral("profiles")).toArray();
    if (manifest.isEmpty() == true ||
        (profiles.contains(QStringLiteral("metal")) == false && profiles.contains(QStringLiteral("spirv")) == false))
    {
        outError = QStringLiteral("%1 requires a Renderer manifest with a Metal or Vulkan profile.").arg(name);
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    QByteArray bytes = ReadComputeTemplate(selected, QStringLiteral("main.lushader.json.in"));
    bytes.replace("@LUDUS_DEFAULT_RENDER_GRAPH_PROFILES@", QJsonDocument(profiles).toJson(QJsonDocument::Compact));
    const QJsonObject source = QJsonDocument::fromJson(bytes).object();
    const QJsonArray records =
        QJsonDocument::fromJson(ReadComputeTemplate(selected, QStringLiteral("library.lurender.json")))
            .object()
            .value(QStringLiteral("configurations"))
            .toArray();
    if (source.isEmpty() == true || records.isEmpty() == true ||
        UpgradeDefaultTemplateBudgets(manifest, source) == false)
    {
        outError = QStringLiteral("Compute ray-tracing template resources are unavailable; repair the editor "
                                  "installation.");
        return RenderGraphDocumentStatus::NotFound;
    }
    RenderGraphDocumentStatus status =
        MergeComputePrograms(inoutSnapshot, manifest, source, selected, outError, inoutBudgetOrNull);
    if (status == RenderGraphDocumentStatus::Ok)
    {
        status = MergeComputeRayTechnique(manifest, source, outError);
    }
    if (status != RenderGraphDocumentStatus::Ok)
    {
        AppendDraftPreserved(outError);
        return status;
    }
    // The settings error already says that the draft was preserved.
    status =
        MergeTemplateSettings(manifest, source.value(QStringLiteral("settings")).toArray(), selected.Name, outError);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        return status;
    }
    QJsonObject record = records.first().toObject();
    record.insert(QStringLiteral("id"), configuration);
    QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
    for (qsizetype index = 0; index < nodes.size(); ++index)
    {
        QJsonObject node = nodes.at(index).toObject();
        QJsonArray shaders = node.value(QStringLiteral("shaders")).toArray();
        if (shaders.isEmpty() == true)
        {
            continue;
        }
        for (qsizetype shader = 0; shader < shaders.size(); ++shader)
        {
            QJsonObject entry = shaders.at(shader).toObject();
            entry.insert(QStringLiteral("program"),
                         GetComputeProgramName(selected, entry.value(QStringLiteral("program")).toString()));
            shaders.replace(shader, entry);
        }
        node.insert(QStringLiteral("shaders"), shaders);
        nodes.replace(index, node);
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

// File and configuration identify distinct authored records.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
RenderGraphDocumentStatus PrepareComputeRayTracingConfiguration(
    RenderGraphSnapshot& inoutSnapshot,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): root path, library path and configuration identity
    const QString& rootGraph,
    const QString& file,
    const QString& configuration,
    QString& outError,
    const QString& templateIdentity,
    RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull)
{
    SceneComputeTemplate selected{templateIdentity, {}, {}};
    if (templateIdentity == QStringLiteral("simple-compute-ray-tracing"))
    {
        selected.ProgramPrefix = QStringLiteral("SimpleComputeRay");
        selected.Name = QStringLiteral("Simple compute ray tracing");
    }
    else if (templateIdentity == QStringLiteral("compute-ray-tracing"))
    {
        selected.ProgramPrefix = QStringLiteral("ComputeRay");
        selected.Name = QStringLiteral("Compute ray tracing");
    }
    else if (templateIdentity == QStringLiteral("veach-guibas-mis"))
    {
        selected.ProgramPrefix = QStringLiteral("VeachMis");
        selected.Name = QStringLiteral("Veach and Guibas MIS");
    }
    else if (templateIdentity == QStringLiteral("ris-projected-solid-angle"))
    {
        selected.ProgramPrefix = QStringLiteral("RisProjectedSolidAngle");
        selected.Name = QStringLiteral("RIS and projected solid angle");
    }
    else if (templateIdentity == QStringLiteral("polygonal-light-sampling"))
    {
        selected.ProgramPrefix = QStringLiteral("PolygonalLight");
        selected.Name = QStringLiteral("Polygonal light sampling");
    }
    else if (templateIdentity == QStringLiteral("slice-sampling-luminaire"))
    {
        selected.ProgramPrefix = QStringLiteral("SliceSampling");
        selected.Name = QStringLiteral("Slice sampling luminaire");
    }
    else if (templateIdentity == QStringLiteral("steinberg-yan-plt"))
    {
        selected.ProgramPrefix = QStringLiteral("SteinbergYan");
        selected.Name = QStringLiteral("Steinberg and Yan physical light transport");
    }
    else
    {
        outError = QStringLiteral("Choose a built-in scene compute template.");
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    return PrepareSceneComputeTemplateConfiguration(inoutSnapshot,
                                                    rootGraph,
                                                    file,
                                                    configuration,
                                                    outError,
                                                    selected,
                                                    inoutBudgetOrNull);
}
} // namespace ludus::editor
