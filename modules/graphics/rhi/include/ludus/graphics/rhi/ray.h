#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::rhi
{
/// Immutable native triangle BLAS in the shared resource registry. Copies borrow
/// identity; scenes and pending builds retain the exact incarnation after Destroy.
struct RayGeometryHandle final
{
private:
    ludus::foundation::uint64 Owner = 0, Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Private registry access; applications cannot construct resource identities.
    friend struct internal::RasterAccess;
};
/// Native query compute program with the fixed RT0 ray/hit/opacity ABI.
/// This profile uses SPIR-V RayQueryKHR or MSL 2.4 intersection_query; ordinary
/// raster shaders retain their existing target. Dedicated ray pipelines are absent.
struct RayQueryProgram final
{
private:
    ludus::foundation::uint64 Owner = 0, Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Private registry access; applications cannot construct resource identities.
    friend struct internal::RasterAccess;
};
/// Asynchronously submitted query/result version. Read only after Ready; no polling
/// or read operation blocks. Destroy detaches ownership while accepted GPU work lives.
struct RayQueryBatch final
{
private:
    ludus::foundation::uint64 Owner = 0, Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Private registry access; applications cannot construct resource identities.
    friend struct internal::RasterAccess;
};
/// Negotiated engine operations; unavailable fields are zero. Compute support alone
/// does not establish ray support. Browser backends expose an unavailable profile.
struct RayCapabilities final
{
    /// Native immutable triangle BLAS/TLAS builds on an admitted hardware ray device.
    bool StaticTriangles = false;
    /// Nearest accepted hit and early-exit accepted visibility with opacity filtering.
    bool InlineQueries = false;
    /// Dedicated ray shader stages; false for RT0.
    bool RayPipelines = false;
    /// Procedural intersections; false for RT0.
    bool ProceduralIntersections = false;
    /// Native geometry updates/refits; false for RT0.
    bool Updates = false;
    /// Motion geometry; false for RT0.
    bool Motion = false;
    /// Enabled maximum instances per scene, clamped from StorageLimits::RayInstances
    /// by backend/representation limits. Per-scene byte budgets may reject smaller scenes.
    ludus::foundation::uint32 MaxInstances = 0;
    /// Maximum rays per submitted batch; larger jobs must explicitly tile.
    ludus::foundation::uint32 MaxRays = 0;
    /// Maximum bytes for one native buffer, further bounded by each admission budget.
    ludus::foundation::usize MaxBufferBytes = 0;
};
/// Borrowed local triangle-list inputs, copied/consumed before creation returns.
/// Finite positions in meters and optional finite top-left UVs are tightly packed.
/// Nonzero indices form complete nondegenerate triangles. No deformation or refit.
struct RayGeometryDescription final
{
    /// Tightly packed XYZ, three finite float32 values per vertex in [-1000000,1000000] meters.
    const void* Positions = nullptr;
    /// Positive number of positions, representable by uint32.
    ludus::foundation::usize VertexCount = 0;
    /// Local uint32 triangle indices, all below VertexCount.
    const ludus::foundation::uint32* Indices = nullptr;
    /// Positive multiple of three, representable by uint32.
    ludus::foundation::usize IndexCount = 0;
    /// Optional tightly packed finite UV pairs in [-1000000,1000000]; null supplies zero UV.
    const void* Texcoords = nullptr;
    /// Explicit byte budget for resident inputs/BLAS and temporary build scratch.
    /// Zero is invalid. Admission checks actual native requirements before submission.
    ludus::foundation::usize ByteBudget = 0;
    /// Leading triangle count that bypasses opacity filtering even in a masked instance.
    /// Must not exceed IndexCount/3. Zero keeps every triangle eligible for filtering;
    /// a positive prefix and remaining suffix form native opaque/nonopaque ranges in
    /// one BLAS. Public hit Primitive still addresses the full original triangle list.
    /// Requires the matching SDK query shader when querying a partitioned geometry.
    ludus::foundation::uint32 OpaqueTriangleCount = 0;
};
/// Copied instance and opacity contract. Reference filtering uses a declared fixed
/// mip with nearest or bilinear alpha; it never borrows raster derivatives.
struct RayInstanceDescription final
{
    /// Ready exact BLAS; scene ownership retains it after public destruction.
    RayGeometryHandle Geometry{};
    /// Row-major local-to-world 3x4, finite and nonsingular; mirrored transforms work.
    ludus::foundation::float32 Transform[12]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    /// Nonzero ray visibility mask in the low eight bits.
    ludus::foundation::uint32 Mask = 255;
    /// Opacity filtering outside Geometry's opaque prefix; opaque instances ignore the alpha table.
    bool AlphaMasked = false;
    /// Scalar alpha in [0,1], multiplying sampled alpha before threshold comparison.
    ludus::foundation::float32 Opacity = 1;
    /// Threshold in [0,1]; equal opacity survives.
    ludus::foundation::float32 AlphaCutoff = 0.5F;
    /// UV scale XY and offset ZW, finite.
    ludus::foundation::float32 UvTransform[4]{1, 1, 0, 0};
    /// Borrowed initialized scalar alpha texels in [0,1], copied at scene creation.
    /// Null supplies one; otherwise Width*Height records are required.
    const ludus::foundation::float32* Alpha = nullptr;
    /// Fixed-mip physical width; zero only when Alpha is null.
    ludus::foundation::uint32 Width = 0;
    /// Fixed-mip physical height; zero only when Alpha is null.
    ludus::foundation::uint32 Height = 0;
    /// Bilinear fixed-mip opacity filtering; false selects nearest.
    bool LinearFilter = false;
    /// Periodic U addressing; false clamps to edge texels.
    bool RepeatU = false;
    /// Periodic V addressing; false clamps to edge texels.
    bool RepeatV = false;
};
/// Immutable complete declared ray scene, with no camera frustum pruning.
struct RaySceneDescription final
{
    /// Borrowed ordered instances; returned hit indices address this exact order.
    const RayInstanceDescription* Instances = nullptr;
    /// Count up to RayCapabilities::MaxInstances; zero creates an empty scene.
    ludus::foundation::usize Count = 0;
    /// Explicit TLAS/table/scratch budget in bytes, excluding retained BLAS versions.
    ludus::foundation::usize ByteBudget = 0;
};
/// Fixed 48-byte ray ABI; normalized world direction makes distance units meters.
/// Interval is [MinDistance,MaxDistance], finite, nonnegative and ordered.
struct RayInput final
{
    /// Finite world-space XYZ in [-1000000,1000000] meters.
    ludus::foundation::float32 Origin[3]{};
    /// Inclusive minimum distance in meters.
    ludus::foundation::float32 MinDistance = 0;
    /// Finite unit world direction, within 1e-5 squared-length tolerance.
    ludus::foundation::float32 Direction[3]{0, 0, -1};
    /// Inclusive maximum distance in meters.
    ludus::foundation::float32 MaxDistance = 1000000;
    /// Low eight visibility bits, nonzero.
    ludus::foundation::uint32 Mask = 255;
    /// Zero finds nearest accepted hit; one stops at any accepted hit for visibility.
    ludus::foundation::uint32 VisibilityOnly = 0;
    /// Must be zero; retained for a stable vector-aligned shader ABI.
    ludus::foundation::uint32 Reserved[2]{};
};
/// Fixed 32-byte result ABI. Misses are zero; visibility hits promise only Hit.
/// Closest hits provide local primitive/instance identities and barycentric weights
/// (1-U-V,U,V), with distance along the original normalized world ray.
struct RayResult final
{
    /// World distance in meters, valid only for a closest hit.
    ludus::foundation::float32 Distance = 0;
    /// Vertex-one barycentric weight.
    ludus::foundation::float32 U = 0;
    /// Vertex-two barycentric weight.
    ludus::foundation::float32 V = 0;
    /// Zero padding, not an application channel.
    ludus::foundation::float32 ReservedFloat = 0;
    /// Geometry-local triangle index.
    ludus::foundation::uint32 Primitive = 0;
    /// Index in the copied RaySceneDescription::Instances array.
    ludus::foundation::uint32 Instance = 0;
    /// One for accepted hit, zero for miss.
    ludus::foundation::uint32 Hit = 0;
    /// Zero padding, not an application channel.
    ludus::foundation::uint32 Reserved = 0;
};
/// Actual native allocation categories for one geometry, scene or batch version.
struct RayAllocation final
{
    /// Resident native structure, input/table and result bytes.
    ludus::foundation::usize ResidentBytes = 0;
    /// Required peak build scratch bytes; released when the build becomes Ready.
    ludus::foundation::usize ScratchBytes = 0;
};
/// Device-wide known ray resource bytes, including detached accepted incarnations.
/// Driver/pipeline objects and allocator backing-page overhead are unavailable,
/// rather than represented as measured zero. No synchronous GPU timing/readback.
struct RayResourceStats final
{
    /// Live/retained native buffer and acceleration-structure resource bytes.
    ludus::foundation::usize ResidentBytes = 0;
    /// Scratch for Pending builds only; Ready builds have released it.
    ludus::foundation::usize BuildScratchBytes = 0;
    /// Subset of ResidentBytes whose public ownership has detached.
    ludus::foundation::usize DetachedBytes = 0;
    /// Owned or retained BLAS incarnations.
    ludus::foundation::uint32 Geometries = 0;
    /// Owned or retained TLAS/table incarnations.
    ludus::foundation::uint32 Scenes = 0;
    /// Owned or retained query/result incarnations.
    ludus::foundation::uint32 Batches = 0;
};
/// Nonblocking collection followed by a device-wide known resource snapshot.
/// Failure preserves output; Unsupported means no negotiated native ray profile.
[[nodiscard]] RasterStatus GetRayResourceStats(DeviceHandle, RayResourceStats&) noexcept;
/// Query negotiated native ray operations without allocating or launching work.
/// Invalid owner preserves output; unavailable backend zeros it and returns Unsupported.
[[nodiscard]] RasterStatus GetRayCapabilities(DeviceHandle, RayCapabilities&) noexcept;
/// Create the fixed query program from the installed RT0 kernel artifact. Stage must
/// be Compute. Vulkan uses SPIR-V RayQueryKHR; Metal uses its cooked MSL 2.4 variant.
/// Owner-thread/between-frames only. Failure preserves null output.
[[nodiscard]] RasterStatus CreateRayQueryProgram(DeviceHandle, const ShaderDescription&, RayQueryProgram&) noexcept;
/// Asynchronously build a copied immutable BLAS. Ready/Pending publishes ownership;
/// failure preserves null output. Owner-thread/between-frames only.
[[nodiscard]] RasterStatus CreateRayGeometry(DeviceHandle, const RayGeometryDescription&, RayGeometryHandle&) noexcept;
/// Asynchronously build a TLAS/opacity table. Pending BLAS returns NotReady without
/// publishing output. Ready/Pending retains every exact input version. Between frames.
[[nodiscard]] RasterStatus CreateRayScene(DeviceHandle, const RaySceneDescription&, RaySceneHandle&) noexcept;
/// Submit a bounded immutable ray batch between frames. Copies every ray; retains
/// program/scene until completion. Count must be positive and within negotiated limits.
/// Pending dependencies return NotReady; other failures preserve null output.
[[nodiscard]] RasterStatus SubmitRayQueries(DeviceHandle,
                                            RayQueryProgram,
                                            RaySceneHandle,
                                            const RayInput* rays,
                                            ludus::foundation::usize count,
                                            RayQueryBatch&) noexcept;
/// Nonblocking poll of a BLAS build; stale/foreign identity returns InvalidHandle.
[[nodiscard]] RasterStatus GetStatus(DeviceHandle, RayGeometryHandle) noexcept;
/// Nonblocking poll of a TLAS build; stale/foreign identity returns InvalidHandle.
[[nodiscard]] RasterStatus GetStatus(DeviceHandle, RaySceneHandle) noexcept;
/// Query program readiness; stale/foreign identity returns InvalidHandle.
[[nodiscard]] RasterStatus GetStatus(DeviceHandle, RayQueryProgram) noexcept;
/// Nonblocking poll of an accepted query batch; failure still requires Destroy.
[[nodiscard]] RasterStatus GetStatus(DeviceHandle, RayQueryBatch) noexcept;
/// Copy completed query results into exactly Count records. NotReady/failure
/// preserves output; no mapping/wait occurs until the native completion is ready.
[[nodiscard]] RasterStatus
ReadRayResults(DeviceHandle, RayQueryBatch, RayResult*, ludus::foundation::usize count) noexcept;
/// Query native geometry byte categories; invalid identity preserves output.
[[nodiscard]] RasterStatus GetRayAllocation(DeviceHandle, RayGeometryHandle, RayAllocation&) noexcept;
/// Query native scene byte categories; invalid identity preserves output.
[[nodiscard]] RasterStatus GetRayAllocation(DeviceHandle, RaySceneHandle, RayAllocation&) noexcept;
/// Query native batch byte categories; invalid identity preserves output.
[[nodiscard]] RasterStatus GetRayAllocation(DeviceHandle, RayQueryBatch, RayAllocation&) noexcept;
/// Detach geometry ownership between frames. Scenes/builds retain accepted uses.
/// Invalid identity preserves handle; success clears it.
[[nodiscard]] RasterStatus Destroy(DeviceHandle, RayGeometryHandle&) noexcept;
/// Detach scene ownership between frames; pending batches keep the scene alive.
/// Invalid identity preserves handle; success clears it.
[[nodiscard]] RasterStatus Destroy(DeviceHandle, RaySceneHandle&) noexcept;
/// Detach program ownership between frames; pending batches retain its pipeline.
/// Invalid identity preserves handle; success clears it.
[[nodiscard]] RasterStatus Destroy(DeviceHandle, RayQueryProgram&) noexcept;
/// Detach batch ownership between frames; pending GPU work retires on completion.
/// Invalid identity preserves handle; success clears it. Does not cancel submitted rays.
[[nodiscard]] RasterStatus Destroy(DeviceHandle, RayQueryBatch&) noexcept;
} // namespace ludus::graphics::rhi
