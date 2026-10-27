#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

// Names, formats and uses of the textures in one cooked render-graph generation, for the
// texture viewer. The runtime package is numeric; the cooker writes `<package>.resources.json`
// beside it (scripts/python/ludus_tools/render_graph_execution.py encode_resource_names), which
// is admitted only when its generation identity matches the package. Without it every texture is
// still listed, as "Resource N". Canvas nodes resolve to textures through the same names.
namespace ludus::editor
{
/// One attachment an operation writes or reads, by resource name.
struct RenderGraphAttachmentUse final
{
    QString Aspect;   ///< Sidecar key: "color", "depth" or "texture" (an Export or Present root).
    QString Resource; ///< Expanded resource name, `<graph id>/<target id>` with subgraph instances.
    QString Load;     ///< Raster load operation; empty for roots.
    QString Store;    ///< Raster store operation; empty for roots.
};

/// One expanded graph operation: a Raster pass or an Export/Present root.
struct RenderGraphOperation final
{
    QString Id;   ///< Expanded node ID, for example "Main/raster/draw".
    QString Kind; ///< "Raster", "Export" or "Present".
    QVector<RenderGraphAttachmentUse> Attachments;
};

/// One inspectable logical texture of a generation.
struct RenderGraphTarget final
{
    QString Name;                    ///< Expanded resource name, or "Resource N" without names.
    foundation::uint32 Resource = 0; ///< Logical resource index in the plan.
    foundation::uint32 Backing = 0;  ///< Index of the resource owning the allocation.
    foundation::uint32 Width = 0;
    foundation::uint32 Height = 0;
    bool IsDepth = false;
    bool IsBuffer = false; ///< Persistent storage has metadata but is not a texture-viewer image.
    bool IsExported = false;
    bool IsSurface = false; ///< Backed by the presentation surface; never inspectable.
    QStringList Uses;       ///< Human-readable uses, in execution order.
};

/// Every texture of one generation, with operation and camera names when known.
struct RenderGraphTargetCatalogue final
{
    QVector<RenderGraphTarget> Targets;
    QVector<RenderGraphOperation> Operations;
    /// Expanded camera ID to the resource name it renders to.
    QVector<QPair<QString, QString>> Cameras;
    /// True when a matching names sidecar supplied resource names.
    bool HasNames = false;
    /// True only when the sidecar also binds the exact runtime bytes, distinguishing configurations.
    bool HasExactRuntimeNames = false;
    /// Why the names sidecar was not used; empty when HasNames or when none was found.
    QString NamesProblem;

    /// Index into Targets of a resource name; -1 when absent.
    [[nodiscard]] qsizetype Find(const QString& name) const noexcept;
};

/// Path of the names sidecar the cooker writes beside a runtime package.
[[nodiscard]] QString GetTargetNamesPath(const QString& runtimePackagePath);

/// Build the catalogue of a decoded plan. A names document is admitted only when it is version 1,
/// names exactly the plan's resources with matching extents and kinds, and carries the same
/// generation identity; otherwise NamesProblem says why and the numeric names are used.
/// @param generationIdentity RenderGraphPackage::GetGenerationIdentity of the same package.
/// @param namesDocument Sidecar bytes, or empty when no sidecar exists.
/// @param runtimeIdentity Optional hex SHA-256 of decoded runtime bytes. A conflicting binding
/// rejects names; a missing legacy binding keeps HasNames but never HasExactRuntimeNames.
[[nodiscard]] RenderGraphTargetCatalogue
MakeTargetCatalogue(const graphics::renderer::RenderGraphPlan& plan,
                    foundation::BorrowedView<const foundation::uint8> generationIdentity,
                    const QByteArray& namesDocument,
                    const QByteArray& runtimeIdentity = {});

/// Resource names a canvas node shows, in a stable order: a Target its own texture; a Raster
/// node the attachments its pass writes (colour, then depth); a Subgraph node the final
/// attachments of every pass inside it; an Export or Present node the texture it publishes;
/// a Camera node its destination target. Graph IDs are the canvas document's graph ID; nodes
/// of a document opened on its own (a subgraph file) also match every instance of it.
/// @return Empty without names or for nodes that touch no texture (Scene, inputs).
[[nodiscard]] QStringList
ResolveCanvasTargets(const RenderGraphTargetCatalogue& catalogue, const QString& graphId, const QString& nodeId);

/// Info line: resolution, format and uses, for example
/// "640 × 360 · RGBA8Unorm · colour attachment of Main/raster/draw (Clear → Store) · exported".
[[nodiscard]] QString DescribeTarget(const RenderGraphTarget& target);
} // namespace ludus::editor
