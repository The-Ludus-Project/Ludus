#include "internal/import_report.h"

#include <QFileInfo>
#include <ludus/foundation/base/types.h>

#include <ludus/content/scene_format/luscn.h>

#include <QJsonArray>
#include <QStringList>

namespace ludus::editor
{
namespace
{
namespace format = content::scene_format;
/// Reports written before the cooked format version was recorded describe version 4 bundles.
constexpr foundation::int64 UNRECORDED_SCENE_FORMAT_VERSION = format::VERSION_4;

[[nodiscard]] QString FormatCount(const QJsonObject& object, const QString& key)
{
    return QString::number(object.value(key).toInteger());
}

[[nodiscard]] QString FormatMebibytes(foundation::int64 bytes)
{
    return QStringLiteral("%1 MiB").arg(static_cast<foundation::float64>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
}

[[nodiscard]] QString DescribeBestEffortReport(const QJsonObject& report)
{
    const QJsonObject source = report.value(QStringLiteral("source")).toObject();
    const QJsonObject cooked = report.value(QStringLiteral("cooked")).toObject();
    const QJsonObject lights = source.value(QStringLiteral("lights")).toObject();
    foundation::int64 otherLights = 0;
    for (const QString& key :
         {QStringLiteral("point"), QStringLiteral("spot"), QStringLiteral("distant"), QStringLiteral("other")})
    {
        otherLights += lights.value(key).toInteger();
    }
    QStringList lines;
    lines.append(QStringLiteral("Importer: %1 (%2)")
                     .arg(report.value(QStringLiteral("importer")).toString(),
                          report.value(QStringLiteral("entry")).toString()));
    lines.append(QStringLiteral("Source: %1 files, %2 placements, %3 vertices, %4 materials, %5 textures")
                     .arg(FormatCount(source, QStringLiteral("files")),
                          FormatCount(source, QStringLiteral("placements")),
                          FormatCount(source, QStringLiteral("vertices")),
                          FormatCount(source, QStringLiteral("materials")),
                          FormatCount(source, QStringLiteral("textures"))));
    lines.append(QStringLiteral("Source lights: %1 area (emissive), %2 environment, %3 punctual/other")
                     .arg(FormatCount(lights, QStringLiteral("area")),
                          FormatCount(lights, QStringLiteral("infinite")),
                          QString::number(otherLights)));
    const QJsonObject environment = report.value(QStringLiteral("environment")).toObject();
    const QString environmentFile = environment.value(QStringLiteral("file")).toString();
    lines.append(environmentFile.isEmpty() == false
                     ? QStringLiteral("Environment: %1 (%2)")
                           .arg(environmentFile, environment.value(QStringLiteral("fallback")).toString())
                     : QStringLiteral("Environment: source lighting"));
    const foundation::int64 vertices = cooked.value(QStringLiteral("vertices")).toInteger();
    const foundation::int64 bytes = cooked.value(QStringLiteral("bundle_bytes")).toInteger();
    // Runtime limits of the cooked LUSCN version, shown beside the cooked counts.
    const foundation::int64 version =
        cooked.value(QStringLiteral("scene_format_version")).toInteger(UNRECORDED_SCENE_FORMAT_VERSION);
    const format::BundleLimits& limits =
        format::GetBundleLimits(format::IsSupportedVersion(static_cast<foundation::uint32>(version)) == true
                                    ? static_cast<foundation::uint32>(version)
                                    : format::VERSION_4);
    lines.append(QStringLiteral("Cooked: %1/%2 meshes, %3/%4 materials, %5/%6 textures, %7 objects")
                     .arg(FormatCount(cooked, QStringLiteral("meshes")),
                          QString::number(limits.Meshes),
                          FormatCount(cooked, QStringLiteral("materials")),
                          QString::number(limits.Materials),
                          FormatCount(cooked, QStringLiteral("textures")),
                          QString::number(limits.Textures),
                          FormatCount(cooked, QStringLiteral("objects"))));
    const bool isDryRun = report.value(QStringLiteral("dry_run")).toBool();
    const QString geometryBudget = version >= format::VERSION_5
                                       ? QStringLiteral("%1 vertices (bundle byte budget)").arg(vertices)
                                       : QStringLiteral("%1 of %2 vertices").arg(vertices).arg(limits.Vertices);
    lines.append(QStringLiteral("Budget: %1; %2; %3 placements dropped")
                     .arg(geometryBudget,
                          isDryRun == true
                              ? QStringLiteral("bundle size measured on import")
                              : QStringLiteral("%1 of %2")
                                    .arg(FormatMebibytes(bytes),
                                         FormatMebibytes(static_cast<foundation::int64>(limits.BundleBytes))),
                          FormatCount(cooked, QStringLiteral("dropped_placements"))));
    QStringList fallbacks;
    for (const QJsonValue value : report.value(QStringLiteral("diagnostics")).toArray())
    {
        const QJsonObject diagnostic = value.toObject();
        const QString severity = diagnostic.value(QStringLiteral("severity")).toString();
        if (severity == QStringLiteral("info"))
        {
            continue;
        }
        fallbacks.append(QStringLiteral("  %1 %2 ×%3: %4")
                             .arg(severity,
                                  diagnostic.value(QStringLiteral("code")).toString(),
                                  FormatCount(diagnostic, QStringLiteral("count")),
                                  diagnostic.value(QStringLiteral("message")).toString()));
    }
    lines.append(fallbacks.isEmpty() == true ? QStringLiteral("Fallbacks: none")
                                             : QStringLiteral("Fallbacks:\n") + fallbacks.join(QLatin1Char('\n')));
    return lines.join(QLatin1Char('\n'));
}

[[nodiscard]] QString DescribeReferenceReport(const QJsonObject& manifest)
{
    QStringList lines;
    lines.append(
        QStringLiteral("Importer: pbrt-reference-v1 (%1)").arg(manifest.value(QStringLiteral("source")).toString()));
    lines.append(QStringLiteral("Source: %1 PLY meshes, %2 triangles")
                     .arg(FormatCount(manifest, QStringLiteral("source_ply_meshes")),
                          FormatCount(manifest, QStringLiteral("source_triangles"))));
    lines.append(QStringLiteral("Cooked: %1 meshes, %2 materials, %3 objects, %4 triangles, %5")
                     .arg(FormatCount(manifest, QStringLiteral("meshes")),
                          FormatCount(manifest, QStringLiteral("materials")),
                          FormatCount(manifest, QStringLiteral("items")),
                          FormatCount(manifest, QStringLiteral("cooked_triangles")),
                          FormatMebibytes(manifest.value(QStringLiteral("bytes")).toInteger())));
    QStringList approximations;
    for (const QJsonValue value : manifest.value(QStringLiteral("approximations")).toArray())
    {
        approximations.append(QStringLiteral("  ") + value.toString());
    }
    lines.append(approximations.isEmpty() == true
                     ? QStringLiteral("Fallbacks: none")
                     : QStringLiteral("Fallbacks:\n") + approximations.join(QLatin1Char('\n')));
    return lines.join(QLatin1Char('\n'));
}
} // namespace

ImportSource SplitImportSource(const QString& path)
{
    const QFileInfo info(path);
    if (info.isDir() == true)
    {
        return {info.absoluteFilePath(), {}};
    }
    return {info.absolutePath(), info.fileName()};
}

QString DescribeImportReport(const QJsonObject& report)
{
    // pbrt-best-effort-v1, gltf-best-effort-v1 and obj-best-effort-v1 share one report schema.
    return report.value(QStringLiteral("importer")).toString().endsWith(QStringLiteral("-best-effort-v1"))
               ? DescribeBestEffortReport(report)
               : DescribeReferenceReport(report);
}

QString DescribeImportPreview(const QJsonObject& response)
{
    QString text = DescribeImportReport(response.value(QStringLiteral("report")).toObject());
    const QString rejection = response.value(QStringLiteral("reference_rejection")).toString();
    if (rejection.isEmpty() == false)
    {
        text += QStringLiteral("\nReference profile declined this scene (%1); using best effort.").arg(rejection);
    }
    return text;
}

QString DescribeImportReports(const QJsonObject& response)
{
    const QJsonObject reports = response.value(QStringLiteral("import_reports")).toObject();
    QStringList blocks;
    for (QJsonObject::const_iterator entry = reports.constBegin(); entry != reports.constEnd(); ++entry)
    {
        blocks.append(QStringLiteral("Import %1\n%2").arg(entry.key(), DescribeImportReport(entry.value().toObject())));
    }
    return blocks.join(QStringLiteral("\n\n"));
}
} // namespace ludus::editor
