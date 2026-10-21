#pragma once

#include <ludus/foundation/base/types.h>

#include <QString>

namespace ludus::editor
{
class RenderGraphDocument;
struct RenderGraphLinkCheck;

/// Fixed-size defaults for newly authored targets and shared RenderSize nodes, in pixels.
inline constexpr foundation::int32 DEFAULT_RENDER_WIDTH = 640;
inline constexpr foundation::int32 DEFAULT_RENDER_HEIGHT = 480;

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
