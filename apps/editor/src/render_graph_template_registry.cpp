#include "internal/render_graph_template_registry.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/logging/category.hpp>
#include <ludus/foundation/logging/log.hpp>

#include "internal/render_graph_upscale_node.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>

namespace ludus::editor
{
using namespace foundation;

namespace
{
LUDUS_DEFINE_LOG_CATEGORY(LOG_RENDER_GRAPH_TEMPLATES, "Editor.RenderGraphTemplates");

constexpr int32 CATALOG_VERSION = 1;

// Vocabularies are built on use: parsing runs once per catalog, and namespace-scope Qt containers
// would allocate during static initialization.
QStringList GetGraphCategories()
{
    return {QStringLiteral("raster"), QStringLiteral("ray-traced"), QStringLiteral("hybrid")};
}

QStringList GetInsertedCategories()
{
    return {QStringLiteral("scene"),
            QStringLiteral("resources"),
            QStringLiteral("passes"),
            QStringLiteral("visibility"),
            QStringLiteral("lighting"),
            QStringLiteral("simulation"),
            QStringLiteral("post-processing"),
            QStringLiteral("upscaling"),
            QStringLiteral("output"),
            QStringLiteral("structure")};
}

QStringList GetRequirements()
{
    return {QStringLiteral("native-ray-tracing"), QStringLiteral("denoiser"), QStringLiteral("metalfx")};
}

QStringList GetEntryFields()
{
    return {QStringLiteral("kind"),
            QStringLiteral("identity"),
            QStringLiteral("algorithm"),
            QStringLiteral("label"),
            QStringLiteral("description"),
            QStringLiteral("category"),
            QStringLiteral("defaultConfiguration"),
            QStringLiteral("requirements"),
            QStringLiteral("isDraft")};
}

bool ReadKind(const QString& text, RenderGraphTemplateKind& outKind)
{
    if (text == QStringLiteral("graph"))
    {
        outKind = RenderGraphTemplateKind::Graph;
        return true;
    }
    if (text == QStringLiteral("snippet"))
    {
        outKind = RenderGraphTemplateKind::Snippet;
        return true;
    }
    if (text == QStringLiteral("node"))
    {
        outKind = RenderGraphTemplateKind::Node;
        return true;
    }
    return false;
}

bool ReadText(const QJsonObject& entry, const QString& field, QString& outText)
{
    const QJsonValue value = entry.value(field);
    if (value.isString() == false || value.toString().trimmed().isEmpty() == true)
    {
        return false;
    }
    outText = value.toString();
    return true;
}

/// Validate one entry. The caller supplies the entry's position for messages.
bool ReadEntry(const QJsonObject& entry, const QString& where, RenderGraphTemplate& outTemplate, QString& outError)
{
    for (const QString& field : entry.keys())
    {
        if (GetEntryFields().contains(field) == false)
        {
            outError = QStringLiteral("%1 has an unknown field '%2'.").arg(where, field);
            return false;
        }
    }
    RenderGraphTemplate result;
    QString kind;
    if (ReadText(entry, QStringLiteral("kind"), kind) == false || ReadKind(kind, result.Kind) == false)
    {
        outError = QStringLiteral("%1 needs a kind of graph, snippet or node.").arg(where);
        return false;
    }
    const bool isNode = result.Kind == RenderGraphTemplateKind::Node;
    // Graph and snippet identities are tooling template names; node identities are authored node kinds.
    const QRegularExpression identityPattern(isNode == true ? QStringLiteral("\\A[A-Z][A-Za-z0-9]{0,62}\\z")
                                                            : QStringLiteral("\\A[a-z][a-z0-9-]{0,62}\\z"));
    if (ReadText(entry, QStringLiteral("identity"), result.Identity) == false ||
        identityPattern.match(result.Identity).hasMatch() == false)
    {
        outError = isNode == true
                       ? QStringLiteral("%1 needs an identity naming a node kind, such as ToneMap.").arg(where)
                       : QStringLiteral("%1 needs a lowercase identity of letters, digits and '-'.").arg(where);
        return false;
    }
    const QString name = QStringLiteral("%1 '%2'").arg(where, result.Identity);
    if (ReadText(entry, QStringLiteral("category"), result.Category) == false ||
        (result.Kind == RenderGraphTemplateKind::Graph ? GetGraphCategories() : GetInsertedCategories())
                .contains(result.Category) == false)
    {
        outError =
            QStringLiteral("%1 needs a category from: %2.")
                .arg(name,
                     (result.Kind == RenderGraphTemplateKind::Graph ? GetGraphCategories() : GetInsertedCategories())
                         .join(QStringLiteral(", ")));
        return false;
    }
    if (entry.contains(QStringLiteral("requirements")) == true)
    {
        const QJsonValue requirements = entry.value(QStringLiteral("requirements"));
        if (requirements.isArray() == false)
        {
            outError = QStringLiteral("%1 requirements must be an array.").arg(name);
            return false;
        }
        for (const QJsonValueConstRef requirement : requirements.toArray())
        {
            const QString text = requirement.toString();
            if (GetRequirements().contains(text) == false || result.Requirements.contains(text) == true)
            {
                outError = QStringLiteral("%1 requirements must be distinct values from: %2.")
                               .arg(name, GetRequirements().join(QStringLiteral(", ")));
                return false;
            }
            result.Requirements.append(text);
        }
    }
    if (isNode == false)
    {
        if (ReadText(entry, QStringLiteral("label"), result.Label) == false ||
            ReadText(entry, QStringLiteral("description"), result.Description) == false)
        {
            outError = QStringLiteral("%1 needs a label and a description.").arg(name);
            return false;
        }
        if (entry.contains(QStringLiteral("algorithm")) == true || entry.contains(QStringLiteral("isDraft")) == true)
        {
            outError = QStringLiteral("%1: only Upscale nodes take an algorithm or isDraft.").arg(name);
            return false;
        }
        const bool isSnippet = result.Kind == RenderGraphTemplateKind::Snippet;
        const QRegularExpression configurationPattern(QStringLiteral("\\A[A-Za-z][A-Za-z0-9_-]{0,62}\\z"));
        if (isSnippet == true &&
            (ReadText(entry, QStringLiteral("defaultConfiguration"), result.DefaultConfiguration) == false ||
             configurationPattern.match(result.DefaultConfiguration).hasMatch() == false))
        {
            outError = QStringLiteral("%1 needs a defaultConfiguration beginning with a letter, followed by "
                                      "letters, digits, '_' or '-'.")
                           .arg(name);
            return false;
        }
        if (isSnippet == false && entry.contains(QStringLiteral("defaultConfiguration")) == true)
        {
            outError = QStringLiteral("%1: only snippets take a defaultConfiguration.").arg(name);
            return false;
        }
        outTemplate = Move(result);
        return true;
    }
    if (entry.contains(QStringLiteral("label")) == true || entry.contains(QStringLiteral("description")) == true ||
        entry.contains(QStringLiteral("defaultConfiguration")) == true)
    {
        outError = QStringLiteral("%1: nodes are labelled by their kind and take no label, description or "
                                  "defaultConfiguration.")
                       .arg(name);
        return false;
    }
    const bool isUpscale = result.Identity == QStringLiteral("Upscale");
    if (isUpscale == false)
    {
        if (entry.contains(QStringLiteral("algorithm")) == true || entry.contains(QStringLiteral("isDraft")) == true)
        {
            outError = QStringLiteral("%1: only Upscale nodes take an algorithm or isDraft.").arg(name);
            return false;
        }
        result.Label = result.Identity;
        outTemplate = Move(result);
        return true;
    }
    if (ReadText(entry, QStringLiteral("algorithm"), result.Algorithm) == false ||
        GetUpscaleAlgorithms().contains(result.Algorithm) == false)
    {
        outError = QStringLiteral("%1 needs an algorithm from: %2.")
                       .arg(name, GetUpscaleAlgorithms().join(QStringLiteral(", ")));
        return false;
    }
    if (entry.contains(QStringLiteral("isDraft")) == true)
    {
        if (entry.value(QStringLiteral("isDraft")).isBool() == false)
        {
            outError = QStringLiteral("%1 isDraft must be true or false.").arg(name);
            return false;
        }
        result.IsDraft = entry.value(QStringLiteral("isDraft")).toBool();
    }
    result.Label = (result.IsDraft == true ? QStringLiteral("Upscale — %1 (draft)") : QStringLiteral("Upscale — %1"))
                       .arg(GetUpscaleLabel(result.Algorithm));
    outTemplate = Move(result);
    return true;
}
} // namespace

bool RenderGraphTemplateRegistry::Parse(const QByteArray& source,
                                        RenderGraphTemplateRegistry& outRegistry,
                                        QString& outError)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(source, &parseError);
    if (parseError.error != QJsonParseError::NoError || document.isObject() == false)
    {
        outError = QStringLiteral("The template catalog is not a JSON object: %1.").arg(parseError.errorString());
        return false;
    }
    const QJsonObject root = document.object();
    for (const QString& field : root.keys())
    {
        if (field != QStringLiteral("version") && field != QStringLiteral("templates"))
        {
            outError = QStringLiteral("The template catalog has an unknown field '%1'.").arg(field);
            return false;
        }
    }
    if (root.value(QStringLiteral("version")).isDouble() == false ||
        root.value(QStringLiteral("version")).toDouble() != static_cast<float64>(CATALOG_VERSION))
    {
        outError = QStringLiteral("The template catalog must declare version %1.").arg(CATALOG_VERSION);
        return false;
    }
    if (root.value(QStringLiteral("templates")).isArray() == false)
    {
        outError = QStringLiteral("The template catalog needs a templates array.");
        return false;
    }
    RenderGraphTemplateRegistry result;
    const QJsonArray entries = root.value(QStringLiteral("templates")).toArray();
    for (qsizetype i = 0; i < entries.size(); ++i)
    {
        const QString where = QStringLiteral("Template %1").arg(i + 1);
        if (entries.at(i).isObject() == false)
        {
            outError = QStringLiteral("%1 must be an object.").arg(where);
            return false;
        }
        RenderGraphTemplate entry;
        if (ReadEntry(entries.at(i).toObject(), where, entry, outError) == false)
        {
            return false;
        }
        for (const RenderGraphTemplate& existing : result.mTemplates)
        {
            const bool isSameEntry = existing.Kind == entry.Kind && existing.Identity == entry.Identity &&
                                     existing.Algorithm == entry.Algorithm;
            const bool isSameConfiguration = entry.Kind == RenderGraphTemplateKind::Snippet &&
                                             existing.Kind == RenderGraphTemplateKind::Snippet &&
                                             existing.DefaultConfiguration == entry.DefaultConfiguration;
            if (isSameEntry == true || isSameConfiguration == true)
            {
                outError = QStringLiteral("%1 '%2' repeats an earlier entry.").arg(where, entry.Identity);
                return false;
            }
        }
        result.mTemplates.append(entry);
    }
    outRegistry = Move(result);
    outError.clear();
    return true;
}

