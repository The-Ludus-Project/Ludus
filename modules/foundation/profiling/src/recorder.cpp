#include "internal/recorder.hpp"

#include <ludus/foundation/base/assert.hpp>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>

#include <new>

namespace ludus::foundation::profiling::internal
{

constinit std::atomic<bool> gCaptureActive{false};

// -----------------------------------------------------------------------------
// MVP collector topology (documented deviation from the design's §23 diagram).
//
// The final design shows a dedicated collector thread draining producers
// continuously. For Phase 1 (explicit capture sessions, not the continuous ring
// / hitch trigger of Phase 3) a background thread buys nothing and adds join /
// lifetime surface. So the MVP drains SYNCHRONOUSLY: producers hand full chunks
// to the recorder's full list, and EndCapture() (or the exporter) drains that
// list on the calling thread. The dedicated collector thread lands with Phase 3,
// where continuous rolling capture needs it.
//
// Lifetime protocol: see internal/recorder.hpp. The slab comes from the system
// AllocationDomain with TraceChunk's 64-byte alignment and is released through
// the same domain with the same size and alignment. The previous calloc/free
// pair did not portably guarantee that alignment for placement construction.
// -----------------------------------------------------------------------------

// Never destroyed: TLS destructors of threads that outlive static destruction
// still detach safely. Constant-initialized, so there is no initialization-order
// dependency on first use.
union ImmortalTraceRecorder
{
    constexpr ImmortalTraceRecorder() noexcept : Recorder() {}
    ImmortalTraceRecorder(const ImmortalTraceRecorder&) = delete;
    ImmortalTraceRecorder& operator=(const ImmortalTraceRecorder&) = delete;
    ImmortalTraceRecorder(ImmortalTraceRecorder&&) = delete;
    ImmortalTraceRecorder& operator=(ImmortalTraceRecorder&&) = delete;
    ~ImmortalTraceRecorder() noexcept {}

