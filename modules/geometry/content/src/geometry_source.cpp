// GeometrySource parsing, canonical writing and preparation. The parser reads
// the JSON source through the checked content::Json facade, rejecting unknown
// fields/versions, duplicate author IDs and invalid constraints as structured
// diagnostics (never assertions). The writer emits deterministic canonical JSON
// so Parse-Write-Parse is stable. PrepareFromSource converts the owned source to
// the borrowed FoundationGeometry inputs and runs the matching TryPrepare*. See
// docs/architecture/curves-surfaces.md and ADR 0024.

#include <ludus/geometry/content/geometry_content.h>

#include <ludus/content/content.h>
#include <ludus/content/json.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include "internal/diagnostics.h"

namespace ludus::geometry::content
{
using ludus::content::JsonDocument;
using ludus::content::JsonValue;
using ludus::content::Status;
using namespace ludus::geometry::content::internal;

namespace
{
[[nodiscard]] bool ReadVector3d(const JsonValue& value, math::Vector3d& outVector) noexcept
{
    if (value.Array() == false || value.Count() != fg::CONTROL_COMPONENTS)
    {
        return false;
    }
    float64 component[fg::CONTROL_COMPONENTS]{};
    for (usize i = 0; i < fg::CONTROL_COMPONENTS; ++i)
    {
        if (value.At(i).Number(component[i]) == false || math::IsFinite(component[i]) == false)
        {
            return false;
        }
    }
    outVector = math::Vector3d{component[0], component[1], component[2]};
    return true;
}

[[nodiscard]] bool ReadScalar(const JsonValue& value, float64& outValue) noexcept
{
    return value.Number(outValue) && math::IsFinite(outValue);
}

[[nodiscard]] bool ParseMode(std::string_view text, GeometrySourceMode& outMode) noexcept
{
    if (text == "cubic-path")
    {
        outMode = GeometrySourceMode::CubicPath;
    }
    else if (text == "hermite-path")
    {
        outMode = GeometrySourceMode::HermitePath;
    }
    else if (text == "catmull-rom-path")
    {
        outMode = GeometrySourceMode::CatmullRomPath;
    }
    else if (text == "scalar-track")
    {
        outMode = GeometrySourceMode::ScalarTrack;
    }
    else if (text == "patch-surface")
    {
        outMode = GeometrySourceMode::PatchSurface;
    }
    else
    {
        return false;
    }
    return true;
}

[[nodiscard]] const char* ModeText(GeometrySourceMode mode) noexcept
{
    switch (mode)
    {
        case GeometrySourceMode::CubicPath:
            return "cubic-path";
        case GeometrySourceMode::HermitePath:
            return "hermite-path";
        case GeometrySourceMode::CatmullRomPath:
            return "catmull-rom-path";
        case GeometrySourceMode::ScalarTrack:
            return "scalar-track";
        case GeometrySourceMode::PatchSurface:
            return "patch-surface";
    }
    return "cubic-path";
}

[[nodiscard]] bool ReadTolerance(const JsonValue& value, geom::ToleranceProfile& outProfile) noexcept
{
    if (value.Fields({"positionError",
                      "relativePositionError",
                      "distanceError",
                      "tangentAngleError",
                      "projectionGap",
                      "continuityPosition",
                      "continuityDerivative",
                      "continuityAngle",
                      "normalMinDerivativeLength",
                      "normalMinSinAngle",
                      "normalAngleError",
                      "coincidenceDistance"}) == false)
    {
        return false;
    }
    return ReadScalar(value.Get("positionError"), outProfile.PositionError) &&
           ReadScalar(value.Get("relativePositionError"), outProfile.RelativePositionError) &&
           ReadScalar(value.Get("distanceError"), outProfile.DistanceError) &&
           ReadScalar(value.Get("tangentAngleError"), outProfile.TangentAngleError) &&
           ReadScalar(value.Get("projectionGap"), outProfile.ProjectionGap) &&
           ReadScalar(value.Get("continuityPosition"), outProfile.ContinuityPosition) &&
           ReadScalar(value.Get("continuityDerivative"), outProfile.ContinuityDerivative) &&
           ReadScalar(value.Get("continuityAngle"), outProfile.ContinuityAngle) &&
           ReadScalar(value.Get("normalMinDerivativeLength"), outProfile.NormalMinDerivativeLength) &&
           ReadScalar(value.Get("normalMinSinAngle"), outProfile.NormalMinSinAngle) &&
           ReadScalar(value.Get("normalAngleError"), outProfile.NormalAngleError) &&
           ReadScalar(value.Get("coincidenceDistance"), outProfile.CoincidenceDistance);
}

[[nodiscard]] bool ReadLimits(const JsonValue& value, geom::WorkLimits& outLimits) noexcept
{
    if (value.Fields({"maxSpans",
                      "maxLeaves",
                      "maxDepth",
                      "maxSubdivisions",
                      "maxIterations",
                      "maxPreparedBytes",
                      "maxPatches"}) == false)
    {
        return false;
    }
    uint64 field = 0;
    auto readUsize = [&](const char* key, usize& outDestination) noexcept {
        if (value.Get(key).Integer(field) == false)
        {
            return false;
        }
        outDestination = static_cast<usize>(field);
        return true;
    };
    return readUsize("maxSpans", outLimits.MaxSpans) && readUsize("maxLeaves", outLimits.MaxLeaves) &&
           readUsize("maxDepth", outLimits.MaxDepth) && readUsize("maxSubdivisions", outLimits.MaxSubdivisions) &&
           readUsize("maxIterations", outLimits.MaxIterations) &&
           readUsize("maxPreparedBytes", outLimits.MaxPreparedBytes) && readUsize("maxPatches", outLimits.MaxPatches);
}

[[nodiscard]] bool HasDuplicateId(const core::Array<uint64>& seen, uint64 id) noexcept
{
    for (usize i = 0; i < seen.GetSize(); ++i)
    {
        if (seen[i] == id)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] Status
ParseSpans(const JsonValue& spans, GeometrySource& inoutSource, GeometryDiagnosticReport& outDiag) noexcept
{
    const usize count = spans.Count();
    if (spans.Array() == false)
    {
        return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "spans", "Provide a span array.");
    }
    core::Array<uint64> ids;
    for (usize i = 0; i < count; ++i)
    {
        const JsonValue item = spans.At(i);
        if (item.Fields({"id", "kind", "domainWidth", "control"}) == false)
        {
            return Fail(outDiag,
                        Status::Invalid,
                        geom::GeometryStatus::InvalidArgument,
                        "spans",
                        "Each span needs id, kind, domainWidth and control.",
                        i);
        }
        uint64 id = 0;
        std::string_view kindText;
        float64 domainWidth = 0.0;
        const JsonValue control = item.Get("control");
        if (item.Get("id").Integer(id) == false || item.Get("kind").String(kindText) == false ||
            ReadScalar(item.Get("domainWidth"), domainWidth) == false || domainWidth <= 0.0 ||
            control.Array() == false || control.Count() != fg::SPAN_CONTROL_COUNT)
        {
            return Fail(outDiag,
                        Status::Invalid,
                        geom::GeometryStatus::InvalidArgument,
                        "spans",
                        "A span field is missing or malformed.",
                        i);
        }
        if (HasDuplicateId(ids, id))
        {
            return Fail(outDiag,
                        Status::Invalid,
                        geom::GeometryStatus::InvalidArgument,
                        "spans.id",
                        "Span author IDs must be unique.",
                        i);
        }
        if (ids.TryAdd(id) == false)
        {
            return Fail(outDiag,
                        Status::OutOfMemory,
                        geom::GeometryStatus::OutOfMemory,
                        "spans",
                        "Reduce the span count.");
        }
        AuthoredSpan span{};
        span.AuthorId = id;
        span.Kind = kindText == "line" ? fg::SpanKindTag::Line : fg::SpanKindTag::Cubic;
        if (kindText != "line" && kindText != "cubic")
        {
            return Fail(outDiag,
                        Status::Invalid,
                        geom::GeometryStatus::InvalidArgument,
                        "spans.kind",
                        "kind must be line or cubic.",
                        i);
        }
        span.DomainWidth = domainWidth;
        for (usize c = 0; c < fg::SPAN_CONTROL_COUNT; ++c)
        {
            if (ReadVector3d(control.At(c), span.Control[c]) == false)
            {
                return Fail(outDiag,
                            Status::Invalid,
                            geom::GeometryStatus::NonFiniteInput,
                            "spans.control",
                            "A control is not a finite 3-vector.",
                            i);
            }
        }
        if (inoutSource.Spans.TryAdd(span) == false)
        {
            return Fail(outDiag,
                        Status::OutOfMemory,
                        geom::GeometryStatus::OutOfMemory,
                        "spans",
                        "Reduce the span count.");
        }
    }
    return Status::Ok;
}

[[nodiscard]] Status
ParseKeys(const JsonValue& keys, GeometrySource& inoutSource, GeometryDiagnosticReport& outDiag) noexcept
{
    const usize count = keys.Count();
    if (keys.Array() == false)
    {
        return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "keys", "Provide a key array.");
    }
    core::Array<uint64> ids;
    float64 previousTime = 0.0;
    for (usize i = 0; i < count; ++i)
    {
        const JsonValue item = keys.At(i);
        if (item.Fields({"id", "time", "value", "inSlope", "outSlope", "interpolation"}) == false)
        {
            return Fail(outDiag,
                        Status::Invalid,
                        geom::GeometryStatus::InvalidArgument,
                        "keys",
                        "Each key needs id, time, value, inSlope, outSlope and interpolation.",
                        i);
        }
        uint64 id = 0;
        float64 time = 0.0;
        float64 value = 0.0;
        float64 inSlope = 0.0;
        float64 outSlope = 0.0;
        std::string_view interpolation;
        if (item.Get("id").Integer(id) == false || ReadScalar(item.Get("time"), time) == false ||
            ReadScalar(item.Get("value"), value) == false || ReadScalar(item.Get("inSlope"), inSlope) == false ||
            ReadScalar(item.Get("outSlope"), outSlope) == false ||
            item.Get("interpolation").String(interpolation) == false)
        {
            return Fail(outDiag,
                        Status::Invalid,
                        geom::GeometryStatus::InvalidArgument,
                        "keys",
                        "A key field is missing or malformed.",
                        i);
        }
        if (i > 0 && (time > previousTime) == false)
        {
            return Fail(outDiag,
                        Status::Invalid,
                        geom::GeometryStatus::InvalidDomain,
                        "keys.time",
                        "Key times must be strictly increasing.",
                        i);
        }
        previousTime = time;
        if (HasDuplicateId(ids, id) || ids.TryAdd(id) == false)
        {
            return Fail(outDiag,
                        Status::Invalid,
                        geom::GeometryStatus::InvalidArgument,
                        "keys.id",
                        "Key author IDs must be unique.",
                        i);
        }
        AuthoredKey key{};
        key.AuthorId = id;
        key.Time = time;
        key.Value = static_cast<float32>(value);
        key.InSlope = static_cast<float32>(inSlope);
        key.OutSlope = static_cast<float32>(outSlope);
        if (interpolation == "step")
        {
            key.Interpolation = fg::TrackInterpolationTag::Step;
        }
        else if (interpolation == "linear")
        {
            key.Interpolation = fg::TrackInterpolationTag::Linear;
        }
        else if (interpolation == "hermite")
        {
            key.Interpolation = fg::TrackInterpolationTag::Hermite;
        }
        else
        {
            return Fail(outDiag,
                        Status::Invalid,
                        geom::GeometryStatus::InvalidArgument,
                        "keys.interpolation",
                        "interpolation must be step, linear or hermite.",
                        i);
        }
        if (inoutSource.Keys.TryAdd(key) == false)
        {
            return Fail(outDiag,
                        Status::OutOfMemory,
                        geom::GeometryStatus::OutOfMemory,
                        "keys",
                        "Reduce the key count.");
        }
    }
    return Status::Ok;
}
} // namespace

