// LoadGeometry: validate a LUGEO blob and rebuild the FoundationGeometry prepared
// owner. The reader validates the magic, versions, enum tags, counts, section
// byte products, offsets, alignment, overlap, monotone domains, finiteness and
// the payload digest BEFORE building any source input or exposing a view, like
// modules/runtime/resources SceneDefinition::Decode. Bad content is a structured
// diagnostic, never an assertion. See docs/architecture/curves-surfaces.md
// ("Content and failure contracts") and ADR 0024.

#include <ludus/geometry/content/geometry_content.h>

#include <ludus/content/content.h>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include "internal/byte_codec.h"
#include "internal/diagnostics.h"

namespace ludus::geometry::content
{
using ludus::content::Status;
using namespace ludus::geometry::content::internal;

namespace
{
/// Decoded blob header plus its validated section directory.
struct BlobHeader final
{
    uint32 FormatVersion = 0;
    uint32 CookerVersion = 0;
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
    uint32 SectionCount = 0;
    core::StaticArray<fg::SectionEntry, fg::MAX_SECTIONS> Sections{};
    ludus::content::Digest SourceDigest{};
    ludus::content::Digest DependencyDigest{};
    ludus::content::Digest PayloadDigest{};
    usize HeaderBytes = 0;
};

[[nodiscard]] bool FindSection(const BlobHeader& header, fg::SectionId id, const fg::SectionEntry*& outEntry) noexcept
{
    for (uint32 i = 0; i < header.SectionCount; ++i)
    {
        if (header.Sections[i].Id == static_cast<uint32>(id))
        {
            outEntry = &header.Sections[i];
            return true;
        }
    }
    return false;
}

/// Read and validate the header and its section directory.
[[nodiscard]] Status ReadHeader(std::span<const uint8> blob, BlobHeader& outHeader, GeometryDiagnosticReport& outDiag) noexcept
{
    if (blob.size() < fg::HEADER_MIN_BYTES)
    {
        return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "Header",
                    "Blob is shorter than the minimum header; re-cook the source.");
    }
    if (blob.size() > fg::MAX_BLOB_BYTES)
    {
        return Fail(outDiag, Status::Limit, geom::GeometryStatus::BudgetExceeded, "Header",
                    "Blob exceeds the geometry admission ceiling; split the asset.");
    }
    ByteReader reader(blob);
    uint8 magic[8]{};
    if (reader.ReadBytes(magic) == false)
    {
        return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "Magic", "Re-cook the source.");
    }
    for (usize i = 0; i < fg::MAGIC.size(); ++i)
    {
        if (magic[i] != static_cast<uint8>(fg::MAGIC[i]))
        {
            return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "Magic",
                        "Not a LUGEO geometry blob; re-cook the source.");
        }
    }
    uint32 kindTag = 0;
    uint32 precisionTag = 0;
    bool ok = reader.ReadUint32(outHeader.FormatVersion) && reader.ReadUint32(outHeader.CookerVersion) &&
              reader.ReadUint32(kindTag) && reader.ReadUint32(precisionTag) && reader.ReadUint32(outHeader.Flags) &&
              reader.ReadUint32(outHeader.SpanCount) && reader.ReadUint32(outHeader.JointCount) &&
              reader.ReadUint32(outHeader.KeyCount) && reader.ReadUint32(outHeader.PatchCount) &&
              reader.ReadUint32(outHeader.AdjacencyCount) && reader.ReadUint32(outHeader.SourceIdentityCount) &&
              reader.ReadUint64(outHeader.Revision);
    if (ok == false)
    {
        return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "Header", "Re-cook the source.");
    }
    if (fg::IsSupportedVersion(outHeader.FormatVersion) == false)
    {
        return Fail(outDiag, Status::Unsupported, geom::GeometryStatus::UnsupportedRepresentation, "FormatVersion",
                    "Re-cook the source with the current cooker.");
    }
    if (fg::IsKnownKind(kindTag) == false)
    {
        return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "KindTag", "Re-cook the source.");
    }
    if (fg::IsKnownPrecision(precisionTag) == false)
    {
        return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "PrecisionTag",
                    "Re-cook the source.");
    }
    outHeader.Kind = static_cast<fg::GeometryKindTag>(kindTag);
    outHeader.Precision = static_cast<fg::PrecisionProfileTag>(precisionTag);
    if ((outHeader.Flags & ~static_cast<uint32>(fg::GeometryFlag::IsClosed)) != 0)
    {
        return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "Flags", "Re-cook the source.");
    }
    float64 origin[fg::CONTROL_COMPONENTS]{};
    ok = reader.ReadFloat64(origin[0]) && reader.ReadFloat64(origin[1]) && reader.ReadFloat64(origin[2]) &&
         reader.ReadFloat64(outHeader.AssetScale) && reader.ReadFloat64(outHeader.PositionFloor) &&
         reader.ReadFloat64(outHeader.EffectivePositionError);
    if (ok == false)
    {
        return Fail(outDiag, Status::Invalid, geom::GeometryStatus::NonFiniteInput, "Origin",
                    "A header value is non-finite; re-cook the source.");
    }
    outHeader.Origin = math::Vector3d{origin[0], origin[1], origin[2]};
    float64 tolerance[fg::TOLERANCE_FIELD_COUNT]{};
    for (usize i = 0; i < fg::TOLERANCE_FIELD_COUNT; ++i)
    {
        if (reader.ReadFloat64(tolerance[i]) == false)
        {
            return Fail(outDiag, Status::Invalid, geom::GeometryStatus::NonFiniteInput, "Tolerance",
                        "A tolerance value is non-finite; re-cook the source.");
        }
    }
    outHeader.Tolerance = geom::ToleranceProfile{
        .PositionError = tolerance[0],
        .RelativePositionError = tolerance[1],
        .DistanceError = tolerance[2],
        .TangentAngleError = tolerance[3],
        .ProjectionGap = tolerance[4],
        .ContinuityPosition = tolerance[5],
        .ContinuityDerivative = tolerance[6],
        .ContinuityAngle = tolerance[7],
        .NormalMinDerivativeLength = tolerance[8],
        .NormalMinSinAngle = tolerance[9],
        .NormalAngleError = tolerance[10],
        .CoincidenceDistance = tolerance[11],
    };
    if (reader.ReadUint32(outHeader.SectionCount) == false || outHeader.SectionCount > fg::MAX_SECTIONS)
    {
        return Fail(outDiag, Status::Limit, geom::GeometryStatus::BudgetExceeded, "SectionCount",
                    "Too many sections; re-cook the source.");
    }
    outHeader.HeaderBytes = fg::GetHeaderBytes(outHeader.SectionCount);
    if (blob.size() < outHeader.HeaderBytes)
    {
        return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "Header",
                    "Blob is shorter than its declared header; re-cook the source.");
    }
    for (uint32 i = 0; i < outHeader.SectionCount; ++i)
    {
        fg::SectionEntry entry{};
        ok = reader.ReadUint32(entry.Id) && reader.ReadUint32(entry.Offset) && reader.ReadUint32(entry.Count) &&
             reader.ReadUint32(entry.ElementBytes);
        if (ok == false)
        {
            return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "Section",
                        "Re-cook the source.");
        }
        outHeader.Sections[i] = entry;
    }
    ok = reader.ReadBytes(outHeader.SourceDigest.Data) && reader.ReadBytes(outHeader.DependencyDigest.Data) &&
         reader.ReadBytes(outHeader.PayloadDigest.Data);
    if (ok == false)
    {
        return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "Digest", "Re-cook the source.");
    }
    return Status::Ok;
}

