#pragma once

#include <ludus/foundation/base/types.h>

#include <QHash>
#include <QJsonObject>
#include <QString>

namespace ludus::editor
{
class RenderGraphDocument;
/// Where the editor would execute a render graph. Unknown makes no device claim.
enum class RenderGraphDeviceKind : foundation::uint8
{
    Unknown, ///< No device yet (or no GPU preview in this build): only engine-profile limits are reported.
    Native,  ///< A desktop Metal/Vulkan viewport device whose enabled capabilities are recorded below.
    Browser, ///< The browser editor: render graphs never execute; the viewport uses its built-in renderer.
};
/// Capabilities of the device that previews render graphs, copied when the device starts.
/// Fields are meaningful only for Native; Browser and Unknown ignore them.
struct RenderGraphDeviceProfile final
{
    RenderGraphDeviceKind Kind = RenderGraphDeviceKind::Unknown;
    /// Ordinary compute with storage buffers: the triangle-loop, compute-BVH and custom traversals.
    bool HasCompute = false;
    /// Enabled native acceleration structures and inline ray queries (renderer selection admits
    /// HardwareRayTracing), required by NativeQuery traversal.
    bool HasNativeRayQueries = false;
    /// Native ray stage/function-table execution (rhi::RayCapabilities::RayPipelines), required by a
    /// RayTracingPipeline whose invocation type is the hardware RT pipeline.
    bool HasRayPipelines = false;
    /// The optional OIDN denoiser is linked; without it the preview bypasses Denoise nodes.
    bool HasDenoiser = false;
    /// Short device description for messages, for example "Metal" or "Vulkan"; may be empty.
    QString DeviceName;
};
/// The most demanding execution capability one authored node needs.
enum class RenderGraphRequirement : foundation::uint8
{
    None,             ///< Raster, resources, scene/view and image nodes.
    Denoiser,         ///< A Denoise node; bypassed without the optional denoiser.
    Compute,          ///< Software ray traversal or scene compute in ordinary GPU compute.
    NativeRayQueries, ///< NativeQuery traversal (the legacy RayTracingPipeline default).
    RayPipelines,     ///< Hardware RT pipeline invocation (native ray stages).
    BeyondProfile,    ///< Authored settings the current graph build profile rejects on every device.
};
/// Whether one node can run where the editor previews graphs.
enum class RenderGraphAvailability : foundation::uint8
{
    Available,   ///< Runs, or the device is unknown and nothing rules it out.
    Unavailable, ///< This device or editor cannot run it; another device can.
    Unsupported, ///< The engine's graph build profile does not support these settings yet.
};
/// The automatic repair the canvas context menu offers for an unavailable node.
enum class RenderGraphAvailabilityRepair : foundation::uint8
{
    None,           ///< No repair: the explanation names the next step.
    UseComputeBvh,  ///< Run the ray pass as a Compute invocation with Compute BVH traversal.
    AddSceneInputs, ///< Make a generic ComputePipeline a scene compute pass.
};
/// Read-only canvas annotation for one node of the displayed configuration.
struct RenderGraphAvailabilityFeedback final
{
    RenderGraphAvailability Availability = RenderGraphAvailability::Available;
    RenderGraphRequirement Requirement = RenderGraphRequirement::None;
    RenderGraphAvailabilityRepair Repair = RenderGraphAvailabilityRepair::None;
    /// Short badge text, for example "Needs RT hardware"; empty when available.
    QString Label;
    /// Cause and fix in full sentences; empty when available.
    QString Explanation;
};
/// Availability of one displayed configuration.
struct RenderGraphConfigurationAvailability final
{
    /// Annotations keyed by node ID; available nodes are omitted.
    QHash<QString, RenderGraphAvailabilityFeedback> Nodes;
    /// Enabled nodes that cannot run on this device or are beyond the engine profile.
    foundation::int32 UnavailableCount = 0;
    /// Configuration-wide notice for the canvas; empty when every node can run.
    QString Summary;
};

/// Requirement of one authored node, resolved exactly as the graph compiler resolves invocation
/// type and traversal (scripts/python/ludus_tools/render_graph_ray_invocation.py). Subgraph
/// instances return None; InspectRenderGraphAvailability summarizes their imports.
[[nodiscard]] RenderGraphRequirement GetRenderGraphNodeRequirement(const QJsonObject& node);
/// Compare the displayed configuration's enabled nodes with the device profile. Disabled nodes are
/// skipped (the compiler bypasses them); Subgraph instances report their import's unavailable nodes
/// within a bounded, cycle-safe walk. Unknown devices report only BeyondProfile settings.
/// GUI thread only; reads the draft, never changes document/history and performs no GPU work.
[[nodiscard]] RenderGraphConfigurationAvailability
InspectRenderGraphAvailability(const RenderGraphDocument& document,
                               const QString& file,
                               const QString& configuration,
                               const RenderGraphDeviceProfile& profile);
/// The node with the repair applied: invocation type Compute with ComputeBvh traversal and no
/// traversal source. Returns false and preserves outNode unless the node's repair is UseComputeBvh.
[[nodiscard]] bool MakeComputeBvhRayNode(const QJsonObject& node, QJsonObject& outNode);
} // namespace ludus::editor