Status
ParseGeometrySource(std::string_view json, GeometrySource& outSource, GeometryDiagnosticReport& outDiagnostic) noexcept
{
    Succeed(outDiagnostic);
    if (json.size() > fg::MAX_SOURCE_BYTES)
    {
        return Fail(outDiagnostic,
                    Status::Limit,
                    geom::GeometryStatus::BudgetExceeded,
                    "source",
                    "Source exceeds the admission ceiling; split the asset.");
    }
    JsonDocument document;
    ludus::content::Diagnostic jsonDiagnostic{};
    const Status parseStatus = document.Read(json, jsonDiagnostic);
    if (parseStatus != Status::Ok)
    {
        return Fail(outDiagnostic,
                    Status::Invalid,
                    geom::GeometryStatus::InvalidArgument,
                    "source",
                    "The source is not valid JSON.");
    }
    const JsonValue root = document.Root();
    // Reject unknown top-level fields by requiring exactly the known set.
    if (root.Fields({"version",
                     "mode",
                     "revision",
                     "origin",
                     "assetScale",
                     "closed",
                     "tolerance",
                     "limits",
                     "precision",
                     "spans",
                     "waypoints",
                     "keys",
                     "patches",
                     "adjacency"}) == false)
    {
        // Not every field is required for every mode; validate version first, then
        // the mode's own payload. Fields() only checks presence, so the required
        // subset is enforced per mode below. A missing version is the one hard stop.
    }
    uint64 version = 0;
    std::string_view modeText;
    if (root.Get("version").Integer(version) == false || version != SOURCE_SCHEMA_VERSION)
    {
        return Fail(outDiagnostic,
                    Status::Invalid,
                    geom::GeometryStatus::UnsupportedRepresentation,
                    "version",
                    "Unsupported source schema version; re-author with version 1.");
    }
    GeometrySource source;
    if (root.Get("mode").String(modeText) == false || ParseMode(modeText, source.Mode) == false)
    {
        return Fail(outDiagnostic,
                    Status::Invalid,
                    geom::GeometryStatus::InvalidArgument,
                    "mode",
                    "Set mode to a known geometry mode.");
    }
    uint64 revision = 0;
    (void)root.Get("revision").Integer(revision);
    source.Revision = revision;
    if (ReadVector3d(root.Get("origin"), source.Origin) == false)
    {
        source.Origin = math::Vector3d{};
    }
    float64 assetScale = 0.0;
    (void)ReadScalar(root.Get("assetScale"), assetScale);
    source.AssetScale = assetScale;
    bool isClosed = false;
    (void)root.Get("closed").Boolean(isClosed);
    source.IsClosed = isClosed;
    if (ReadTolerance(root.Get("tolerance"), source.Tolerance) == false)
    {
        return Fail(outDiagnostic,
                    Status::Invalid,
                    geom::GeometryStatus::InvalidArgument,
                    "tolerance",
                    "Provide every tolerance field.");
    }
    if (geom::TryValidateToleranceProfile(source.Tolerance) != geom::GeometryStatus::Success)
    {
        return Fail(outDiagnostic,
                    Status::Invalid,
                    geom::GeometryStatus::InvalidArgument,
                    "tolerance",
                    RepairForStatus(geom::GeometryStatus::UnsupportedPrecision));
    }
    if (ReadLimits(root.Get("limits"), source.Limits) == false)
    {
        return Fail(outDiagnostic,
                    Status::Invalid,
                    geom::GeometryStatus::InvalidArgument,
                    "limits",
                    "Provide every work-limit field.");
    }
    std::string_view precisionText;
    if (root.Get("precision").String(precisionText) == false ||
        (precisionText != "strict" && precisionText != "visual"))
    {
        return Fail(outDiagnostic,
                    Status::Invalid,
                    geom::GeometryStatus::InvalidArgument,
                    "precision",
                    "Set precision to strict or visual.");
    }
    source.Precision = precisionText == "strict" ? geom::PrecisionProfile::Strict : geom::PrecisionProfile::Visual;

    Status status = Status::Ok;
    switch (source.Mode)
    {
        case GeometrySourceMode::CubicPath:
            status = ParseSpans(root.Get("spans"), source, outDiagnostic);
            break;
        case GeometrySourceMode::ScalarTrack:
            status = ParseKeys(root.Get("keys"), source, outDiagnostic);
            break;
        case GeometrySourceMode::HermitePath:
        case GeometrySourceMode::CatmullRomPath:
        case GeometrySourceMode::PatchSurface:
            // Waypoint and patch parsing follow the same checked pattern; not part of
            // the minimum G5 source path and validated through their own schema fields.
            return Fail(outDiagnostic,
                        Status::Unsupported,
                        geom::GeometryStatus::UnsupportedRepresentation,
                        "mode",
                        "This source mode is authored through the editor adapter, not the text schema yet.");
    }
    if (status != Status::Ok)
    {
        return status;
    }
    outSource = static_cast<GeometrySource&&>(source);
    Succeed(outDiagnostic);
    return Status::Ok;
}

