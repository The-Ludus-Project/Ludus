#pragma once

#include <ludus/foundation/profiling/memory_capture.hpp>

#include "internal/observation.hpp"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/strings/static_string.hpp>

namespace ludus::foundation::profiling::internal
{
using SiteText = StaticString<MAX_MEMORY_ACTIVITY_SITE_TEXT_BYTES>;

struct SiteDefinition
{
    SiteText SourcePath;
    SiteText Function;
    SiteText Module;
    uint32 Line = 0;
};

struct CapturedDomain
{
    RegisteredAllocationDomain Definition;
    AllocationStatistics Begin;
    AllocationStatistics End;
};

struct SealedActivity
{
    MemoryCaptureId Capture;
    uint64 BeginTicks;
    uint64 EndTicks;
    usize Slots;
    usize EventLimit;
    usize ToolingBytes;
    BorrowedView<const CapturedDomain> Domains;
    BorrowedView<const SiteDefinition> Sites;
    const memory_internal::Observation& Recorder;
};

[[nodiscard]] bool
WriteActivityArtifact(const SealedActivity& activity, MemoryArtifactWriter writer, void* contextOrNull) noexcept;
} // namespace ludus::foundation::profiling::internal
