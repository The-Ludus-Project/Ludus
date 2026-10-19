#include "internal/render_graph_targets.h"

#include <ludus/content/render_graph_format/plan.h>

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace ludus::editor
{
using namespace foundation;
namespace format = content::render_graph_format;
namespace renderer = graphics::renderer;
namespace
{
/// Sidecar schema written by render_graph_execution.encode_resource_names.
constexpr int32 NAMES_VERSION = 1;
constexpr int32 HEX_BASE = 16;
constexpr int32 HEX_DIGITS_PER_BYTE = 2;

QString ToHex(BorrowedView<const uint8> bytes)
{
    QString text;
    for (usize i = 0; i < bytes.GetSize(); ++i)
    {
        text += QStringLiteral("%1").arg(bytes[i], HEX_DIGITS_PER_BYTE, HEX_BASE, QLatin1Char('0'));
    }
    return text;
}

QString DescribeLoad(format::Load load)
{
    switch (load)
    {
        case format::Load::Clear:
            return QStringLiteral("Clear");
        case format::Load::Load:
            return QStringLiteral("Load");
        case format::Load::Discard:
            return QStringLiteral("Discard");
    }
    return QStringLiteral("Unknown");
}

QString DescribeStore(format::Store store)
{
    return store == format::Store::Store ? QStringLiteral("Store") : QStringLiteral("Discard");
}

QString DescribeAspect(const QString& aspect)
{
    return aspect == QStringLiteral("color") ? QStringLiteral("colour") : aspect;
}

/// Admit the sidecar only when it describes exactly this plan and generation.
bool ReadNames(const renderer::RenderGraphPlan& plan,
               const QString& generation,
               const QByteArray& document,
               RenderGraphTargetCatalogue& inoutCatalogue,
               QString& outProblem)
{
    QJsonParseError error;
    const QJsonDocument parsed = QJsonDocument::fromJson(document, &error);
    const QJsonObject root = parsed.object();
    if (error.error != QJsonParseError::NoError || parsed.isObject() == false)
    {
        outProblem = QStringLiteral("the resource names file is not a JSON object");
        return false;
    }
    if (root.value(QStringLiteral("version")).toInt() != NAMES_VERSION)
    {
        outProblem = QStringLiteral("the resource names file has an unsupported version");
        return false;
    }
    if (root.value(QStringLiteral("generation")).toString() != generation)
    {
        outProblem = QStringLiteral("the resource names file belongs to another generation");
        return false;
    }
    const QJsonArray resources = root.value(QStringLiteral("resources")).toArray();
    if (static_cast<usize>(resources.size()) != plan.Resources.GetSize())
    {
        outProblem = QStringLiteral("the resource names file lists a different number of textures");
        return false;
    }
    QSet<QString> seen;
    for (qsizetype i = 0; i < resources.size(); ++i)
    {
        const QJsonObject entry = resources.at(i).toObject();
        const QString name = entry.value(QStringLiteral("name")).toString();
        const QJsonArray extent = entry.value(QStringLiteral("extent")).toArray();
        const format::Resource& resource = plan.Resources[static_cast<usize>(i)];
        const bool isDepth = entry.value(QStringLiteral("type")).toString() == QStringLiteral("DepthTexture");
        if (name.isEmpty() == true || seen.contains(name) == true || extent.size() != 2 ||
            extent.at(0).toInteger() != resource.Width || extent.at(1).toInteger() != resource.Height ||
            isDepth != (resource.Kind == format::Aspect::Depth))
        {
            outProblem = QStringLiteral("the resource names file does not match texture %1").arg(i);
            return false;
        }
        seen.insert(name);
        inoutCatalogue.Targets[i].Name = name;
    }
    for (const QJsonValue& value : root.value(QStringLiteral("operations")).toArray())
    {
        const QJsonObject object = value.toObject();
        RenderGraphOperation operation;
        operation.Id = object.value(QStringLiteral("id")).toString();
        operation.Kind = object.value(QStringLiteral("kind")).toString();
        const QJsonObject attachments = object.value(QStringLiteral("attachments")).toObject();
        // Colour before depth: the order a reader expects on a Raster node.
        for (const QString& aspect : {QStringLiteral("color"), QStringLiteral("depth"), QStringLiteral("texture")})
        {
            const QJsonObject use = attachments.value(aspect).toObject();
            if (use.isEmpty() == false && seen.contains(use.value(QStringLiteral("resource")).toString()) == true)
            {
                operation.Attachments.append({aspect,
                                              use.value(QStringLiteral("resource")).toString(),
                                              use.value(QStringLiteral("load")).toString(),
                                              use.value(QStringLiteral("store")).toString()});
            }
        }
        inoutCatalogue.Operations.append(operation);
    }
    for (const QJsonValue& value : root.value(QStringLiteral("cameras")).toArray())
    {
        const QJsonObject object = value.toObject();
        inoutCatalogue.Cameras.append(
            {object.value(QStringLiteral("id")).toString(), object.value(QStringLiteral("target")).toString()});
    }
    return true;
}

void AddNamedUses(RenderGraphTargetCatalogue& inoutCatalogue)
{
    for (const RenderGraphOperation& operation : inoutCatalogue.Operations)
    {
        for (const RenderGraphAttachmentUse& use : operation.Attachments)
        {
            const qsizetype index = inoutCatalogue.Find(use.Resource);
            if (index < 0)
            {
                continue;
            }
            const QString text =
                operation.Kind == QStringLiteral("Raster")
                    ? QStringLiteral("%1 attachment of %2 (%3 → %4)")
                          .arg(DescribeAspect(use.Aspect), operation.Id, use.Load, use.Store)
                    : QStringLiteral("%1 by %2")
                          .arg(operation.Kind == QStringLiteral("Export") ? QStringLiteral("exported")
                                                                          : QStringLiteral("presented"),
                               operation.Id);
            inoutCatalogue.Targets[index].Uses.append(text);
        }
    }
}

void AddNumericUses(const renderer::RenderGraphPlan& plan, RenderGraphTargetCatalogue& inoutCatalogue)
{
    for (usize p = 0; p < plan.Passes.GetSize(); ++p)
    {
        const format::Pass& pass = plan.Passes[p];
        if (pass.Color != format::ABSENT && pass.Color < plan.Resources.GetSize())
        {
            inoutCatalogue.Targets[static_cast<qsizetype>(pass.Color)].Uses.append(
                QStringLiteral("colour attachment of pass %1 (%2 → %3)")
                    .arg(p)
                    .arg(DescribeLoad(pass.ColorLoad), DescribeStore(pass.ColorStore)));
        }
        if (pass.Depth < plan.Resources.GetSize())
        {
            inoutCatalogue.Targets[static_cast<qsizetype>(pass.Depth)].Uses.append(
                QStringLiteral("depth attachment of pass %1 (%2 → %3)")
                    .arg(p)
                    .arg(DescribeLoad(pass.DepthLoad), DescribeStore(pass.DepthStore)));
        }
    }
    for (RenderGraphTarget& target : inoutCatalogue.Targets)
    {
        if (target.IsExported == true)
        {
            target.Uses.append(QStringLiteral("exported"));
        }
    }
}

void AppendUnique(QStringList& inoutNames, const QString& name)
{
    if (name.isEmpty() == false && inoutNames.contains(name) == false)
    {
        inoutNames.append(name);
    }
}
} // namespace

qsizetype RenderGraphTargetCatalogue::Find(const QString& name) const noexcept
{
    for (qsizetype i = 0; i < Targets.size(); ++i)
    {
        if (Targets[i].Name == name)
        {
            return i;
        }
    }
    return -1;
}

QString GetTargetNamesPath(const QString& runtimePackagePath)
{
    return runtimePackagePath + QStringLiteral(".resources.json");
}

RenderGraphTargetCatalogue MakeTargetCatalogue(const renderer::RenderGraphPlan& plan,
                                               BorrowedView<const uint8> generationIdentity,
                                               const QByteArray& namesDocument)
{
    RenderGraphTargetCatalogue catalogue;
    for (usize i = 0; i < plan.Resources.GetSize(); ++i)
    {
        const format::Resource& resource = plan.Resources[i];
        RenderGraphTarget target;
        target.Name = QStringLiteral("Resource %1").arg(i);
        target.Resource = static_cast<uint32>(i);
        target.Backing = resource.Backing;
        target.Width = resource.Width;
        target.Height = resource.Height;
        target.IsDepth = resource.Kind == format::Aspect::Depth;
        target.IsExported = resource.IsExported == 1;
        target.IsSurface = resource.IsSurface == 1;
        catalogue.Targets.append(target);
    }
    if (namesDocument.isEmpty() == false)
    {
        RenderGraphTargetCatalogue named = catalogue;
        QString problem;
        if (ReadNames(plan, ToHex(generationIdentity), namesDocument, named, problem) == true)
        {
            catalogue = named;
            catalogue.HasNames = true;
        }
        else
        {
            catalogue.NamesProblem = problem;
        }
    }
    if (catalogue.HasNames == true)
    {
        AddNamedUses(catalogue);
    }
    else
    {
        AddNumericUses(plan, catalogue);
    }
    for (RenderGraphTarget& target : catalogue.Targets)
    {
        if (target.Backing != target.Resource && target.Backing < static_cast<uint32>(catalogue.Targets.size()))
        {
            target.Uses.append(
                QStringLiteral("shares the allocation of %1").arg(catalogue.Targets[target.Backing].Name));
        }
        if (target.IsSurface == true)
        {
            target.Uses.append(QStringLiteral("presentation surface"));
        }
    }
    return catalogue;
}

QStringList
ResolveCanvasTargets(const RenderGraphTargetCatalogue& catalogue, const QString& graphId, const QString& nodeId)
{
    QStringList names;
    if (catalogue.HasNames == false || nodeId.isEmpty() == true)
    {
        return names;
    }
    const QString prefix = graphId + QLatin1Char('/');
    bool isRootDocument = false;
    for (const RenderGraphTarget& target : catalogue.Targets)
    {
        isRootDocument = isRootDocument || target.Name.startsWith(prefix) == true;
    }
    // A subgraph document opened on its own matches each instance by its node's last ID segment.
    const auto matches = [&](const QString& expanded) {
        return isRootDocument == true ? expanded == prefix + nodeId
                                      : expanded.endsWith(QLatin1Char('/') + nodeId) == true;
    };
    const auto isInside = [&](const QString& expanded) {
        const QString instance = prefix + nodeId + QLatin1Char('/');
        return isRootDocument == true ? expanded.startsWith(instance) == true
                                      : expanded.contains(QLatin1Char('/') + nodeId + QLatin1Char('/')) == true;
    };
    for (const RenderGraphTarget& target : catalogue.Targets)
    {
        if (matches(target.Name) == true)
        {
            AppendUnique(names, target.Name);
        }
    }
    for (const RenderGraphOperation& operation : catalogue.Operations)
    {
        if (matches(operation.Id) == true || isInside(operation.Id) == true)
        {
            for (const RenderGraphAttachmentUse& use : operation.Attachments)
            {
                AppendUnique(names, use.Resource);
            }
        }
    }
    for (const QPair<QString, QString>& camera : catalogue.Cameras)
    {
        if (matches(camera.first) == true)
        {
            AppendUnique(names, camera.second);
        }
    }
    return names;
}

QString DescribeTarget(const RenderGraphTarget& target)
{
    QStringList parts{QStringLiteral("%1 × %2").arg(target.Width).arg(target.Height),
                      target.IsDepth == true ? QStringLiteral("Depth32Float") : QStringLiteral("RGBA8Unorm"),
                      QStringLiteral("1 mip, 1 layer")};
    parts += target.Uses;
    if (target.IsExported == false && target.IsSurface == false)
    {
        parts.append(QStringLiteral("transient"));
    }
    return parts.join(QStringLiteral(" · "));
}
} // namespace ludus::editor
