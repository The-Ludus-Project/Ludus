// CookGeometry: validate a GeometrySource, dry-run the FoundationGeometry
// preparation so a cook never emits a blob that cannot load, then serialise the
// LUGEO blob field by field in little-endian order and compute its digests. The
// cook is cooperatively cancellable. Mirrors modules/content/scene_cook's
// field-by-field ByteWriter packing and source-digest discipline. See
// docs/architecture/curves-surfaces.md and ADR 0024.

#include <ludus/geometry/content/geometry_content.h>

#include <ludus/content/content.h>
#include <ludus/foundation/containers/static_array.hpp>

#include "internal/byte_codec.h"
#include "internal/diagnostics.h"

namespace ludus::geometry::content
{
using ludus::content::Status;
using namespace ludus::geometry::content::internal;

namespace
{
[[nodiscard]] bool IsCancelled(const Cancellation* cancelOrNull) noexcept
{
    return cancelOrNull != nullptr && cancelOrNull->Requested != nullptr &&
           cancelOrNull->Requested(cancelOrNull->ContextOrNull);
}

/// Append one path span record (field by field).
[[nodiscard]] bool
WriteSpan(ByteWriter& inoutWriter, const geom::PreparedSpan& span, const math::Vector3d& origin) noexcept
{
    const uint32 kindTag = span.Kind == geom::SpanKind::Line ? static_cast<uint32>(fg::SpanKindTag::Line)
                                                             : static_cast<uint32>(fg::SpanKindTag::Cubic);
    (void)origin;
    bool isOk = inoutWriter.WriteUint32(kindTag) && inoutWriter.WriteUint32(static_cast<uint32>(span.SourceElement)) &&
                inoutWriter.WriteFloat64(span.DomainWidth);
    for (usize c = 0; c < fg::SPAN_CONTROL_COUNT && isOk; ++c)
    {
        // Prepared controls are already chunk-relative float32; store them directly.
        isOk = inoutWriter.WriteFloat32(span.Control[c].X) && inoutWriter.WriteFloat32(span.Control[c].Y) &&
               inoutWriter.WriteFloat32(span.Control[c].Z);
    }
    return isOk;
}

[[nodiscard]] bool WriteJoint(ByteWriter& inoutWriter, const geom::JointContinuity& joint) noexcept
{
    return inoutWriter.WriteFloat64(joint.PositionGap) && inoutWriter.WriteFloat64(joint.DirectionAngle) &&
           inoutWriter.WriteFloat64(joint.DerivativeDifference) &&
           inoutWriter.WriteFloat64(joint.SecondDerivativeDifference) && inoutWriter.WriteUint8(joint.IsC0 ? 1 : 0) &&
           inoutWriter.WriteUint8(joint.IsG1 ? 1 : 0) && inoutWriter.WriteUint8(joint.IsC1 ? 1 : 0) &&
           inoutWriter.WriteUint8(joint.IsC2 ? 1 : 0) && inoutWriter.WriteUint8(0) && inoutWriter.WriteUint8(0) &&
           inoutWriter.WriteUint8(0) && inoutWriter.WriteUint8(0);
}

/// One section directory entry assembled while writing.
struct PendingSection final
{
    fg::SectionId Id = fg::SectionId::PathSpans;
    uint32 Offset = 0;
    uint32 Count = 0;
    uint32 ElementBytes = 0;
};

/// Serialize the header with placeholder section offsets and digests, remembering
/// where to back-patch them.
struct HeaderPlan final
{
    fg::GeometryKindTag Kind = fg::GeometryKindTag::Path;
    fg::PrecisionProfileTag Precision = fg::PrecisionProfileTag::Visual;
    uint32 Flags = 0;
    uint32 SpanCount = 0;
    uint32 JointCount = 0;
    uint32 KeyCount = 0;
    uint32 PatchCount = 0;
    uint32 AdjacencyCount = 0;
    uint32 SourceIdentityCount = 0;
    uint64 Revision = 0;
    math::Vector3d Origin{};
    float64 AssetScale = 0.0;
    float64 PositionFloor = 0.0;
    float64 EffectivePositionError = 0.0;
    geom::ToleranceProfile Tolerance{};
    core::StaticArray<PendingSection, fg::MAX_SECTIONS> Sections{};
    uint32 SectionCount = 0;
};

[[nodiscard]] bool WriteToleranceFields(ByteWriter& inoutWriter, const geom::ToleranceProfile& t) noexcept
{
    return inoutWriter.WriteFloat64(t.PositionError) && inoutWriter.WriteFloat64(t.RelativePositionError) &&
           inoutWriter.WriteFloat64(t.DistanceError) && inoutWriter.WriteFloat64(t.TangentAngleError) &&
           inoutWriter.WriteFloat64(t.ProjectionGap) && inoutWriter.WriteFloat64(t.ContinuityPosition) &&
           inoutWriter.WriteFloat64(t.ContinuityDerivative) && inoutWriter.WriteFloat64(t.ContinuityAngle) &&
           inoutWriter.WriteFloat64(t.NormalMinDerivativeLength) && inoutWriter.WriteFloat64(t.NormalMinSinAngle) &&
           inoutWriter.WriteFloat64(t.NormalAngleError) && inoutWriter.WriteFloat64(t.CoincidenceDistance);
}

/// Write the fixed header prefix (digests and section offsets are patched later).
[[nodiscard]] bool WriteHeaderPrefix(ByteWriter& inoutWriter, const HeaderPlan& plan) noexcept
{
    bool isOk = inoutWriter.WriteBytes(
        std::span<const uint8>(reinterpret_cast<const uint8*>(fg::MAGIC.data()), fg::MAGIC.size()));
    isOk = isOk && inoutWriter.WriteUint32(fg::FORMAT_VERSION) && inoutWriter.WriteUint32(fg::COOKER_VERSION) &&
           inoutWriter.WriteUint32(static_cast<uint32>(plan.Kind)) &&
           inoutWriter.WriteUint32(static_cast<uint32>(plan.Precision)) && inoutWriter.WriteUint32(plan.Flags) &&
           inoutWriter.WriteUint32(plan.SpanCount) && inoutWriter.WriteUint32(plan.JointCount) &&
           inoutWriter.WriteUint32(plan.KeyCount) && inoutWriter.WriteUint32(plan.PatchCount) &&
           inoutWriter.WriteUint32(plan.AdjacencyCount) && inoutWriter.WriteUint32(plan.SourceIdentityCount) &&
           inoutWriter.WriteUint64(plan.Revision) && inoutWriter.WriteFloat64(plan.Origin.X) &&
           inoutWriter.WriteFloat64(plan.Origin.Y) && inoutWriter.WriteFloat64(plan.Origin.Z) &&
           inoutWriter.WriteFloat64(plan.AssetScale) && inoutWriter.WriteFloat64(plan.PositionFloor) &&
           inoutWriter.WriteFloat64(plan.EffectivePositionError) && WriteToleranceFields(inoutWriter, plan.Tolerance) &&
           inoutWriter.WriteUint32(plan.SectionCount);
    // Directory entries: ids and widths are known now; offsets are patched after
    // the payload is written, so write placeholder zero offsets here.
    for (uint32 i = 0; i < plan.SectionCount && isOk; ++i)
    {
        const PendingSection& section = plan.Sections[i];
        isOk = inoutWriter.WriteUint32(static_cast<uint32>(section.Id)) && inoutWriter.WriteUint32(0) &&
               inoutWriter.WriteUint32(section.Count) && inoutWriter.WriteUint32(section.ElementBytes);
    }
    return isOk;
}

/// Patch a section's offset in the already-written header directory.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): directory index and byte offset have distinct wire roles.
void PatchSectionOffset(ByteWriter& inoutWriter, uint32 sectionIndex, uint32 offset) noexcept
{
    // Directory starts right after SectionCount in the fixed prefix.
    const usize directoryStart = fg::HEADER_FIXED_BYTES;
    const usize entryStart = directoryStart + static_cast<usize>(sectionIndex) * fg::SECTION_ENTRY_BYTES;
    const usize offsetField = entryStart + sizeof(uint32); // after Id
    (void)core::TryWriteLittleEndian<uint32>(offset, inoutWriter.GetMutableBytes().subspan(offsetField));
}

/// Patch a 32-byte digest at a known offset.
void PatchDigest(ByteWriter& inoutWriter, usize offset, const ludus::content::Digest& digest) noexcept
{
    std::span<uint8> bytes = inoutWriter.GetMutableBytes();
    for (usize i = 0; i < fg::DIGEST_BYTES; ++i)
    {
        bytes[offset + i] = digest.Data[i];
    }
}

[[nodiscard]] Status CookPath(const GeometrySource& source,
                              const geom::PreparedPath& prepared,
                              core::Array<uint8>& outBlob,
                              const Cancellation* cancelOrNull,
                              GeometryDiagnosticReport& outDiag) noexcept
{
    const geom::PathView view = prepared.GetView();
    HeaderPlan plan{};
    plan.Kind = fg::GeometryKindTag::Path;
    plan.Precision = source.Precision == geom::PrecisionProfile::Strict ? fg::PrecisionProfileTag::Strict
                                                                        : fg::PrecisionProfileTag::Visual;
    plan.Flags = view.IsClosed ? static_cast<uint32>(fg::GeometryFlag::IsClosed) : 0U;
    plan.SpanCount = static_cast<uint32>(view.Spans.GetSize());
    plan.JointCount = static_cast<uint32>(view.Joints.GetSize());
    plan.Revision = view.Revision;
    plan.Origin = view.Origin;
    plan.AssetScale = source.AssetScale;
    plan.PositionFloor = view.PositionFloor;
    plan.EffectivePositionError = view.EffectivePositionError;
    plan.Tolerance = view.Tolerance;
    plan.Sections[0] = PendingSection{fg::SectionId::PathSpans, 0, plan.SpanCount, fg::PATH_SPAN_RECORD_BYTES};
    plan.Sections[1] = PendingSection{fg::SectionId::PathJoints, 0, plan.JointCount, fg::JOINT_RECORD_BYTES};
    plan.SectionCount = 2;

    ByteWriter writer;
    if (WriteHeaderPrefix(writer, plan) == false)
    {
        return Fail(outDiag,
                    Status::OutOfMemory,
                    geom::GeometryStatus::OutOfMemory,
                    "Header",
                    RepairForStatus(geom::GeometryStatus::OutOfMemory));
    }
    // Reserve the three trailing digests (written as zero now, patched later).
    const usize digestsOffset = writer.GetSize();
    for (usize i = 0; i < fg::HEADER_DIGEST_BYTES; ++i)
    {
        if (writer.WriteUint8(0) == false)
        {
            return Fail(outDiag,
                        Status::OutOfMemory,
                        geom::GeometryStatus::OutOfMemory,
                        "Header",
                        RepairForStatus(geom::GeometryStatus::OutOfMemory));
        }
    }

    const uint32 spanOffset = static_cast<uint32>(writer.GetSize());
    for (usize i = 0; i < view.Spans.GetSize(); ++i)
    {
        if (IsCancelled(cancelOrNull))
        {
            return Fail(outDiag,
                        Status::Cancelled,
                        geom::GeometryStatus::Cancelled,
                        "PathSpans",
                        "Cancelled by the caller.");
        }
        if (WriteSpan(writer, view.Spans[i], view.Origin) == false)
        {
            return Fail(outDiag,
                        Status::OutOfMemory,
                        geom::GeometryStatus::OutOfMemory,
                        "PathSpans",
                        RepairForStatus(geom::GeometryStatus::OutOfMemory));
        }
    }
    const uint32 jointOffset = static_cast<uint32>(writer.GetSize());
    for (usize i = 0; i < view.Joints.GetSize(); ++i)
    {
        if (WriteJoint(writer, view.Joints[i]) == false)
        {
            return Fail(outDiag,
                        Status::OutOfMemory,
                        geom::GeometryStatus::OutOfMemory,
                        "PathJoints",
                        RepairForStatus(geom::GeometryStatus::OutOfMemory));
        }
    }
    PatchSectionOffset(writer, 0, spanOffset);
    PatchSectionOffset(writer, 1, jointOffset);

    // Digests: payload is everything after the header; source digest is the
    // canonical source bytes.
    const usize headerBytes = fg::GetHeaderBytes(plan.SectionCount);
    const std::span<const uint8> payload = writer.GetMutableBytes().subspan(headerBytes);
    const ludus::content::Digest payloadDigest = ludus::content::Hash(payload);
    ludus::content::Bytes canonicalSource;
    GeometryDiagnosticReport sourceDiag{};
    const Status sourceStatus = WriteGeometrySource(source, canonicalSource, sourceDiag);
    if (sourceStatus != Status::Ok)
    {
        outDiag = sourceDiag;
        return sourceStatus;
    }
    const ludus::content::Digest sourceDigest = ludus::content::Hash(canonicalSource.Data());
    const ludus::content::Digest dependencyDigest = ludus::content::Hash(std::span<const uint8>{});
    PatchDigest(writer, digestsOffset, sourceDigest);
    PatchDigest(writer, digestsOffset + fg::DIGEST_BYTES, dependencyDigest);
    PatchDigest(writer, digestsOffset + 2 * fg::DIGEST_BYTES, payloadDigest);

    if (writer.Release(outBlob) == false)
    {
        return Fail(outDiag,
                    Status::OutOfMemory,
                    geom::GeometryStatus::OutOfMemory,
                    "Blob",
                    RepairForStatus(geom::GeometryStatus::OutOfMemory));
    }
    Succeed(outDiag);
    return Status::Ok;
}

[[nodiscard]] Status CookTrack(const GeometrySource& source,
                               const geom::PreparedTrack& prepared,
                               core::Array<uint8>& outBlob,
                               const Cancellation* cancelOrNull,
                               GeometryDiagnosticReport& outDiag) noexcept
{
    (void)prepared;
    HeaderPlan plan{};
    plan.Kind = fg::GeometryKindTag::Track;
    plan.Precision = source.Precision == geom::PrecisionProfile::Strict ? fg::PrecisionProfileTag::Strict
                                                                        : fg::PrecisionProfileTag::Visual;
    plan.KeyCount = static_cast<uint32>(source.Keys.GetSize());
    plan.Revision = source.Revision;
    plan.AssetScale = source.AssetScale;
    plan.Tolerance = source.Tolerance;
    plan.Sections[0] = PendingSection{fg::SectionId::TrackKeys, 0, plan.KeyCount, fg::TRACK_KEY_RECORD_BYTES};
    plan.SectionCount = 1;

    ByteWriter writer;
    if (WriteHeaderPrefix(writer, plan) == false)
    {
        return Fail(outDiag,
                    Status::OutOfMemory,
                    geom::GeometryStatus::OutOfMemory,
                    "Header",
                    RepairForStatus(geom::GeometryStatus::OutOfMemory));
    }
    const usize digestsOffset = writer.GetSize();
    for (usize i = 0; i < fg::HEADER_DIGEST_BYTES; ++i)
    {
        if (writer.WriteUint8(0) == false)
        {
            return Fail(outDiag,
                        Status::OutOfMemory,
                        geom::GeometryStatus::OutOfMemory,
                        "Header",
                        RepairForStatus(geom::GeometryStatus::OutOfMemory));
        }
    }
    const uint32 keyOffset = static_cast<uint32>(writer.GetSize());
    for (usize i = 0; i < source.Keys.GetSize(); ++i)
    {
        if (IsCancelled(cancelOrNull))
        {
            return Fail(outDiag,
                        Status::Cancelled,
                        geom::GeometryStatus::Cancelled,
                        "TrackKeys",
                        "Cancelled by the caller.");
        }
        const AuthoredKey& key = source.Keys[i];
        const bool isOk = writer.WriteFloat64(key.Time) && writer.WriteFloat32(key.Value) &&
                          writer.WriteFloat32(key.InSlope) && writer.WriteFloat32(key.OutSlope) &&
                          writer.WriteUint32(static_cast<uint32>(key.Interpolation));
        if (isOk == false)
        {
            return Fail(outDiag,
                        Status::OutOfMemory,
                        geom::GeometryStatus::OutOfMemory,
                        "TrackKeys",
                        RepairForStatus(geom::GeometryStatus::OutOfMemory));
        }
    }
    PatchSectionOffset(writer, 0, keyOffset);

    const usize headerBytes = fg::GetHeaderBytes(plan.SectionCount);
    const ludus::content::Digest payloadDigest = ludus::content::Hash(writer.GetMutableBytes().subspan(headerBytes));
    ludus::content::Bytes canonicalSource;
    GeometryDiagnosticReport sourceDiag{};
    const Status sourceStatus = WriteGeometrySource(source, canonicalSource, sourceDiag);
    if (sourceStatus != Status::Ok)
    {
        outDiag = sourceDiag;
        return sourceStatus;
    }
    const ludus::content::Digest sourceDigest = ludus::content::Hash(canonicalSource.Data());
    const ludus::content::Digest dependencyDigest = ludus::content::Hash(std::span<const uint8>{});
    PatchDigest(writer, digestsOffset, sourceDigest);
    PatchDigest(writer, digestsOffset + fg::DIGEST_BYTES, dependencyDigest);
    PatchDigest(writer, digestsOffset + 2 * fg::DIGEST_BYTES, payloadDigest);

    if (writer.Release(outBlob) == false)
    {
        return Fail(outDiag,
                    Status::OutOfMemory,
                    geom::GeometryStatus::OutOfMemory,
                    "Blob",
                    RepairForStatus(geom::GeometryStatus::OutOfMemory));
    }
    Succeed(outDiag);
    return Status::Ok;
}

[[nodiscard]] Status CookSurface(const GeometrySource& source,
                                 const geom::PreparedSurface& prepared,
                                 core::Array<uint8>& outBlob,
                                 const Cancellation* cancelOrNull,
                                 GeometryDiagnosticReport& outDiag) noexcept
{
    (void)prepared;
    HeaderPlan plan{};
    plan.Kind = fg::GeometryKindTag::Surface;
    plan.Precision = source.Precision == geom::PrecisionProfile::Strict ? fg::PrecisionProfileTag::Strict
                                                                        : fg::PrecisionProfileTag::Visual;
    plan.Flags = source.IsClosed ? static_cast<uint32>(fg::GeometryFlag::IsClosed) : 0U;
    plan.PatchCount = static_cast<uint32>(source.Patches.GetSize());
    plan.AdjacencyCount = static_cast<uint32>(source.Adjacency.GetSize());
    plan.Revision = source.Revision;
    plan.Origin = source.Origin;
    plan.AssetScale = source.AssetScale;
    plan.Tolerance = source.Tolerance;
    plan.Sections[0] = PendingSection{fg::SectionId::SurfacePatches, 0, plan.PatchCount, fg::PATCH_RECORD_BYTES};
    plan.Sections[1] =
        PendingSection{fg::SectionId::SurfaceAdjacency, 0, plan.AdjacencyCount, fg::ADJACENCY_RECORD_BYTES};
    plan.SectionCount = 2;

    ByteWriter writer;
    if (WriteHeaderPrefix(writer, plan) == false)
    {
        return Fail(outDiag,
                    Status::OutOfMemory,
                    geom::GeometryStatus::OutOfMemory,
                    "Header",
                    RepairForStatus(geom::GeometryStatus::OutOfMemory));
    }
    const usize digestsOffset = writer.GetSize();
    for (usize i = 0; i < fg::HEADER_DIGEST_BYTES; ++i)
    {
        if (writer.WriteUint8(0) == false)
        {
            return Fail(outDiag,
                        Status::OutOfMemory,
                        geom::GeometryStatus::OutOfMemory,
                        "Header",
                        RepairForStatus(geom::GeometryStatus::OutOfMemory));
        }
    }
    const uint32 patchOffset = static_cast<uint32>(writer.GetSize());
    for (usize i = 0; i < source.Patches.GetSize(); ++i)
    {
        if (IsCancelled(cancelOrNull))
        {
            return Fail(outDiag,
                        Status::Cancelled,
                        geom::GeometryStatus::Cancelled,
                        "SurfacePatches",
                        "Cancelled by the caller.");
        }
        const AuthoredPatch& patch = source.Patches[i];
        bool isOk = writer.WriteUint64(patch.AuthorId);
        for (usize u = 0; u < fg::PATCH_CONTROL_ORDER && isOk; ++u)
        {
            for (usize v = 0; v < fg::PATCH_CONTROL_ORDER && isOk; ++v)
            {
                // Store chunk-relative controls (subtract the origin as the owner does).
                const math::Vector3d relative = patch.Control[u][v] - source.Origin;
                isOk = writer.WriteFloat32(static_cast<float32>(relative.X)) &&
                       writer.WriteFloat32(static_cast<float32>(relative.Y)) &&
                       writer.WriteFloat32(static_cast<float32>(relative.Z));
            }
        }
        if (isOk == false)
        {
            return Fail(outDiag,
                        Status::OutOfMemory,
                        geom::GeometryStatus::OutOfMemory,
                        "SurfacePatches",
                        RepairForStatus(geom::GeometryStatus::OutOfMemory));
        }
    }
    const uint32 adjacencyOffset = static_cast<uint32>(writer.GetSize());
    for (usize i = 0; i < source.Adjacency.GetSize(); ++i)
    {
        const geom::SurfaceAdjacencySource& join = source.Adjacency[i];
        const bool isOk = writer.WriteUint32(static_cast<uint32>(join.FirstPatch)) &&
                          writer.WriteUint32(static_cast<uint32>(join.FirstSide)) &&
                          writer.WriteUint32(static_cast<uint32>(join.SecondPatch)) &&
                          writer.WriteUint32(static_cast<uint32>(join.SecondSide)) &&
                          writer.WriteUint32(join.IsReversed ? 1U : 0U);
        if (isOk == false)
        {
            return Fail(outDiag,
                        Status::OutOfMemory,
                        geom::GeometryStatus::OutOfMemory,
                        "SurfaceAdjacency",
                        RepairForStatus(geom::GeometryStatus::OutOfMemory));
        }
    }
    PatchSectionOffset(writer, 0, patchOffset);
    PatchSectionOffset(writer, 1, adjacencyOffset);

    const usize headerBytes = fg::GetHeaderBytes(plan.SectionCount);
    const ludus::content::Digest payloadDigest = ludus::content::Hash(writer.GetMutableBytes().subspan(headerBytes));
    ludus::content::Bytes canonicalSource;
    GeometryDiagnosticReport sourceDiag{};
    const Status sourceStatus = WriteGeometrySource(source, canonicalSource, sourceDiag);
    if (sourceStatus != Status::Ok)
    {
        outDiag = sourceDiag;
        return sourceStatus;
    }
    const ludus::content::Digest sourceDigest = ludus::content::Hash(canonicalSource.Data());
    const ludus::content::Digest dependencyDigest = ludus::content::Hash(std::span<const uint8>{});
    PatchDigest(writer, digestsOffset, sourceDigest);
    PatchDigest(writer, digestsOffset + fg::DIGEST_BYTES, dependencyDigest);
    PatchDigest(writer, digestsOffset + 2 * fg::DIGEST_BYTES, payloadDigest);

    if (writer.Release(outBlob) == false)
    {
        return Fail(outDiag,
                    Status::OutOfMemory,
                    geom::GeometryStatus::OutOfMemory,
                    "Blob",
                    RepairForStatus(geom::GeometryStatus::OutOfMemory));
    }
    Succeed(outDiag);
    return Status::Ok;
}
} // namespace

// Declared in geometry_source.cpp; converts an owned source to the borrowed
// FoundationGeometry inputs and runs the matching TryPrepare*.
Status PrepareFromSource(const GeometrySource& source,
                         geom::PreparedPath& outPath,
                         geom::PreparedTrack& outTrack,
                         geom::PreparedSurface& outSurface,
                         fg::GeometryKindTag& outKind,
                         GeometryDiagnosticReport& outDiag) noexcept;

Status CookGeometry(const GeometrySource& source,
                    core::Array<uint8>& outBlob,
                    GeometryDiagnosticReport& outDiagnostic,
                    const Cancellation* cancelOrNull) noexcept
{
    Succeed(outDiagnostic);
    if (IsCancelled(cancelOrNull))
    {
        return Fail(outDiagnostic,
                    Status::Cancelled,
                    geom::GeometryStatus::Cancelled,
                    "Source",
                    "Cancelled by the caller.");
    }
    // Dry-run the preparation so a cook never emits a blob that cannot load.
    geom::PreparedPath preparedPath;
    geom::PreparedTrack preparedTrack;
    geom::PreparedSurface preparedSurface;
    fg::GeometryKindTag kind = fg::GeometryKindTag::Path;
    const Status prepareStatus =
        PrepareFromSource(source, preparedPath, preparedTrack, preparedSurface, kind, outDiagnostic);
    if (prepareStatus != Status::Ok)
    {
        return prepareStatus;
    }

    core::Array<uint8> blob;
    Status status = Status::Ok;
    switch (kind)
    {
        case fg::GeometryKindTag::Path:
            status = CookPath(source, preparedPath, blob, cancelOrNull, outDiagnostic);
            break;
        case fg::GeometryKindTag::Track:
            status = CookTrack(source, preparedTrack, blob, cancelOrNull, outDiagnostic);
            break;
        case fg::GeometryKindTag::Surface:
            status = CookSurface(source, preparedSurface, blob, cancelOrNull, outDiagnostic);
            break;
    }
    if (status != Status::Ok)
    {
        return status;
    }
    outBlob = static_cast<core::Array<uint8>&&>(blob);
    Succeed(outDiagnostic);
    return Status::Ok;
}
} // namespace ludus::geometry::content
