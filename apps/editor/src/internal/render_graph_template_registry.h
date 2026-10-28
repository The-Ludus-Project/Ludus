#pragma once

#include <ludus/foundation/base/types.h>

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ludus::editor
{
/// How much of a graph a template creates.
enum class RenderGraphTemplateKind : foundation::uint8
{
    /// A complete graph template, or a package graph reference offered by Quick Add.
    Graph,
    /// A reusable configuration inserted into an open graph.
    Snippet,
    /// One node type for the Add Node controls.
    Node,
};

/// One validated catalog entry. Order within a kind is the order the editor offers them in.
struct RenderGraphTemplate final
{
    RenderGraphTemplateKind Kind = RenderGraphTemplateKind::Node;
    /// Catalog graph/snippet: tooling identity. Quick Add graph: package-relative asset path.
    /// Node: the authored node kind.
    QString Identity;
    /// Upscale presets only: the stable algorithm identity from GetUpscaleAlgorithms().
    QString Algorithm;
    /// Display text. Nodes use their kind, and Upscale presets derive it from GetUpscaleLabel().
    QString Label;
    /// What the template creates. Required for graphs and snippets; optional for nodes.
    QString Description;
    /// Grouping identity from the kind's fixed vocabulary.
    QString Category;
    /// Snippet: proposed configuration name. Quick Add graph: the asset default at palette opening.
    QString DefaultConfiguration;
    /// Capabilities the template needs to preview: native-ray-tracing (hardware ray queries),
    /// compute-ray-tracing (Metal or Vulkan compute running the software triangle loop), denoiser or metalfx.
    QStringList Requirements;
    /// Extra lowercase search terms, such as synonyms people type for this template.
    QStringList Keywords;
    /// True for presets whose backend is not executable on any current profile.
    bool IsDraft = false;
};

/// Category identities a kind may use, in the order pickers group them.
[[nodiscard]] QStringList GetRenderGraphTemplateCategories(RenderGraphTemplateKind kind);
/// Display name of a category identity, such as "Scene & view"; empty for unknown identities.
[[nodiscard]] QString GetRenderGraphTemplateCategoryLabel(const QString& category);
/// Display name of a requirement identity, such as "Native ray tracing"; empty for unknown identities.
[[nodiscard]] QString GetRenderGraphTemplateRequirementLabel(const QString& requirement);

/// Ordered catalog of built-in render graph templates: complete graphs, snippets and node types.
/// The editor's New Graph dialog, Apply Configuration Template dialog, Add Node controls and
/// Quick Add palette read it instead of keeping their own lists. The built-in catalog is the embedded
/// render_graph_templates.json resource.
class RenderGraphTemplateRegistry final
{
public:
    /// Parse and validate a catalog document. Unknown fields, kinds, categories, requirements or
    /// Upscale algorithms, missing required fields and duplicate entries are rejected with a
    /// message naming the entry. Failure leaves outRegistry unchanged.
    [[nodiscard]] static bool
    Parse(const QByteArray& source, RenderGraphTemplateRegistry& outRegistry, QString& outError);
    /// The embedded catalog, parsed on first use. An invalid resource logs one error and yields an
    /// empty registry; tests keep the shipped resource valid. GUI thread only.
    [[nodiscard]] static const RenderGraphTemplateRegistry& GetBuiltIn();

    /// Entries of one kind in catalog order.
    [[nodiscard]] QVector<RenderGraphTemplate> GetTemplates(RenderGraphTemplateKind kind) const;
    /// True when an entry of this kind has this identity.
    [[nodiscard]] bool HasTemplate(RenderGraphTemplateKind kind, const QString& identity) const;
    [[nodiscard]] bool IsEmpty() const noexcept;

private:
    QVector<RenderGraphTemplate> mTemplates;
};
} // namespace ludus::editor
