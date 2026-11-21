#include <ludus/foundation/profiling/memory_capture.hpp>

#include "internal/memory_capture.hpp"

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/memory/owned_object.hpp>
#include <ludus/foundation/profiling/clock.hpp>
#include <ludus/foundation/strings/utf8.hpp>
#include <ludus/foundation/threading/thread.hpp>

#include <string_view>

namespace ludus::foundation::profiling
{
namespace
{
using internal::CapturedDomain;
using internal::SiteDefinition;
using internal::SiteText;
using memory_internal::Observation;
using memory_internal::ObservationConfig;

struct ActivitySession
{
    ActivitySession(const ObservationConfig& config, MemoryCaptureId capture) noexcept;

    Observation Recorder;
    MemoryCaptureId Capture;
    usize Slots;
    usize EventLimit;
    AllocationSiteId SiteCount;
    bool isActive = true;
    uint64 BeginTicks = 0;
    uint64 EndTicks = 0;
    usize DomainCount = 0;
    StaticArray<CapturedDomain, MAX_ALLOCATION_DOMAINS> Domains{};
};

memory_internal::ActivityMetadata ReadActivityMetadata() noexcept
{
    return {NowTicks(), threading::GetCallingThreadId().Value};
}

ActivitySession::ActivitySession(const ObservationConfig& config, MemoryCaptureId capture) noexcept
    : Recorder(config, ReadActivityMetadata), Capture(capture), Slots(config.LiveSlots), EventLimit(config.EventLimit),
      SiteCount(config.SiteCount)
{
}

using SessionOwner = UniquePtr<ActivitySession, DomainDeleter<ActivitySession>>;
constinit SessionOwner gSession;
constinit uint64 gLastCapture = 0;
constinit StaticArray<SiteDefinition, MAX_MEMORY_ACTIVITY_SITES> gSites{};
constinit AllocationSiteId gSiteCount = 0;

bool TryCopySiteText(BorrowedView<const char> source, SiteText& outText) noexcept
{
    if (source.IsValid() == false || source.IsEmpty() == true || source.GetSize() > SiteText::GetCapacity())
    {
        return false;
    }
    Utf8View proof;
    usize errorOffset = 0;
    const std::string_view value(source.GetDataOrNull(), source.GetSize());
    return ValidateCString(value) == StringStatus::Ok && ValidateUtf8(value, proof, errorOffset) == StringStatus::Ok &&
           outText.TryAssign(value) == StringStatus::Ok;
}

bool AreSitesEqual(const SiteDefinition& left, const SiteDefinition& right) noexcept
{
    return left.Line == right.Line && left.SourcePath.GetView() == right.SourcePath.GetView() &&
           left.Function.GetView() == right.Function.GetView() && left.Module.GetView() == right.Module.GetView();
}

bool TryCopyDomains(BorrowedView<const AllocationDomainId> ids,
                    StaticArray<CapturedDomain, MAX_ALLOCATION_DOMAINS>& outDomains) noexcept
{
    if (ids.IsValid() == false || ids.IsEmpty() == true || ids.GetSize() > MAX_ALLOCATION_DOMAINS)
    {
        return false;
    }
    for (usize index = 0; index < ids.GetSize(); ++index)
    {
        const AllocationDomainId id = ids[index];
        if (id == 0 || TryGetRegisteredAllocationDomain(id - 1, outDomains[index].Definition) == false)
        {
            return false;
        }
        for (usize previous = 0; previous < index; ++previous)
        {
            if (ids[previous] == id)
            {
                return false;
            }
        }
    }
    return true;
}

ActivitySession* FindCaptureOrNull(MemoryCaptureId capture) noexcept
{
    ActivitySession* sessionOrNull = gSession.Get();
    return sessionOrNull != nullptr && sessionOrNull->Capture == capture ? sessionOrNull : nullptr;
}
} // namespace

MemoryCaptureStatus RegisterMemorySite(const MemorySiteDefinition& definition, AllocationSiteId& outSite) noexcept
{
    const ActivitySession* sessionOrNull = gSession.Get();
    if (sessionOrNull != nullptr && sessionOrNull->isActive == true)
    {
        return MemoryCaptureStatus::Busy;
    }
    SiteDefinition candidate{};
    candidate.Line = definition.Line;
    if (definition.Line == 0 || TryCopySiteText(definition.SourcePath, candidate.SourcePath) == false ||
        TryCopySiteText(definition.Function, candidate.Function) == false ||
        TryCopySiteText(definition.Module, candidate.Module) == false)
    {
        return MemoryCaptureStatus::InvalidInput;
    }
    for (AllocationSiteId index = 0; index < gSiteCount; ++index)
    {
        if (AreSitesEqual(gSites[index], candidate) == true)
        {
            outSite = index + 1;
            return MemoryCaptureStatus::Ok;
        }
    }
    if (gSiteCount == MAX_MEMORY_ACTIVITY_SITES)
    {
        return MemoryCaptureStatus::StorageExhausted;
    }
    gSites[gSiteCount] = candidate;
    outSite = ++gSiteCount;
    return MemoryCaptureStatus::Ok;
}

MemoryCaptureStatus BeginMemoryCapture(const MemoryActivityConfig& config, MemoryCaptureId& outCapture) noexcept
{
    if (gSession.Get() != nullptr)
    {
        return MemoryCaptureStatus::Busy;
    }
    const usize slots = config.LiveSlots == 0 ? memory_internal::MAX_ACTIVITY_LIVE_SLOTS : config.LiveSlots;
    const usize events = config.Events == 0 ? memory_internal::MAX_ACTIVITY_EVENTS : config.Events;
    StaticArray<CapturedDomain, MAX_ALLOCATION_DOMAINS> domains{};
    if (slots < 2 || slots > memory_internal::MAX_ACTIVITY_LIVE_SLOTS ||
        events > memory_internal::MAX_ACTIVITY_EVENTS || TryCopyDomains(config.Domains, domains) == false)
    {
        return MemoryCaptureStatus::InvalidInput;
    }
    const ObservationConfig recorderConfig = { .LiveSlots = slots, .EventLimit = events, .SiteCount = gSiteCount };
    constexpr MemoryCaptureId LAST_CAPTURE_ID = static_cast<MemoryCaptureId>(-1);
    if (gLastCapture == LAST_CAPTURE_ID ||
        TryCreateOwned(memory_internal::GetCaptureStorageDomain(), gSession, recorderConfig, gLastCapture + 1) !=
            OwnershipStatus::Ok)
    {
        return MemoryCaptureStatus::StorageExhausted;
    }
    ActivitySession& session = *gSession;
    session.Domains = domains;
    session.DomainCount = config.Domains.GetSize();
    session.BeginTicks = NowTicks();
    for (usize index = 0; index < session.DomainCount; ++index)
    {
        CapturedDomain& entry = session.Domains[index];
        const AllocationDomain& domain = *entry.Definition.DomainOrNull;
        entry.Begin = GetAllocationStatistics(domain);
        Observation::Attach(domain, entry.Definition.Id, session.Recorder);
    }
    outCapture = ++gLastCapture;
    return MemoryCaptureStatus::Ok;
}

MemoryCaptureStatus EndMemoryCapture(MemoryCaptureId capture) noexcept
{
    ActivitySession* sessionOrNull = FindCaptureOrNull(capture);
    if (sessionOrNull == nullptr)
    {
        return MemoryCaptureStatus::UnknownCapture;
    }
    if (sessionOrNull->isActive == false)
    {
        return MemoryCaptureStatus::WrongState;
    }
    bool isIncomplete = sessionOrNull->Recorder.GetLoss() != memory_internal::ActivityLoss::None;
    for (usize index = 0; index < sessionOrNull->DomainCount; ++index)
    {
        CapturedDomain& entry = sessionOrNull->Domains[index];
        const AllocationDomain& domain = *entry.Definition.DomainOrNull;
        Observation::Detach(domain);
        entry.End = GetAllocationStatistics(domain);
        isIncomplete = isIncomplete == true || entry.End.Validity != AccountingValidity::Valid;
    }
    sessionOrNull->EndTicks = NowTicks();
    sessionOrNull->isActive = false;
    return isIncomplete == true ? MemoryCaptureStatus::Incomplete : MemoryCaptureStatus::Ok;
}

MemoryCaptureStatus
ExportMemoryCapture(MemoryCaptureId capture, MemoryArtifactWriter writer, void* contextOrNull) noexcept
{
    const ActivitySession* sessionOrNull = FindCaptureOrNull(capture);
    if (sessionOrNull == nullptr)
    {
        return MemoryCaptureStatus::UnknownCapture;
    }
    if (sessionOrNull->isActive == true)
    {
        return MemoryCaptureStatus::WrongState;
    }
    if (writer == nullptr)
    {
        return MemoryCaptureStatus::InvalidInput;
    }
    const internal::SealedActivity activity =
    {
        .Capture = capture,
        .BeginTicks = sessionOrNull->BeginTicks,
        .EndTicks = sessionOrNull->EndTicks,
        .Slots = sessionOrNull->Slots,
        .EventLimit = sessionOrNull->EventLimit,
        .ToolingBytes = sizeof(ActivitySession) + sizeof(gSites),
        .Domains = {sessionOrNull->Domains.GetData(), sessionOrNull->DomainCount},
        .Sites = {gSites.GetData(), sessionOrNull->SiteCount},
        .Recorder = sessionOrNull->Recorder,
    };
    return internal::WriteActivityArtifact(activity, writer, contextOrNull) == true ? MemoryCaptureStatus::Ok
                                                                                    : MemoryCaptureStatus::WriteFailed;
}

MemoryCaptureStatus ReleaseMemoryCapture(MemoryCaptureId capture) noexcept
{
    const ActivitySession* sessionOrNull = FindCaptureOrNull(capture);
    if (sessionOrNull == nullptr)
    {
        return MemoryCaptureStatus::UnknownCapture;
    }
    if (sessionOrNull->isActive == true)
    {
        return MemoryCaptureStatus::WrongState;
    }
    gSession.Reset();
    return MemoryCaptureStatus::Ok;
}
} // namespace ludus::foundation::profiling