/// Validate a count against its admission ceiling.
[[nodiscard]] bool CheckCount(uint32 value, uint32 ceiling) noexcept
{
    return value <= ceiling;
}

/// Validate the section directory: byte products, alignment, in-range offsets,
/// non-overlap, and that exactly the kind's required sections are present.
[[nodiscard]] Status ValidateSections(std::span<const uint8> blob, const BlobHeader& header, GeometryDiagnosticReport& outDiag) noexcept
{
    // Every section must be a known id with the matching element width and count,
    // sit after the header, be four-byte aligned, lie in range and not overlap.
    core::StaticArray<usize, fg::MAX_SECTIONS> starts{};
    core::StaticArray<usize, fg::MAX_SECTIONS> ends{};
    for (uint32 i = 0; i < header.SectionCount; ++i)
    {
        const fg::SectionEntry& entry = header.Sections[i];
        const usize expectedWidth = fg::GetSectionElementBytes(static_cast<fg::SectionId>(entry.Id));
        if (expectedWidth == 0 || entry.ElementBytes != expectedWidth)
        {
            return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "Section.ElementBytes",
                        "Unknown or mismatched section; re-cook the source.", i);
        }
        usize payloadBytes = 0;
        if (core::TryMultiply<usize>(static_cast<usize>(entry.Count), entry.ElementBytes, payloadBytes) == false)
        {
            return Fail(outDiag, Status::Invalid, geom::GeometryStatus::OutOfRange, "Section.Count",
                        "Section count times width overflows; the blob is corrupt.", i);
        }
        usize sectionEnd = 0;
        if (core::TryAdd<usize>(static_cast<usize>(entry.Offset), payloadBytes, sectionEnd) == false)
        {
            return Fail(outDiag, Status::Invalid, geom::GeometryStatus::OutOfRange, "Section.Offset",
                        "Section extent overflows; the blob is corrupt.", i);
        }
        if (entry.Offset < header.HeaderBytes || sectionEnd > blob.size() || (entry.Offset % 4) != 0)
        {
            return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "Section.Offset",
                        "Section is misaligned or out of range; the blob is corrupt.", i);
        }
        starts[i] = entry.Offset;
        ends[i] = sectionEnd;
    }
    for (uint32 i = 0; i < header.SectionCount; ++i)
    {
        for (uint32 j = i + 1; j < header.SectionCount; ++j)
        {
            const bool disjoint = ends[i] <= starts[j] || ends[j] <= starts[i];
            if (disjoint == false)
            {
                return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "Section.Offset",
                            "Two sections overlap; the blob is corrupt.", i);
            }
        }
    }
    return Status::Ok;
}

