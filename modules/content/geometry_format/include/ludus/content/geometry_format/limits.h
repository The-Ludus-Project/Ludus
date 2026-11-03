#pragma once

#include <ludus/foundation/base/types.h>

// Admission limits for the cooked geometry interchange format (Ludus::GeometryFormat).
//
// These are the single owner for geometry admission policy (AGENTS.md, "Admission
// limits have one owner per policy"; docs/architecture/content-resources.md#limit-ownership).
// GeometryContent reuses them at read, validation, cook and publication. Encoded
// input, decoded payload counts and peak-memory budgets are kept distinct: the
// byte ceilings bound the encoded blob and the JSON source, the count ceilings
// bound the decoded payload, and peak preparation memory is a separate WorkLimits
// budget owned by Ludus::FoundationGeometry, never one of these constants.
namespace ludus::content::geometry_format
{
using foundation::uint32;
using foundation::usize;

/// Largest admitted cooked geometry blob (decoded-payload admission ceiling).
inline constexpr usize MAX_BLOB_BYTES = usize{16} * 1024 * 1024;
/// Largest admitted JSON geometry source (encoded-source admission ceiling).
inline constexpr usize MAX_SOURCE_BYTES = usize{8} * 1024 * 1024;

/// Most path spans in one cooked path.
inline constexpr uint32 MAX_SPANS = 65536;
/// Most joints in one cooked path (closed paths add the closing joint).
inline constexpr uint32 MAX_JOINTS = 65536;
/// Most scalar-track keys in one cooked track.
inline constexpr uint32 MAX_KEYS = 65536;
/// Most bicubic patches in one cooked surface.
inline constexpr uint32 MAX_PATCHES = 16384;
/// Most explicit adjacency joins in one cooked surface.
inline constexpr uint32 MAX_ADJACENCY = 65536;
/// Most source-identity records mapping a prepared element to its author ID.
inline constexpr uint32 MAX_SOURCE_IDENTITIES = 65536;
/// Most payload sections named by one blob's section directory.
inline constexpr uint32 MAX_SECTIONS = 8;
} // namespace ludus::content::geometry_format
