#include "internal/host_services.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/sorted_map.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>
#include <string_view>

namespace ludus::runtime::game_host
{
namespace
{
LUDUS_DEFINE_LOG_CATEGORY(LOG_GAME_HOST, "GameHost");

using game_api::HostContext;
using game_api::HostServices;
using game_api::LogSeverity;

// Bounded allocation cap per session so a misbehaving module cannot exhaust the
// host. 64 MiB is generous for the supported fixtures (design 6: bounded).
constexpr usize kMaxAllocationBytes = static_cast<usize>(64) * 1024 * 1024;
constexpr uint64 kWorkClosed = uint64{1} << 63U;
constexpr uint64 kMaxWorkLeases = 1024;

// Header stored just before each allocation so FreeBytes can account exactly.
struct alignas(std::max_align_t) AllocationHeader
{
    usize Size = 0;
    void* Base = nullptr; // Original aligned_alloc pointer.
    HostContext* Owner = nullptr;
    AllocationHeader* Previous = nullptr;
    AllocationHeader* Next = nullptr;
};
} // namespace

struct HostServiceProvider::Context
{
    HostServices Table = {};
    AllocationHeader* Allocations = nullptr;
    bool Retired = false;
    bool Pinned = false;
    Context* NextPinned = nullptr;
    std::atomic<uint64> Outstanding{0};
    std::atomic<usize> BytesLive{0};
    std::atomic<uint64> Work{kWorkClosed};
    ludus::foundation::SortedMap<uint64, uint64> Resources;
};

namespace
{
// A session disables reload at its first uncertain retirement. Keep intentional
// process-lifetime pins reachable even when tests create several sessions.
HostServiceProvider::Context* pinnedContexts = nullptr;

// The HostContext* handed over the ABI is a reinterpret of our Context. It is
// opaque to the module; only our service callbacks interpret it.
[[nodiscard]] HostServiceProvider::Context* Decode(HostContext* ctx) noexcept
{
    return reinterpret_cast<HostServiceProvider::Context*>(ctx);
}

void LogCallback(HostContext* ctx, LogSeverity severity, const char* text, usize length) noexcept
{
    (void)ctx;
    if (text == nullptr || length == 0 || length > 4096)
    {
        return;
    }
    const std::string_view message(text, length);
    switch (severity)
    {
        case LogSeverity::Trace:
            LUDUS_LOG_TRACE(LOG_GAME_HOST, "{}", message);
            break;
        case LogSeverity::Debug:
            LUDUS_LOG_DEBUG(LOG_GAME_HOST, "{}", message);
            break;
        case LogSeverity::Info:
            LUDUS_LOG_INFO(LOG_GAME_HOST, "{}", message);
            break;
        case LogSeverity::Warning:
            LUDUS_LOG_WARN(LOG_GAME_HOST, "{}", message);
            break;
        case LogSeverity::Error:
            LUDUS_LOG_ERROR(LOG_GAME_HOST, "{}", message);
            break;
    }
}

// The size/alignment parameter order is the fixed ABI service signature.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void* AllocateCallback(HostContext* ctx, usize size, usize alignment) noexcept
{
    auto* context = Decode(ctx);
    if (context == nullptr || context->Retired || size == 0 || size > kMaxAllocationBytes || alignment > 4096)
    {
        return nullptr;
    }
    if (alignment < alignof(std::max_align_t))
    {
        alignment = alignof(std::max_align_t);
    }
    // Round up to a power-of-two alignment multiple for aligned_alloc.
    if ((alignment & (alignment - 1)) != 0)
    {
        return nullptr;
    }
    const usize backingSize = size + sizeof(AllocationHeader) + alignment;
    const usize prior = context->BytesLive.load(std::memory_order_relaxed);
    if (backingSize > kMaxAllocationBytes || prior > kMaxAllocationBytes - backingSize)
    {
        return nullptr;
    }

    // Over-allocate room for the header plus worst-case alignment slack, then
    // place the user block with std::align (no integer<->pointer casts).
    void* base = std::malloc(backingSize);
    if (base == nullptr)
    {
        return nullptr;
    }
    void* cursor = static_cast<char*>(base) + sizeof(AllocationHeader);
    usize space = size + alignment;
    void* user = std::align(alignment, size, cursor, space);
    if (user == nullptr)
    {
        std::free(base);
        return nullptr;
    }
    auto* header = reinterpret_cast<AllocationHeader*>(static_cast<char*>(user) - sizeof(AllocationHeader));
    header->Size = backingSize;
    header->Base = base;
    header->Owner = ctx;
    header->Previous = nullptr;
    header->Next = context->Allocations;
    if (header->Next != nullptr)
    {
        header->Next->Previous = header;
    }
    context->Allocations = header;

    context->BytesLive.fetch_add(backingSize, std::memory_order_relaxed);
    context->Outstanding.fetch_add(1, std::memory_order_relaxed);
    return user;
}

void FreeCallback(HostContext* ctx, void* ptr) noexcept
{
    auto* context = Decode(ctx);
    if (context == nullptr || ptr == nullptr)
    {
        return;
    }
    auto* header = reinterpret_cast<AllocationHeader*>(static_cast<char*>(ptr) - sizeof(AllocationHeader));
    if (header->Owner != ctx)
    {
        return;
    }
    if (header->Previous != nullptr)
    {
        header->Previous->Next = header->Next;
    }
    else
    {
        context->Allocations = header->Next;
    }
    if (header->Next != nullptr)
    {
        header->Next->Previous = header->Previous;
    }
    const usize size = header->Size;
    void* base = header->Base;
    std::free(base);
    context->BytesLive.fetch_sub(size, std::memory_order_relaxed);
    context->Outstanding.fetch_sub(1, std::memory_order_relaxed);
}

uint64 ResolveResourceCallback(HostContext* ctx, uint64 logicalAssetId) noexcept
{
    auto* context = Decode(ctx);
    if (context == nullptr || context->Retired)
    {
        return 0;
    }
    const uint64* resource = context->Resources.Find(logicalAssetId);
    return resource == nullptr ? 0 : *resource;
}

bool AcquireWorkCallback(HostContext* ctx) noexcept
{
    auto* context = Decode(ctx);
    if (context == nullptr)
    {
        return false;
    }
    uint64 state = context->Work.load(std::memory_order_acquire);
    while (state < kMaxWorkLeases)
    {
        if (context->Work.compare_exchange_weak(state, state + 1, std::memory_order_acq_rel))
        {
            return true;
        }
    }
    return false;
}

bool ReleaseWorkCallback(HostContext* ctx) noexcept
{
    auto* context = Decode(ctx);
    if (context == nullptr)
    {
        return false;
    }
    uint64 state = context->Work.load(std::memory_order_acquire);
    while ((state & ~kWorkClosed) != 0)
    {
        if (context->Work.compare_exchange_weak(state, state - 1, std::memory_order_acq_rel))
        {
            return true;
        }
    }
    return false;
}
} // namespace

HostServiceProvider::HostServiceProvider() noexcept : Context_(new (std::nothrow) Context())
{
    if (Context_ != nullptr)
    {
        auto& table = Context_->Table;
        table.StructSize = static_cast<ludus::foundation::uint32>(sizeof(HostServices));
        table.Context = reinterpret_cast<HostContext*>(Context_);
        table.Log = &LogCallback;
        table.AllocateBytes = &AllocateCallback;
        table.FreeBytes = &FreeCallback;
        table.ResolveResource = &ResolveResourceCallback;
        table.AcquireWorkLease = &AcquireWorkCallback;
        table.ReleaseWorkLease = &ReleaseWorkCallback;
    }
}

HostServiceProvider::HostServiceProvider(HostServiceProvider&& other) noexcept : Context_(other.Context_)
{
    // The Context pointer is heap-stable; the moved table still references it.
    other.Context_ = nullptr;
}

HostServiceProvider& HostServiceProvider::operator=(HostServiceProvider&& other) noexcept
{
    if (this != &other)
    {
        Retire();
        if (OutstandingAllocations() != 0 || OutstandingWork() != 0)
        {
            PinUntilProcessExit();
        }
        if (Context_ != nullptr && Context_->Pinned == false && OutstandingAllocations() == 0 && OutstandingWork() == 0)
        {
            delete Context_;
        }
        Context_ = other.Context_;
        other.Context_ = nullptr;
    }
    return *this;
}

HostServiceProvider::~HostServiceProvider() noexcept
{
    Retire();
    if (OutstandingAllocations() != 0 || OutstandingWork() != 0)
    {
        PinUntilProcessExit();
    }
    // A failed retirement keeps the table/context alive until process exit.
    // It must never leave an outstanding caller with dangling service storage.
    if (Context_ != nullptr && Context_->Pinned == false && OutstandingAllocations() == 0 && OutstandingWork() == 0)
    {
        delete Context_;
    }
    Context_ = nullptr;
}

const HostServices& HostServiceProvider::Services() const noexcept
{
    static const HostServices empty = {};
    return Context_ == nullptr ? empty : Context_->Table;
}

void HostServiceProvider::PinUntilProcessExit() noexcept
{
    Retire();
    if (Context_ != nullptr && Context_->Pinned == false)
    {
        Context_->Pinned = true;
        Context_->NextPinned = pinnedContexts;
        pinnedContexts = Context_;
    }
}

void HostServiceProvider::Retire() noexcept
{
    GateWork();
    if (Context_ != nullptr)
    {
        Context_->Retired = true;
    }
}

void HostServiceProvider::GateWork() noexcept
{
    if (Context_ != nullptr)
    {
        Context_->Work.fetch_or(kWorkClosed, std::memory_order_acq_rel);
    }
}

void HostServiceProvider::Activate() noexcept
{
    if (Context_ != nullptr && Context_->Retired == false)
    {
        Context_->Work.fetch_and(~kWorkClosed, std::memory_order_acq_rel);
    }
}

uint64 HostServiceProvider::OutstandingWork() const noexcept
{
    return Context_ == nullptr ? 0 : Context_->Work.load(std::memory_order_acquire) & ~kWorkClosed;
}

bool HostServiceProvider::CopyResourcesFrom(const HostServiceProvider& source) noexcept
{
    if (Context_ == nullptr || source.Context_ == nullptr || Context_->Retired)
    {
        return false;
    }
    // Stage independently. Failure leaves the destination registry untouched.
    ludus::foundation::SortedMap<uint64, uint64> copy;
    if (copy.TryEnsureCapacity(source.Context_->Resources.GetSize()) == false)
    {
        return false;
    }
    for (const auto& entry : source.Context_->Resources)
    {
        copy.AddInPlace(entry.Key, entry.Value);
    }
    Context_->Resources.Swap(copy);
    return true;
}

bool HostServiceProvider::PrepareResource(uint64 logicalAssetId) noexcept
{
    if (Context_ == nullptr || Context_->Retired)
    {
        return false;
    }
    auto& resources = Context_->Resources;
    return resources.Contains(logicalAssetId) || resources.TryEnsureCapacity(resources.GetSize() + 1);
}

uint64 HostServiceProvider::OutstandingAllocations() const noexcept
{
    return Context_ == nullptr ? 0 : Context_->Outstanding.load(std::memory_order_relaxed);
}

bool HostServiceProvider::SetResource(uint64 logicalAssetId, uint64 resourceHandle) noexcept
{
    if (Context_ == nullptr || Context_->Retired)
    {
        return false;
    }
    const auto result = Context_->Resources.TryAddInPlace(logicalAssetId, resourceHandle);
    if (result.Value == nullptr)
    {
        return false;
    }
    *result.Value = resourceHandle;
    return true;
}

} // namespace ludus::runtime::game_host