/// Confirm a required section exists with the expected record count.
[[nodiscard]] bool RequireSection(const BlobHeader& header, fg::SectionId id, uint32 expectedCount, const fg::SectionEntry*& outEntry) noexcept
{
    return FindSection(header, id, outEntry) && outEntry->Count == expectedCount;
}

[[nodiscard]] Status BuildPath(std::span<const uint8> blob,
                               const BlobHeader& header,
                               const geom::WorkLimits& limits,
                               GeometryLoad& outLoad,
                               GeometryDiagnosticReport& outDiag) noexcept
{
    if (CheckCount(header.SpanCount, fg::MAX_SPANS) == false || CheckCount(header.JointCount, fg::MAX_JOINTS) == false)
    {
        return Fail(outDiag, Status::Limit, geom::GeometryStatus::BudgetExceeded, "SpanCount",
                    "Too many spans or joints; split the path.");
    }
    const fg::SectionEntry* spanSectionOrNull = nullptr;
    if (RequireSection(header, fg::SectionId::PathSpans, header.SpanCount, spanSectionOrNull) == false)
    {
        return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "PathSpans",
                    "Missing or inconsistent span section; re-cook the source.");
    }
    core::Array<geom::PathSpanSource> spans;
    if (spans.TryResize(header.SpanCount) == false)
    {
        return Fail(outDiag, Status::OutOfMemory, geom::GeometryStatus::OutOfMemory, "PathSpans",
                    RepairForStatus(geom::GeometryStatus::OutOfMemory));
    }
    ByteReader reader(blob);
    (void)reader.SeekTo(spanSectionOrNull->Offset);
    for (uint32 i = 0; i < header.SpanCount; ++i)
    {
        uint32 kindTag = 0;
        uint32 sourceElement = 0;
        float64 domainWidth = 0.0;
        if (reader.ReadUint32(kindTag) == false || reader.ReadUint32(sourceElement) == false ||
            reader.ReadFloat64(domainWidth) == false || fg::IsKnownSpanKind(kindTag) == false)
        {
            return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "PathSpans.Kind",
                        "Corrupt span record; re-cook the source.", i);
        }
        geom::PathSpanSource& span = spans[i];
        span.Kind = kindTag == static_cast<uint32>(fg::SpanKindTag::Line) ? geom::SpanKind::Line : geom::SpanKind::Cubic;
        span.DomainWidth = domainWidth;
        span.SourceElement = sourceElement;
        for (usize c = 0; c < fg::SPAN_CONTROL_COUNT; ++c)
        {
            float32 component[fg::CONTROL_COMPONENTS]{};
            if (reader.ReadFloat32(component[0]) == false || reader.ReadFloat32(component[1]) == false ||
                reader.ReadFloat32(component[2]) == false)
            {
                return Fail(outDiag, Status::Invalid, geom::GeometryStatus::NonFiniteInput, "PathSpans.Control",
                            "Non-finite control; re-cook the source.", i);
            }
            span.Control[c] = math::Vector3d{static_cast<float64>(component[0]), static_cast<float64>(component[1]),
                                             static_cast<float64>(component[2])} +
                              header.Origin;
        }
    }

    geom::PathSource source{};
    source.Origin = header.Origin;
    source.Spans = BorrowedView<const geom::PathSpanSource>(spans.GetData(), spans.GetSize());
    source.IsClosed = (header.Flags & static_cast<uint32>(fg::GeometryFlag::IsClosed)) != 0;
    source.AssetScale = header.AssetScale;
    source.Revision = header.Revision;
    geom::PathPreparation preparation{};
    preparation.Tolerance = header.Tolerance;
    preparation.Limits = limits;

    geom::PreparedPath prepared;
    geom::GeometryDiagnostic prepareDiag{};
    const geom::GeometryStatus status = geom::TryPreparePath(source, preparation, prepared, &prepareDiag);
    if (status != geom::GeometryStatus::Success)
    {
        outDiag.Transport = MapPrepareTransport(status);
        outDiag.Detail = prepareDiag;
        outDiag.Repair = RepairForStatus(status);
        (void)outDiag.Message.Set("");
        return outDiag.Transport;
    }
    outLoad.Kind = fg::GeometryKindTag::Path;
    outLoad.Path = static_cast<geom::PreparedPath&&>(prepared);
    return Status::Ok;
}

