#pragma once

#include <ludus/foundation/base/types.h>

#include <QSize>
#include <QString>

namespace ludus::editor
{
class RenderGraphDocument;
struct RenderGraphLinkCheck;

/// Fixed-size defaults for newly authored targets and shared RenderSize nodes, in pixels.
inline constexpr foundation::int32 DEFAULT_RENDER_WIDTH = 640;
inline constexpr foundation::int32 DEFAULT_RENDER_HEIGHT = 480;

/// Read-only inferred size and texture contract for canvas presentation.
struct RenderGraphResourceFeedback final
{
    QString SizeLabel;
    QString Description;
};
/// Describe an output endpoint through fixed/shared sizes, texture metadata, writable
/// pipeline targets and subgraph bindings. Missing/cyclic inputs remain unresolved.
/// View policies use the current drawable reference in physical pixels with runtime ceil
/// rounding and extent bounds; an empty reference keeps the policy symbolic. A library
/// inspected alone has no caller bindings. Never allocates GPU resources or edits drafts.
[[nodiscard]] RenderGraphResourceFeedback DescribeRenderGraphResource(const RenderGraphDocument& document,
                                                                      const QString& file,
                                                                      const QString& configuration,
                                                                      const QString& endpoint,
                                                                      const QSize& referenceSize);

/// Check newly provable texture-contract violations after replacing one input connection.
/// Resolves fixed/shared extents and paired resource identities through subgraph bindings.
/// Existing unrelated problems and unresolved draft inputs do not reject the proposed edit;
/// bounded inference defers unknown contracts to the build validator. Does not modify drafts.
[[nodiscard]] RenderGraphLinkCheck CheckRenderGraphTextureContracts(const RenderGraphDocument& document,
                                                                    const QString& file,
                                                                    const QString& configuration,
                                                                    const QString& from,
                                                                    const QString& to);
} // namespace ludus::editor
