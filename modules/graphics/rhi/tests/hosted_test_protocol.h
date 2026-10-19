#pragma once

// Shared by the hosted surface producer child (hosted_producer.cpp) and the
// two-process tests that drive it.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>

namespace ludus::graphics::rhi::hosted_test
{
/// Extent the editor endpoint starts with, in backing pixels.
inline constexpr foundation::uint32 INITIAL_WIDTH = 64;
/// Extent the editor endpoint starts with, in backing pixels.
inline constexpr foundation::uint32 INITIAL_HEIGHT = 48;
/// The producer's clear colour (unorm RGBA).
inline constexpr foundation::StaticArray<foundation::float64, 4> HOSTED_TEST_COLOR = {0.25, 0.5, 0.75, 1.0};
/// The same colour as 8-bit RGBA after unorm conversion (round to nearest).
inline constexpr foundation::StaticArray<foundation::uint8, 4> HOSTED_TEST_BYTES = {64, 128, 191, 255};
/// Largest difference per channel accepted between expected and read bytes.
inline constexpr foundation::uint8 COLOR_TOLERANCE = 1;
/// Adapter UUID byte of the forged producer, which no real adapter reports.
inline constexpr foundation::uint8 FORGED_UUID_BYTE = 0xFF;
/// Allocation size the forged producer announces.
inline constexpr foundation::uint64 FORGED_ALLOCATION_BYTES = 4096;

/// What the producer reports on standard output when its window closes.
struct ProducerStatistics final
{
    /// Frames for which BeginFrame returned Ready.
    foundation::uint32 ReadyFrames = 0;
    /// Frames for which BeginFrame returned Skipped.
    foundation::uint32 SkippedFrames = 0;
    /// Longest BeginFrame call that returned Skipped.
    foundation::uint64 MaxSkippedNanoseconds = 0;
};
} // namespace ludus::graphics::rhi::hosted_test
