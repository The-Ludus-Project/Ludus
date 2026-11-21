#include "internal/memory_capture.hpp"

#include <ludus/foundation/base/assert_config.hpp>

namespace ludus::foundation::profiling::internal
{
namespace
{
using memory_internal::ActivityEvent;
using memory_internal::ActivityKind;
using memory_internal::ActivityLoss;

constexpr usize OUTPUT_BUFFER_BYTES = 4096;
constexpr usize UINT64_DECIMAL_DIGITS = 20;
constexpr uint64 DECIMAL_RADIX = 10;
constexpr uint8 ASCII_CONTROL_LIMIT = 0x20;
constexpr uint8 HEX_NIBBLE_BITS = 4;
constexpr uint8 HEX_NIBBLE_MASK = 0xf;
constexpr uint64 ACTIVITY_SCHEMA_VERSION = 1;

class ArtifactOutput final
{
public:
    ArtifactOutput(MemoryArtifactWriter writer, void* contextOrNull) noexcept
        : mWriter(writer), mContextOrNull(contextOrNull)
    {
    }

    void WriteText(const char* text) noexcept
    {
        while (*text != '\0')
        {
            writeByte(*text++);
        }
    }

    void WriteString(const char* text) noexcept
    {
        writeByte('"');
        while (*text != '\0')
        {
            const char character = *text++;
            const uint8 byte = static_cast<uint8>(character);
            if (character == '"' || character == '\\')
            {
                writeByte('\\');
                writeByte(character);
            }
            else if (byte < ASCII_CONTROL_LIMIT)
            {
                constexpr char HEX_DIGITS[] = "0123456789abcdef";
                WriteText("\\u00");
                writeByte(HEX_DIGITS[byte >> HEX_NIBBLE_BITS]);
                writeByte(HEX_DIGITS[byte & HEX_NIBBLE_MASK]);
            }
            else
            {
                writeByte(character);
            }
        }
        writeByte('"');
    }

    void WriteNumber(uint64 value) noexcept
    {
        StaticArray<char, UINT64_DECIMAL_DIGITS> digits;
        usize start = digits.GetSize();
        do
        {
            digits[--start] = static_cast<char>('0' + value % DECIMAL_RADIX);
            value /= DECIMAL_RADIX;
        } while (value != 0);
        writeByte('"');
        for (usize index = start; index < digits.GetSize(); ++index)
        {
            writeByte(digits[index]);
        }
        writeByte('"');
    }

    void WriteSeparator(usize index) noexcept
    {
        if (index != 0)
        {
            writeByte(',');
        }
    }

    [[nodiscard]] bool Flush() noexcept
    {
        if (mIsValid == true && mSize != 0)
        {
            mIsValid = mWriter(mContextOrNull, {mBuffer.GetData(), mSize});
        }
        mSize = 0;
        return mIsValid;
    }

    [[nodiscard]] bool IsValid() const noexcept
    {
        return mIsValid;
    }

private:
    void writeByte(char value) noexcept
    {
        if (mSize == mBuffer.GetSize())
        {
            (void)Flush();
        }
        if (mIsValid == true)
        {
            mBuffer[mSize++] = value;
        }
    }

