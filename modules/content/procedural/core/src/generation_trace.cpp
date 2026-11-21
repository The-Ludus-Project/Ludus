#include <ludus/content/procedural/core/generation_trace.h>

namespace ludus::content::procedural
{
GenerationTrace::GenerationTrace(usize capacity) noexcept : mCapacity(capacity) {}

void GenerationTrace::Record(const GenerationStageRecord& record) noexcept
{
    if (mRecords.GetSize() >= mCapacity || mRecords.TryAdd(record) == false)
    {
        ++mDroppedCount;
    }
}

void GenerationTrace::Clear() noexcept
{
    mRecords.Clear();
    mDroppedCount = 0;
}
} // namespace ludus::content::procedural