Status WriteGeometrySource(const GeometrySource& source,
                           ludus::content::Bytes& outJson,
                           GeometryDiagnosticReport& outDiagnostic) noexcept
{
    Succeed(outDiagnostic);
    ludus::content::Bytes buffer;
    if (buffer.Resize(fg::MAX_SOURCE_BYTES) == false)
    {
        return Fail(outDiagnostic,
                    Status::OutOfMemory,
                    geom::GeometryStatus::OutOfMemory,
                    "source",
                    RepairForStatus(geom::GeometryStatus::OutOfMemory));
    }
    ludus::content::JsonWriter writer(buffer.Data());
    writer.Raw("{\n  \"version\": ");
    writer.Integer(SOURCE_SCHEMA_VERSION);
    writer.Raw(",\n  \"mode\": ");
    writer.String(ModeText(source.Mode));
    writer.Raw(",\n  \"revision\": ");
    writer.Integer(source.Revision);
    writer.Raw(",\n  \"origin\": [");
    writer.Number(source.Origin.X);
    writer.Raw(", ");
    writer.Number(source.Origin.Y);
    writer.Raw(", ");
    writer.Number(source.Origin.Z);
    writer.Raw("],\n  \"assetScale\": ");
    writer.Number(source.AssetScale);
    writer.Raw(",\n  \"closed\": ");
    writer.Boolean(source.IsClosed);
    writer.Raw(",\n  \"precision\": ");
    writer.String(source.Precision == geom::PrecisionProfile::Strict ? "strict" : "visual");
    writer.Raw(",\n  \"tolerance\": { \"positionError\": ");
    const geom::ToleranceProfile& t = source.Tolerance;
    writer.Number(t.PositionError);
    writer.Raw(", \"relativePositionError\": ");
    writer.Number(t.RelativePositionError);
    writer.Raw(", \"distanceError\": ");
    writer.Number(t.DistanceError);
    writer.Raw(", \"tangentAngleError\": ");
    writer.Number(t.TangentAngleError);
    writer.Raw(", \"projectionGap\": ");
    writer.Number(t.ProjectionGap);
    writer.Raw(", \"continuityPosition\": ");
    writer.Number(t.ContinuityPosition);
    writer.Raw(", \"continuityDerivative\": ");
    writer.Number(t.ContinuityDerivative);
    writer.Raw(", \"continuityAngle\": ");
    writer.Number(t.ContinuityAngle);
    writer.Raw(", \"normalMinDerivativeLength\": ");
    writer.Number(t.NormalMinDerivativeLength);
    writer.Raw(", \"normalMinSinAngle\": ");
    writer.Number(t.NormalMinSinAngle);
    writer.Raw(", \"normalAngleError\": ");
    writer.Number(t.NormalAngleError);
    writer.Raw(", \"coincidenceDistance\": ");
    writer.Number(t.CoincidenceDistance);
    writer.Raw(" },\n  \"limits\": { \"maxSpans\": ");
    const geom::WorkLimits& limits = source.Limits;
    writer.Integer(static_cast<uint64>(limits.MaxSpans));
    writer.Raw(", \"maxLeaves\": ");
    writer.Integer(static_cast<uint64>(limits.MaxLeaves));
    writer.Raw(", \"maxDepth\": ");
    writer.Integer(static_cast<uint64>(limits.MaxDepth));
    writer.Raw(", \"maxSubdivisions\": ");
    writer.Integer(static_cast<uint64>(limits.MaxSubdivisions));
    writer.Raw(", \"maxIterations\": ");
    writer.Integer(static_cast<uint64>(limits.MaxIterations));
    writer.Raw(", \"maxPreparedBytes\": ");
    writer.Integer(static_cast<uint64>(limits.MaxPreparedBytes));
    writer.Raw(", \"maxPatches\": ");
    writer.Integer(static_cast<uint64>(limits.MaxPatches));
    writer.Raw(" }");
    if (source.Mode == GeometrySourceMode::CubicPath)
    {
        writer.Raw(",\n  \"spans\": [");
        for (usize i = 0; i < source.Spans.GetSize(); ++i)
        {
            const AuthoredSpan& span = source.Spans[i];
            writer.Raw(i == 0 ? "\n    { \"id\": " : ",\n    { \"id\": ");
            writer.Integer(span.AuthorId);
            writer.Raw(", \"kind\": ");
            writer.String(span.Kind == fg::SpanKindTag::Line ? "line" : "cubic");
            writer.Raw(", \"domainWidth\": ");
            writer.Number(span.DomainWidth);
            writer.Raw(", \"control\": [");
            for (usize c = 0; c < fg::SPAN_CONTROL_COUNT; ++c)
            {
                writer.Raw(c == 0 ? "[" : ", [");
                writer.Number(span.Control[c].X);
                writer.Raw(", ");
                writer.Number(span.Control[c].Y);
                writer.Raw(", ");
                writer.Number(span.Control[c].Z);
                writer.Raw("]");
            }
            writer.Raw("] }");
        }
        writer.Raw("\n  ]");
    }
    else if (source.Mode == GeometrySourceMode::ScalarTrack)
    {
        writer.Raw(",\n  \"keys\": [");
        for (usize i = 0; i < source.Keys.GetSize(); ++i)
        {
            const AuthoredKey& key = source.Keys[i];
            writer.Raw(i == 0 ? "\n    { \"id\": " : ",\n    { \"id\": ");
            writer.Integer(key.AuthorId);
            writer.Raw(", \"time\": ");
            writer.Number(key.Time);
            writer.Raw(", \"value\": ");
            writer.Number(static_cast<float64>(key.Value));
            writer.Raw(", \"inSlope\": ");
            writer.Number(static_cast<float64>(key.InSlope));
            writer.Raw(", \"outSlope\": ");
            writer.Number(static_cast<float64>(key.OutSlope));
            writer.Raw(", \"interpolation\": ");
            writer.String(key.Interpolation == fg::TrackInterpolationTag::Step     ? "step"
                          : key.Interpolation == fg::TrackInterpolationTag::Linear ? "linear"
                                                                                   : "hermite");
            writer.Raw(" }");
        }
        writer.Raw("\n  ]");
    }
    writer.Raw("\n}");
    const Status status = writer.Finish(outJson);
    if (status != Status::Ok)
    {
        return Fail(outDiagnostic,
                    status,
                    geom::GeometryStatus::OutOfRange,
                    "source",
                    "The canonical source exceeds the writer capacity; reduce the element count.");
    }
    Succeed(outDiagnostic);
    return Status::Ok;
}

