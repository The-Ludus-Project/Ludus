#pragma once

#include "internal/render_graph_node_ports.h"
#include "internal/render_graph_template_registry.h"

#include <QString>
#include <QVector>

namespace ludus::editor
{
class RenderGraphDocument;

/// One package graph offered as a Subgraph reference, with its default interface and insertion status.
struct RenderGraphQuickAddGraph final
{
    /// Graph kind; Identity is the package-relative asset path, DefaultConfiguration its default.
    RenderGraphTemplate Template;
    RenderGraphNodePorts Ports;
    /// Empty when insertion is allowed; otherwise explains a missing interface or dependency cycle.
    QString UnavailableReason;
};

/// Snapshot the loaded draft's graph assets for insertion into file#configuration. Includes unused
/// assets and disabled entries, without editing the document or reading additional files from disk.
[[nodiscard]] QVector<RenderGraphQuickAddGraph>
BuildRenderGraphQuickAddGraphs(const RenderGraphDocument& document, const QString& file, const QString& configuration);

/// Recheck an offered graph's exact default and dependencies before inserting it. Empty means allowed.
/// A changed default requires reopening Quick Add; failure never changes the document.
[[nodiscard]] QString CheckRenderGraphQuickAddGraph(const RenderGraphDocument& document,
                                                    const QString& file,
                                                    const QString& configuration,
                                                    const RenderGraphTemplate& entry);
} // namespace ludus::editor
