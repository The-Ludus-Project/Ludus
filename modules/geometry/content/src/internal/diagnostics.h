#pragma once

// Private diagnostic helpers for Ludus::GeometryContent. Every failure sets a
// structured report (content transport status, ADR 0024 geometry diagnostic and
// a concrete repair suggestion) and returns the transport status, so bad content
// is reported as data and never as an assertion (AGENTS.md, "Errors offer
// solutions"). Mirrors scene_cook's Failure helper.

#include <ludus/foundation/base/core.h>

#include <ludus/geometry/content/geometry_content.h>

namespace ludus::geometry::content::internal
{
/// @brief Set a failure report and return its transport status.
/// @param outReport Report to populate.
/// @param transport Content transport status returned to the caller.
/// @param code Geometry status code recorded in the diagnostic.
/// @param property Static property path; never owned text.
/// @param repair Static concrete repair suggestion.
/// @param sourceElement Offending source element index.
[[nodiscard]] inline ludus::content::Status Fail(GeometryDiagnosticReport& outReport,
                                                 ludus::content::Status transport,
                                                 geom::GeometryStatus code,
                                                 const char* property,
                                                 const char* repair,
                                                 usize sourceElement = 0) noexcept
{
    outReport.Transport = transport;
    outReport.Detail = geom::GeometryDiagnostic{};
    outReport.Detail.Code = code;
    outReport.Detail.SourceElement = sourceElement;
    outReport.Detail.Property = property;
    outReport.Repair = repair;
    (void)outReport.Message.Set("");
    return transport;
}

/// @brief Clear a report to success.
inline void Succeed(GeometryDiagnosticReport& outReport) noexcept
{
    outReport.Transport = ludus::content::Status::Ok;
    outReport.Detail = geom::GeometryDiagnostic{};
    outReport.Repair = "";
    (void)outReport.Message.Set("");
}

/// @brief Map a FoundationGeometry preparation status to a content transport status.
[[nodiscard]] inline ludus::content::Status MapPrepareTransport(geom::GeometryStatus status) noexcept
{
    switch (status)
    {
    case geom::GeometryStatus::Success:
        return ludus::content::Status::Ok;
    case geom::GeometryStatus::OutOfMemory:
        return ludus::content::Status::OutOfMemory;
    case geom::GeometryStatus::BudgetExceeded:
        return ludus::content::Status::Limit;
    case geom::GeometryStatus::Cancelled:
        return ludus::content::Status::Cancelled;
    case geom::GeometryStatus::UnsupportedRepresentation:
    case geom::GeometryStatus::UnsupportedPrecision:
        return ludus::content::Status::Unsupported;
    default:
        return ludus::content::Status::Invalid;
    }
}

/// @brief A concrete repair suggestion for a preparation status.
[[nodiscard]] inline const char* RepairForStatus(geom::GeometryStatus status) noexcept
{
    switch (status)
    {
    case geom::GeometryStatus::InvalidDomain:
        return "Merge coincident waypoints or make key times strictly increasing.";
    case geom::GeometryStatus::InvalidTopology:
        return "Fix the adjacency join, or remove the partial or disconnected edge.";
    case geom::GeometryStatus::UnsupportedPrecision:
        return "Split this chunk, or relax PositionError to its Certified floor.";
    case geom::GeometryStatus::BudgetExceeded:
        return "Raise MaxLeaves/MaxDepth/MaxSubdivisions, or relax DistanceError.";
    case geom::GeometryStatus::OutOfRange:
        return "Reduce control magnitudes or subtract a nearer chunk origin.";
    case geom::GeometryStatus::OutOfMemory:
        return "Reduce the asset size or raise the preparation memory budget.";
    default:
        return "Correct the reported source element and retry.";
    }
}
} // namespace ludus::geometry::content::internal