    MemoryArtifactWriter mWriter;
    void* mContextOrNull;
    StaticArray<char, OUTPUT_BUFFER_BYTES> mBuffer;
    usize mSize = 0;
    bool mIsValid = true;
};

const char* GetLossName(ActivityLoss loss) noexcept
{
    switch (loss)
    {
        case ActivityLoss::None:
            return "none";
        case ActivityLoss::JournalFull:
            return "journal-full";
        case ActivityLoss::TableFull:
            return "table-full";
        case ActivityLoss::InvalidOwnership:
            return "invalid-ownership";
        case ActivityLoss::Reentry:
            return "reentry";
    }
    return "invalid";
}

const char* GetKindName(ActivityKind kind) noexcept
{
    switch (kind)
    {
        case ActivityKind::Allocate:
            return "alloc";
        case ActivityKind::Free:
            return "free";
        case ActivityKind::Failure:
            return "failure";
    }
    return "invalid";
}

void WriteCounters(ArtifactOutput& inoutOutput, const AllocationStatistics& counters) noexcept
{
    inoutOutput.WriteText("{\"validity\":");
    inoutOutput.WriteNumber(static_cast<uint8>(counters.Validity));
    inoutOutput.WriteText(",\"liveBytes\":");
    inoutOutput.WriteNumber(counters.LiveRequestedBytes);
    inoutOutput.WriteText(",\"liveBlocks\":");
    inoutOutput.WriteNumber(counters.LiveBlocks);
    inoutOutput.WriteText(",\"peakBytes\":");
    inoutOutput.WriteNumber(counters.PeakRequestedBytes);
    inoutOutput.WriteText(",\"allocatedBytes\":");
    inoutOutput.WriteNumber(counters.AllocatedBytes);
    inoutOutput.WriteText(",\"freedBytes\":");
    inoutOutput.WriteNumber(counters.FreedBytes);
    inoutOutput.WriteText(",\"allocationCount\":");
    inoutOutput.WriteNumber(counters.AllocationCount);
    inoutOutput.WriteText(",\"freeCount\":");
    inoutOutput.WriteNumber(counters.FreeCount);
    inoutOutput.WriteText(",\"failureCount\":");
    inoutOutput.WriteNumber(counters.FailureCount);
    inoutOutput.WriteText(",\"rejectedCount\":");
    inoutOutput.WriteNumber(counters.RejectedCount);
    inoutOutput.WriteText("}");
}

void WriteDefinitions(ArtifactOutput& inoutOutput, const SealedActivity& activity) noexcept
{
    inoutOutput.WriteText(",\"domains\":[");
    for (usize index = 0; index < activity.Domains.GetSize(); ++index)
    {
        const CapturedDomain& entry = activity.Domains[index];
        inoutOutput.WriteSeparator(index);
        inoutOutput.WriteText("{\"id\":");
        inoutOutput.WriteNumber(entry.Definition.Id);
        inoutOutput.WriteText(",\"name\":");
        inoutOutput.WriteString(entry.Definition.NameOrNull);
        inoutOutput.WriteText(",\"begin\":");
        WriteCounters(inoutOutput, entry.Begin);
        inoutOutput.WriteText(",\"end\":");
        WriteCounters(inoutOutput, entry.End);
        inoutOutput.WriteText("}");
    }
    inoutOutput.WriteText("],\"exclusions\":[");
    for (usize index = 0; index < GetAllocationCoverageExclusionCount(); ++index)
    {
        const AllocationCoverageExclusion& entry = GetAllocationCoverageExclusion(index);
        inoutOutput.WriteSeparator(index);
        inoutOutput.WriteText("{\"name\":");
        inoutOutput.WriteString(entry.Name);
        inoutOutput.WriteText(",\"reason\":");
        inoutOutput.WriteString(entry.Reason);
        inoutOutput.WriteText("}");
    }
    inoutOutput.WriteText("],\"sites\":[");
    for (usize index = 0; index < activity.Sites.GetSize(); ++index)
    {
        const SiteDefinition& site = activity.Sites[index];
        inoutOutput.WriteSeparator(index);
        inoutOutput.WriteText("{\"id\":");
        inoutOutput.WriteNumber(index + 1);
        inoutOutput.WriteText(",\"path\":");
        inoutOutput.WriteString(site.SourcePath.GetData());
        inoutOutput.WriteText(",\"function\":");
        inoutOutput.WriteString(site.Function.GetData());
        inoutOutput.WriteText(",\"module\":");
        inoutOutput.WriteString(site.Module.GetData());
        inoutOutput.WriteText(",\"line\":");
        inoutOutput.WriteNumber(site.Line);
        inoutOutput.WriteText("}");
    }
    inoutOutput.WriteText("]");
}

void WriteEvent(ArtifactOutput& inoutOutput, usize sequence, const ActivityEvent& event) noexcept
{
    inoutOutput.WriteText("{\"sequence\":");
    inoutOutput.WriteNumber(sequence);
    inoutOutput.WriteText(",\"kind\":");
    inoutOutput.WriteString(GetKindName(event.Kind));
    inoutOutput.WriteText(",\"ticks\":");
    inoutOutput.WriteNumber(event.Metadata.Ticks);
    inoutOutput.WriteText(",\"thread\":");
    inoutOutput.WriteNumber(event.Metadata.Thread);
    inoutOutput.WriteText(",\"domain\":");
    inoutOutput.WriteNumber(event.Domain);
    inoutOutput.WriteText(",\"site\":");
    inoutOutput.WriteNumber(event.Site);
    inoutOutput.WriteText(",\"address\":");
    inoutOutput.WriteNumber(event.Address);
    inoutOutput.WriteText(",\"bytes\":");
    inoutOutput.WriteNumber(event.Bytes);
    inoutOutput.WriteText(",\"alignment\":");
    inoutOutput.WriteNumber(event.Alignment);
    inoutOutput.WriteText(",\"lifetime\":");
    inoutOutput.WriteNumber(event.Lifetime);
    inoutOutput.WriteText("}");
}
} // namespace

bool WriteActivityArtifact(const SealedActivity& activity, MemoryArtifactWriter writer, void* contextOrNull) noexcept
{
    ArtifactOutput output(writer, contextOrNull);
    output.WriteText("{\"schema\":\"ludus-memory-activity\",\"version\":");
    output.WriteNumber(ACTIVITY_SCHEMA_VERSION);
    output.WriteText(",\"buildFlavorId\":");
    output.WriteNumber(LUDUS_BUILD_FLAVOR_ID);
    output.WriteText(",\"capture\":");
    output.WriteNumber(activity.Capture);
    output.WriteText(",\"baseline\":\"unproven\",\"clockUnit\":\"ns\",\"beginTicks\":");
    output.WriteNumber(activity.BeginTicks);
    output.WriteText(",\"endTicks\":");
    output.WriteNumber(activity.EndTicks);
    output.WriteText(",\"liveSlots\":");
    output.WriteNumber(activity.Slots);
    output.WriteText(",\"eventLimit\":");
    output.WriteNumber(activity.EventLimit);
    output.WriteText(",\"toolingBytes\":");
    output.WriteNumber(activity.ToolingBytes);
    WriteDefinitions(output, activity);

    output.WriteText(",\"events\":[");
    const usize eventCount = activity.Recorder.GetEventCount();
    for (usize index = 0; index < eventCount && output.IsValid() == true; ++index)
    {
        output.WriteSeparator(index);
        WriteEvent(output, index + 1, activity.Recorder.GetEvent(index));
    }
    output.WriteText("],\"seal\":{\"eventCount\":");
    output.WriteNumber(eventCount);
    output.WriteText(",\"loss\":");
    output.WriteString(GetLossName(activity.Recorder.GetLoss()));
    output.WriteText(",\"firstLostOperation\":");
    output.WriteNumber(activity.Recorder.GetFirstLostOperation());
    output.WriteText("}}\n");
    return output.Flush();
}
} // namespace ludus::foundation::profiling::internal