    TraceRecorder Recorder;
};

namespace
{
constinit ImmortalTraceRecorder gTraceRecorder;
} // namespace

TraceRecorder& TraceRecorder::Instance() noexcept
{
    return gTraceRecorder.Recorder;
}

ThreadRecorder& CurrentThreadRecorder() noexcept
{
    thread_local ThreadRecorder recorder;
    return recorder;
}

ThreadRecorder::~ThreadRecorder() noexcept
{
    // Thread destruction: publish any partial chunk so its events are not lost
    // (§21). The recorder owns the chunk memory, so we only publish, never free.
    TraceRecorder::Instance().DetachThread(*this);
}

// ---- Chunk lists (mMutex held) ----------------------------------------------

void TraceRecorder::pushFree(TraceChunk* chunk) noexcept
{
    chunk->Count = 0;
    chunk->PoolNextOrNull = mFreeHeadOrNull;
    mFreeHeadOrNull = chunk;
    mHasFreeChunks.store(true, std::memory_order_relaxed);
}

void TraceRecorder::pushFull(TraceChunk* chunk) noexcept
{
    chunk->PoolNextOrNull = mFullHeadOrNull;
    mFullHeadOrNull = chunk;
}

TraceChunk* TraceRecorder::popFree() noexcept
{
    TraceChunk* chunk = mFreeHeadOrNull;
    if (chunk == nullptr)
    {
        return nullptr;
    }
    mFreeHeadOrNull = chunk->PoolNextOrNull;
    chunk->PoolNextOrNull = nullptr;
    if (mFreeHeadOrNull == nullptr)
    {
        mHasFreeChunks.store(false, std::memory_order_relaxed);
    }
    return chunk;
}

void TraceRecorder::returnHeldChunk(ThreadRecorder& inoutRecorder) noexcept
{
    TraceChunk* chunk = inoutRecorder.CurrentOrNull;
    if (chunk == nullptr)
    {
        return;
    }
    inoutRecorder.CurrentOrNull = nullptr;
    if (chunk->Count > 0)
    {
        pushFull(chunk);
    }
    else
    {
        pushFree(chunk);
    }
}

void TraceRecorder::linkProducer(ThreadRecorder& inoutRecorder) noexcept
{
    if (inoutRecorder.IsLinked == true)
    {
        return;
    }
    inoutRecorder.PreviousOrNull = nullptr;
    inoutRecorder.NextOrNull = mProducersOrNull;
    if (mProducersOrNull != nullptr)
    {
        mProducersOrNull->PreviousOrNull = &inoutRecorder;
    }
    mProducersOrNull = &inoutRecorder;
    inoutRecorder.IsLinked = true;
}

void TraceRecorder::unlinkProducer(ThreadRecorder& inoutRecorder) noexcept
{
    if (inoutRecorder.IsLinked == false)
    {
        return;
    }
    if (inoutRecorder.PreviousOrNull != nullptr)
    {
        inoutRecorder.PreviousOrNull->NextOrNull = inoutRecorder.NextOrNull;
    }
    else
    {
        mProducersOrNull = inoutRecorder.NextOrNull;
    }
    if (inoutRecorder.NextOrNull != nullptr)
    {
        inoutRecorder.NextOrNull->PreviousOrNull = inoutRecorder.PreviousOrNull;
    }
    inoutRecorder.NextOrNull = nullptr;
    inoutRecorder.PreviousOrNull = nullptr;
    inoutRecorder.IsLinked = false;
}

bool TraceRecorder::isAnyProducerEmitting() const noexcept
{
    for (const ThreadRecorder* producer = mProducersOrNull; producer != nullptr; producer = producer->NextOrNull)
    {
        // Sequentially consistent: pairs with Emit()'s announce-then-test.
        if (producer->IsEmitting.load(std::memory_order_seq_cst) == true)
        {
            return true;
        }
    }
    return false;
}

void TraceRecorder::harvestProducerChunks() noexcept
{
    for (ThreadRecorder* producer = mProducersOrNull; producer != nullptr; producer = producer->NextOrNull)
    {
        returnHeldChunk(*producer);
    }
}

// ---- Slab (mMutex held) -----------------------------------------------------

bool TraceRecorder::reservePool(usize chunkCount) noexcept
{
    usize bytes = 0;
    if (TryMultiply(chunkCount, sizeof(TraceChunk), bytes) == false)
    {
        return false;
    }
    void* storage = GetSystemAllocationDomain().TryAllocate(bytes, alignof(TraceChunk));
    if (storage == nullptr)
    {
        return false; // OOM: leave capture disabled, engine runs normally.
    }
    mPoolStorageOrNull = static_cast<TraceChunk*>(storage);
    mPoolCapacity = chunkCount;
    mPoolBytes = bytes;
    for (usize i = 0; i < chunkCount; ++i)
    {
        // Value-initialization zeroes each chunk's events, as the former calloc did.
        (void)new (&mPoolStorageOrNull[i]) TraceChunk();
    }
    return true;
}

void TraceRecorder::releasePool() noexcept
{
    mFreeHeadOrNull = nullptr;
    mFullHeadOrNull = nullptr;
    mHasFreeChunks.store(false, std::memory_order_relaxed);
    if (mPoolStorageOrNull == nullptr)
    {
        return;
    }
    for (usize i = 0; i < mPoolCapacity; ++i)
    {
        mPoolStorageOrNull[i].~TraceChunk();
    }
    GetSystemAllocationDomain().Free(mPoolStorageOrNull, mPoolBytes, alignof(TraceChunk));
    mPoolStorageOrNull = nullptr;
    mPoolCapacity = 0;
    mPoolBytes = 0;
}

// ---- Capture lifecycle ------------------------------------------------------

bool TraceRecorder::BeginCapture(usize maxChunks) noexcept
{
    const std::lock_guard<std::mutex> lock(mMutex);
    if (gCaptureActive.load(std::memory_order_relaxed) == true)
    {
        return false;
    }
    if (maxChunks == 0)
    {
        maxChunks = 1;
    }

    // No producer holds a chunk here: chunks are acquired only while armed, and
    // the preceding EndCapture() harvested every held chunk.
    for (const ThreadRecorder* producer = mProducersOrNull; producer != nullptr; producer = producer->NextOrNull)
    {
        LUDUS_ASSERT(producer->CurrentOrNull == nullptr, "trace producer kept a chunk across EndCapture");
    }

    // Pre-reserve the entire pool up front so nothing allocates on the hot path
    // (§21). One contiguous slab; every chunk is threaded onto the free list.
    if (mPoolStorageOrNull == nullptr || mPoolCapacity != maxChunks)
    {
        releasePool();
        if (reservePool(maxChunks) == false)
        {
            return false;
        }
    }
    mFreeHeadOrNull = nullptr;
    mFullHeadOrNull = nullptr;
    for (usize i = mPoolCapacity; i > 0; --i)
    {
        TraceChunk& chunk = mPoolStorageOrNull[i - 1];
        chunk.ThreadId = 0;
        pushFree(&chunk);
    }

    mEventsRecorded.store(0, std::memory_order_relaxed);
    mEventsDropped.store(0, std::memory_order_relaxed);
    gCaptureActive.store(true, std::memory_order_seq_cst);
    return true;
}

void TraceRecorder::EndCapture() noexcept
{
    // Disarm first so producers stop acquiring and writing chunks.
    gCaptureActive.store(false, std::memory_order_seq_cst);
    for (;;)
    {
        {
            const std::lock_guard<std::mutex> lock(mMutex);
            if (isAnyProducerEmitting() == false)
            {
                harvestProducerChunks();
                return;
            }
        }
        // A producer is between announcing an emit and finishing its write. It
        // may need the mutex to finish (chunk handoff), so wait with the mutex
        // released. The wait is bounded by that producer's scheduling: an emit
        // performs no blocking work other than this mutex.
    }
}

uint32 TraceRecorder::RegisterThread(std::string_view name) noexcept
{
    ThreadRecorder& recorder = CurrentThreadRecorder();
    if (recorder.IsRegistered == true)
    {
        return recorder.ThreadId;
    }
    const uint32 id = mNextThreadId.fetch_add(1, std::memory_order_relaxed);
    recorder.ThreadId = id;
    recorder.IsRegistered = true;

    // Bounded table: append under the mutex, then publish the entry.
    const std::lock_guard<std::mutex> lock(mMutex);
    const uint32 slot = mThreadNameCount.load(std::memory_order_relaxed);
    if (slot < MAX_THREADS)
    {
        ThreadNameEntry& entry = mThreadNames[slot];
        entry.Id = id;
        const usize length = name.size() < THREAD_NAME_CAPACITY ? name.size() : THREAD_NAME_CAPACITY;
        for (usize i = 0; i < length; ++i)
        {
            entry.Name[i] = name[i];
        }
        entry.Length = static_cast<uint16>(length);
        mThreadNameCount.store(slot + 1, std::memory_order_release);
    }
    return id;
}

std::string_view TraceRecorder::ThreadName(uint32 threadId) const noexcept
{
    const uint32 count = mThreadNameCount.load(std::memory_order_acquire);
    for (uint32 i = 0; i < count; ++i)
    {
        if (mThreadNames[i].Id == threadId)
        {
            return std::string_view(mThreadNames[i].Name, mThreadNames[i].Length);
        }
    }
    return std::string_view{};
}

uint32 TraceRecorder::ThreadIdAt(usize index) const noexcept
{
    return index < ThreadCount() ? mThreadNames[index].Id : 0;
}

usize TraceRecorder::ThreadCount() const noexcept
{
    return mThreadNameCount.load(std::memory_order_acquire);
}

void TraceRecorder::RegisterSite(uint32 siteId, std::string_view name, std::string_view file, uint32 line) noexcept
{
    // Called once per site. Bounded table; a site past capacity is dropped from
    // the name table (events still record, the exporter falls back to the id).
    const std::lock_guard<std::mutex> lock(mMutex);
    const uint32 slot = mSiteCount.load(std::memory_order_relaxed);
    if (slot < MAX_SITES)
    {
        SiteEntry& entry = mSites[slot];
        entry.SiteId = siteId;
        entry.Name = name;
        entry.File = file;
        entry.Line = line;
        mSiteCount.store(slot + 1, std::memory_order_release);
    }
}

std::string_view TraceRecorder::SiteName(uint32 siteId) const noexcept
{
    const uint32 count = mSiteCount.load(std::memory_order_acquire);
    for (uint32 i = 0; i < count; ++i)
    {
        if (mSites[i].SiteId == siteId)
        {
            return mSites[i].Name;
        }
    }
    return std::string_view{};
}

void TraceRecorder::UnregisterThread() noexcept
{
    ThreadRecorder& recorder = CurrentThreadRecorder();
    DetachThread(recorder);
    recorder.IsRegistered = false;
}

void TraceRecorder::DetachThread(ThreadRecorder& inoutRecorder) noexcept
{
    // Runs on the owning thread, which is not emitting. A held chunk is still
    // valid storage: it is cleared by EndCapture() before any slab release.
    const std::lock_guard<std::mutex> lock(mMutex);
    returnHeldChunk(inoutRecorder);
    unlinkProducer(inoutRecorder);
}

TraceChunk* TraceRecorder::replaceChunk(ThreadRecorder& inoutRecorder) noexcept
{
    const std::lock_guard<std::mutex> lock(mMutex);
    if (inoutRecorder.CurrentOrNull != nullptr)
    {
        pushFull(inoutRecorder.CurrentOrNull);
        inoutRecorder.CurrentOrNull = nullptr;
    }
    // EndCapture() disarms before taking the mutex; never acquire a chunk after it.
    if (gCaptureActive.load(std::memory_order_relaxed) == false)
    {
        return nullptr;
    }
    linkProducer(inoutRecorder);
    TraceChunk* chunk = popFree();
    if (chunk == nullptr)
    {
        mEventsDropped.fetch_add(1, std::memory_order_relaxed);
        return nullptr; // pool exhausted: drop + count (§21 overflow policy)
    }
    chunk->ThreadId = inoutRecorder.ThreadId;
    chunk->Count = 0;
    inoutRecorder.CurrentOrNull = chunk;
    return chunk;
}

bool TraceRecorder::Emit(const TraceEvent& event) noexcept
{
    ThreadRecorder& recorder = CurrentThreadRecorder();

    // Announce the in-flight write before testing the capture flag. Both sides
    // are sequentially consistent (pairs with EndCapture), so EndCapture cannot
    // miss a producer that observed an armed capture.
    recorder.IsEmitting.store(true, std::memory_order_seq_cst);
    if (gCaptureActive.load(std::memory_order_seq_cst) == false)
    {
        recorder.IsEmitting.store(false, std::memory_order_release);
        return false;
    }

    TraceChunk* chunk = recorder.CurrentOrNull;
    if (chunk == nullptr || chunk->Count >= kEventsPerChunk)
    {
        if (chunk == nullptr && mHasFreeChunks.load(std::memory_order_relaxed) == false)
        {
            // Pool already exhausted and nothing to publish: drop without the mutex.
            mEventsDropped.fetch_add(1, std::memory_order_relaxed);
            recorder.IsEmitting.store(false, std::memory_order_release);
            return false;
        }
        chunk = replaceChunk(recorder);
        if (chunk == nullptr)
        {
            recorder.IsEmitting.store(false, std::memory_order_release);
            return false;
        }
    }

    chunk->Events[chunk->Count] = event;
    ++chunk->Count;
    mEventsRecorded.fetch_add(1, std::memory_order_relaxed);
    // Release: EndCapture's acquiring observation of `false` makes this chunk's
    // writes visible before it publishes the chunk.
    recorder.IsEmitting.store(false, std::memory_order_release);
    return true;
}

TraceChunk* TraceRecorder::DrainFullChunks() noexcept
{
    const std::lock_guard<std::mutex> lock(mMutex);
    TraceChunk* head = mFullHeadOrNull;
    mFullHeadOrNull = nullptr;
    return head;
}

bool TraceRecorder::HasPublishedChunks() const noexcept
{
    const std::lock_guard<std::mutex> lock(mMutex);
    return mFullHeadOrNull != nullptr;
}

void TraceRecorder::RecycleChunk(TraceChunk* chunk) noexcept
{
    const std::lock_guard<std::mutex> lock(mMutex);
    pushFree(chunk);
}

TraceHealth TraceRecorder::Health() const noexcept
{
    TraceHealth health;
    health.EventsRecorded = mEventsRecorded.load(std::memory_order_relaxed);
    health.EventsDropped = mEventsDropped.load(std::memory_order_relaxed);
    health.ThreadsRegistered = ThreadCount();
    return health;
}

bool TraceRecorder::Reset() noexcept
{
    const std::lock_guard<std::mutex> lock(mMutex);
    if (gCaptureActive.load(std::memory_order_relaxed) == true)
    {
        return false;
    }
    releasePool();
    return true;
}

usize TraceRecorder::PoolCapacity() const noexcept
{
    const std::lock_guard<std::mutex> lock(mMutex);
    return mPoolCapacity;
}

} // namespace ludus::foundation::profiling::internal