[[nodiscard]] Status BuildTrack(std::span<const uint8> blob,
                                const BlobHeader& header,
                                const geom::WorkLimits& limits,
                                GeometryLoad& outLoad,
                                GeometryDiagnosticReport& outDiag) noexcept
{
    if (CheckCount(header.KeyCount, fg::MAX_KEYS) == false)
    {
        return Fail(outDiag, Status::Limit, geom::GeometryStatus::BudgetExceeded, "KeyCount",
                    "Too many keys; split the track.");
    }
    const fg::SectionEntry* keySectionOrNull = nullptr;
    if (RequireSection(header, fg::SectionId::TrackKeys, header.KeyCount, keySectionOrNull) == false)
    {
        return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "TrackKeys",
                    "Missing or inconsistent key section; re-cook the source.");
    }
    core::Array<geom::TrackKey> keys;
    if (keys.TryResize(header.KeyCount) == false)
    {
        return Fail(outDiag, Status::OutOfMemory, geom::GeometryStatus::OutOfMemory, "TrackKeys",
                    RepairForStatus(geom::GeometryStatus::OutOfMemory));
    }
    ByteReader reader(blob);
    (void)reader.SeekTo(keySectionOrNull->Offset);
    float64 previousTime = 0.0;
    for (uint32 i = 0; i < header.KeyCount; ++i)
    {
        float64 time = 0.0;
        float32 value = 0.0F;
        float32 inSlope = 0.0F;
        float32 outSlope = 0.0F;
        uint32 interpolationTag = 0;
        if (reader.ReadFloat64(time) == false || reader.ReadFloat32(value) == false ||
            reader.ReadFloat32(inSlope) == false || reader.ReadFloat32(outSlope) == false ||
            reader.ReadUint32(interpolationTag) == false || fg::IsKnownTrackInterpolation(interpolationTag) == false)
        {
            return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "TrackKeys",
                        "Corrupt key record; re-cook the source.", i);
        }
        if (i > 0 && (time > previousTime) == false)
        {
            return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidDomain, "TrackKeys.Time",
                        "Key times must be strictly increasing; re-cook the source.", i);
        }
        previousTime = time;
        geom::TrackKey& key = keys[i];
        key.Time = time;
        key.Value = value;
        key.InSlope = inSlope;
        key.OutSlope = outSlope;
        key.Interpolation = interpolationTag == static_cast<uint32>(fg::TrackInterpolationTag::Step)
                                ? geom::TrackInterpolation::Step
                            : interpolationTag == static_cast<uint32>(fg::TrackInterpolationTag::Linear)
                                ? geom::TrackInterpolation::Linear
                                : geom::TrackInterpolation::Hermite;
    }

    geom::PreparedTrack prepared;
    geom::GeometryDiagnostic prepareDiag{};
    const geom::GeometryStatus status = geom::TryPrepareTrack(
        BorrowedView<const geom::TrackKey>(keys.GetData(), keys.GetSize()), limits, prepared, &prepareDiag);
    if (status != geom::GeometryStatus::Success)
    {
        outDiag.Transport = MapPrepareTransport(status);
        outDiag.Detail = prepareDiag;
        outDiag.Repair = RepairForStatus(status);
        (void)outDiag.Message.Set("");
        return outDiag.Transport;
    }
    outLoad.Kind = fg::GeometryKindTag::Track;
    outLoad.Track = static_cast<geom::PreparedTrack&&>(prepared);
    return Status::Ok;
}