const RenderGraphTemplateRegistry& RenderGraphTemplateRegistry::GetBuiltIn()
{
    static const RenderGraphTemplateRegistry BUILT_IN_REGISTRY = []() {
        RenderGraphTemplateRegistry loaded;
        QFile file(QStringLiteral(":/render-graph/render_graph_templates.json"));
        QString error;
        if (file.open(QIODevice::ReadOnly) == false || Parse(file.readAll(), loaded, error) == false)
        {
            LUDUS_LOG_TEXT(LOG_RENDER_GRAPH_TEMPLATES,
                           Error,
                           "the built-in render graph template catalog is missing or invalid; no templates offered");
        }
        return loaded;
    }();
    return BUILT_IN_REGISTRY;
}

QVector<RenderGraphTemplate> RenderGraphTemplateRegistry::GetTemplates(RenderGraphTemplateKind kind) const
{
    QVector<RenderGraphTemplate> result;
    for (const RenderGraphTemplate& entry : mTemplates)
    {
        if (entry.Kind == kind)
        {
            result.append(entry);
        }
    }
    return result;
}

bool RenderGraphTemplateRegistry::HasTemplate(RenderGraphTemplateKind kind, const QString& identity) const
{
    for (const RenderGraphTemplate& entry : mTemplates)
    {
        if (entry.Kind == kind && entry.Identity == identity)
        {
            return true;
        }
    }
    return false;
}

bool RenderGraphTemplateRegistry::IsEmpty() const noexcept
{
    return mTemplates.isEmpty();
}
} // namespace ludus::editor