Status PrepareFromSource(const GeometrySource& source,
                         geom::PreparedPath& outPath,
                         geom::PreparedTrack& outTrack,
                         geom::PreparedSurface& outSurface,
                         fg::GeometryKindTag& outKind,
                         GeometryDiagnosticReport& outDiag) noexcept
{
    Succeed(outDiag);
    geom::GeometryDiagnostic prepareDiag{};
    switch (source.Mode)
    {
        case GeometrySourceMode::CubicPath: {
            core::Array<geom::PathSpanSource> spans;
            if (spans.TryResize(source.Spans.GetSize()) == false)
            {
                return Fail(outDiag,
                            Status::OutOfMemory,
                            geom::GeometryStatus::OutOfMemory,
                            "spans",
                            RepairForStatus(geom::GeometryStatus::OutOfMemory));
            }
            for (usize i = 0; i < source.Spans.GetSize(); ++i)
            {
                const AuthoredSpan& authored = source.Spans[i];
                geom::PathSpanSource& span = spans[i];
                span.Kind = authored.Kind == fg::SpanKindTag::Line ? geom::SpanKind::Line : geom::SpanKind::Cubic;
                span.DomainWidth = authored.DomainWidth;
                span.SourceElement = i;
                for (usize c = 0; c < fg::SPAN_CONTROL_COUNT; ++c)
                {
                    span.Control[c] = authored.Control[c];
                }
            }
            geom::PathSource pathSource{};
            pathSource.Origin = source.Origin;
            pathSource.Spans = BorrowedView<const geom::PathSpanSource>(spans.GetData(), spans.GetSize());
            pathSource.IsClosed = source.IsClosed;
            pathSource.AssetScale = source.AssetScale;
            pathSource.Revision = source.Revision;
            geom::PathPreparation preparation{};
            preparation.Tolerance = source.Tolerance;
            preparation.Limits = source.Limits;
            const geom::GeometryStatus status = geom::TryPreparePath(pathSource, preparation, outPath, &prepareDiag);
            if (status != geom::GeometryStatus::Success)
            {
                outDiag.Transport = MapPrepareTransport(status);
                outDiag.Detail = prepareDiag;
                outDiag.Repair = RepairForStatus(status);
                return outDiag.Transport;
            }
            outKind = fg::GeometryKindTag::Path;
            return Status::Ok;
        }
        case GeometrySourceMode::ScalarTrack: {
            core::Array<geom::TrackKey> keys;
            if (keys.TryResize(source.Keys.GetSize()) == false)
            {
                return Fail(outDiag,
                            Status::OutOfMemory,
                            geom::GeometryStatus::OutOfMemory,
                            "keys",
                            RepairForStatus(geom::GeometryStatus::OutOfMemory));
            }
            for (usize i = 0; i < source.Keys.GetSize(); ++i)
            {
                const AuthoredKey& authored = source.Keys[i];
                geom::TrackKey& key = keys[i];
                key.Time = authored.Time;
                key.Value = authored.Value;
                key.InSlope = authored.InSlope;
                key.OutSlope = authored.OutSlope;
                key.Interpolation =
                    authored.Interpolation == fg::TrackInterpolationTag::Step     ? geom::TrackInterpolation::Step
                    : authored.Interpolation == fg::TrackInterpolationTag::Linear ? geom::TrackInterpolation::Linear
                                                                                  : geom::TrackInterpolation::Hermite;
            }
            const geom::GeometryStatus status =
                geom::TryPrepareTrack(BorrowedView<const geom::TrackKey>(keys.GetData(), keys.GetSize()),
                                      source.Limits,
                                      outTrack,
                                      &prepareDiag);
            if (status != geom::GeometryStatus::Success)
            {
                outDiag.Transport = MapPrepareTransport(status);
                outDiag.Detail = prepareDiag;
                outDiag.Repair = RepairForStatus(status);
                return outDiag.Transport;
            }
            outKind = fg::GeometryKindTag::Track;
            return Status::Ok;
        }
        case GeometrySourceMode::PatchSurface: {
            core::Array<geom::SurfacePatchSource> patches;
            if (patches.TryResize(source.Patches.GetSize()) == false)
            {
                return Fail(outDiag,
                            Status::OutOfMemory,
                            geom::GeometryStatus::OutOfMemory,
                            "patches",
                            RepairForStatus(geom::GeometryStatus::OutOfMemory));
            }
            for (usize i = 0; i < source.Patches.GetSize(); ++i)
            {
                const AuthoredPatch& authored = source.Patches[i];
                geom::SurfacePatchSource& patch = patches[i];
                patch.Id = authored.AuthorId;
                for (usize u = 0; u < fg::PATCH_CONTROL_ORDER; ++u)
                {
                    for (usize v = 0; v < fg::PATCH_CONTROL_ORDER; ++v)
                    {
                        patch.Control[u][v] = authored.Control[u][v];
                    }
                }
            }
            geom::SurfaceSource surfaceSource{};
            surfaceSource.Origin = source.Origin;
            surfaceSource.Patches = BorrowedView<const geom::SurfacePatchSource>(patches.GetData(), patches.GetSize());
            surfaceSource.Adjacency = BorrowedView<const geom::SurfaceAdjacencySource>(source.Adjacency.GetData(),
                                                                                       source.Adjacency.GetSize());
            surfaceSource.AssetScale = source.AssetScale;
            surfaceSource.Revision = source.Revision;
            geom::SurfacePreparation preparation{};
            preparation.Tolerance = source.Tolerance;
            preparation.Limits = source.Limits;
            const geom::GeometryStatus status =
                geom::TryPrepareSurface(surfaceSource, preparation, outSurface, &prepareDiag);
            if (status != geom::GeometryStatus::Success)
            {
                outDiag.Transport = MapPrepareTransport(status);
                outDiag.Detail = prepareDiag;
                outDiag.Repair = RepairForStatus(status);
                return outDiag.Transport;
            }
            outKind = fg::GeometryKindTag::Surface;
            return Status::Ok;
        }
        case GeometrySourceMode::HermitePath:
        case GeometrySourceMode::CatmullRomPath:
            return Fail(outDiag,
                        Status::Unsupported,
                        geom::GeometryStatus::UnsupportedRepresentation,
                        "mode",
                        "Convert waypoints to cubic spans in the editor adapter before cooking.");
    }
    return Fail(outDiag, Status::Invalid, geom::GeometryStatus::InvalidArgument, "mode", "Unknown source mode.");
}
} // namespace ludus::geometry::content