[[nodiscard]] Status BuildSurface(std::span<const uint8> blob,
                                  const BlobHeader& header,
                                  const geom::WorkLimits& limits,
                                  GeometryLoad& outLoad,
                                  GeometryDiagnosticReport& outDiag) noexcept
{
    if (CheckCount(header.PatchCount, fg::MAX_PATCHES) == false ||
        CheckCount(header.AdjacencyCount, fg::MAX_ADJACENCY) == false)
    {
        return Fail(outDiag, Status::Limit, geom::GeometryStatus::BudgetExceeded, "PatchCount",
                    "Too many patches or joins; split the surface.");
    }
    const fg::SectionEntry* patchSectionOrNull = nullptr;
    const fg::SectionEntry* adjacencySectionOrNull = nullptr;
    if (RequireSection(header, fg::SectionId::SurfacePatches, header.PatchCount, patchSectionOrNull) == false ||
        RequireSection(header, fg::SectionId::SurfaceAdjacency, header.AdjacencyCount, adjacencySectionOrNull) == false)
    {
        return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "SurfacePatches",
                    "Missing or inconsistent surface sections; re-cook the source.");
    }
    core::Array<geom::SurfacePatchSource> patches;
    core::Array<geom::SurfaceAdjacencySource> adjacency;
    if (patches.TryResize(header.PatchCount) == false ||
        (header.AdjacencyCount > 0 && adjacency.TryResize(header.AdjacencyCount) == false))
    {
        return Fail(outDiag, Status::OutOfMemory, geom::GeometryStatus::OutOfMemory, "SurfacePatches",
                    RepairForStatus(geom::GeometryStatus::OutOfMemory));
    }
    ByteReader reader(blob);
    (void)reader.SeekTo(patchSectionOrNull->Offset);
    for (uint32 i = 0; i < header.PatchCount; ++i)
    {
        geom::SurfacePatchSource& patch = patches[i];
        if (reader.ReadUint64(patch.Id) == false)
        {
            return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "SurfacePatches.Id",
                        "Corrupt patch record; re-cook the source.", i);
        }
        for (usize u = 0; u < fg::PATCH_CONTROL_ORDER; ++u)
        {
            for (usize v = 0; v < fg::PATCH_CONTROL_ORDER; ++v)
            {
                float32 component[fg::CONTROL_COMPONENTS]{};
                if (reader.ReadFloat32(component[0]) == false || reader.ReadFloat32(component[1]) == false ||
                    reader.ReadFloat32(component[2]) == false)
                {
                    return Fail(outDiag, Status::Invalid, geom::GeometryStatus::NonFiniteInput,
                                "SurfacePatches.Control", "Non-finite control; re-cook the source.", i);
                }
                patch.Control[u][v] = math::Vector3d{static_cast<float64>(component[0]),
                                                     static_cast<float64>(component[1]),
                                                     static_cast<float64>(component[2])} +
                                      header.Origin;
            }
        }
    }
    (void)reader.SeekTo(adjacencySectionOrNull->Offset);
    for (uint32 i = 0; i < header.AdjacencyCount; ++i)
    {
        uint32 firstPatch = 0;
        uint32 firstSide = 0;
        uint32 secondPatch = 0;
        uint32 secondSide = 0;
        uint32 isReversed = 0;
        if (reader.ReadUint32(firstPatch) == false || reader.ReadUint32(firstSide) == false ||
            reader.ReadUint32(secondPatch) == false || reader.ReadUint32(secondSide) == false ||
            reader.ReadUint32(isReversed) == false || fg::IsKnownPatchSide(firstSide) == false ||
            fg::IsKnownPatchSide(secondSide) == false || firstPatch >= header.PatchCount ||
            secondPatch >= header.PatchCount || isReversed > 1)
        {
            return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidTopology, "SurfaceAdjacency",
                        "Corrupt or out-of-range adjacency; re-cook the source.", i);
        }
        geom::SurfaceAdjacencySource& join = adjacency[i];
        join.FirstPatch = firstPatch;
        join.FirstSide = static_cast<geom::PatchSide>(firstSide);
        join.SecondPatch = secondPatch;
        join.SecondSide = static_cast<geom::PatchSide>(secondSide);
        join.IsReversed = isReversed != 0;
    }

    geom::SurfaceSource source{};
    source.Origin = header.Origin;
    source.Patches = BorrowedView<const geom::SurfacePatchSource>(patches.GetData(), patches.GetSize());
    source.Adjacency = BorrowedView<const geom::SurfaceAdjacencySource>(adjacency.GetData(), adjacency.GetSize());
    source.AssetScale = header.AssetScale;
    source.Revision = header.Revision;
    geom::SurfacePreparation preparation{};
    preparation.Tolerance = header.Tolerance;
    preparation.Limits = limits;

    geom::PreparedSurface prepared;
    geom::GeometryDiagnostic prepareDiag{};
    const geom::GeometryStatus status = geom::TryPrepareSurface(source, preparation, prepared, &prepareDiag);
    if (status != geom::GeometryStatus::Success)
    {
        outDiag.Transport = MapPrepareTransport(status);
        outDiag.Detail = prepareDiag;
        outDiag.Repair = RepairForStatus(status);
        (void)outDiag.Message.Set("");
        return outDiag.Transport;
    }
    outLoad.Kind = fg::GeometryKindTag::Surface;
    outLoad.Surface = static_cast<geom::PreparedSurface&&>(prepared);
    return Status::Ok;
}
} // namespace

Status LoadGeometry(std::span<const uint8> blob,
                    const geom::WorkLimits& limits,
                    GeometryLoad& outLoad,
                    GeometryDiagnosticReport& outDiagnostic) noexcept
{
    Succeed(outDiagnostic);
    BlobHeader header{};
    Status status = ReadHeader(blob, header, outDiagnostic);
    if (status != Status::Ok)
    {
        return status;
    }
    status = ValidateSections(blob, header, outDiagnostic);
    if (status != Status::Ok)
    {
        return status;
    }
    if (geom::TryValidateToleranceProfile(header.Tolerance) != geom::GeometryStatus::Success)
    {
        return Fail(outDiagnostic, Status::Invalid, geom::GeometryStatus::InvalidArgument, "Tolerance",
                    "A tolerance field is out of range; re-cook the source.");
    }

    // Cache consistency: the payload is everything after the header; its digest
    // must match the header's PayloadDigest.
    const std::span<const uint8> payload = blob.subspan(header.HeaderBytes);
    const ludus::content::Digest payloadDigest = ludus::content::Hash(payload);
    if ((payloadDigest == header.PayloadDigest) == false)
    {
        return Fail(outDiagnostic, Status::Invalid, geom::GeometryStatus::StaleRevision, "PayloadDigest",
                    "Payload digest mismatch; the cache is inconsistent, re-cook the source.");
    }

    GeometryLoad load;
    switch (header.Kind)
    {
    case fg::GeometryKindTag::Path:
        status = BuildPath(blob, header, limits, load, outDiagnostic);
        break;
    case fg::GeometryKindTag::Track:
        status = BuildTrack(blob, header, limits, load, outDiagnostic);
        break;
    case fg::GeometryKindTag::Surface:
        status = BuildSurface(blob, header, limits, load, outDiagnostic);
        break;
    }
    if (status != Status::Ok)
    {
        return status;
    }
    load.Key = GeometryCacheKey{
        .SourceDigest = header.SourceDigest,
        .DependencyDigest = header.DependencyDigest,
        .TopologyDigest = ludus::content::Digest{},
        .FormatVersion = header.FormatVersion,
        .CookerVersion = header.CookerVersion,
        .SchemaVersion = SOURCE_SCHEMA_VERSION,
        .Precision = header.Precision == fg::PrecisionProfileTag::Strict ? geom::PrecisionProfile::Strict
                                                                         : geom::PrecisionProfile::Visual,
        .Tolerance = header.Tolerance,
    };
    outLoad = static_cast<GeometryLoad&&>(load);
    Succeed(outDiagnostic);
    return Status::Ok;
}
} // namespace ludus::geometry::content
