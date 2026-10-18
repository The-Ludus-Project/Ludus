// Thanks to Andrew Kirmse, "A Network Monitoring and Simulation Tool",
// Game Programming Gems 3, section 5.7 (PDF pp. 542-545), for directional
// adverse-link testing. This original fixture uses bounded caller-clock state,
// seeded uniform jitter and equal-deadline duplicates, rather than NetTool's
// Windows proxy, Gaussian timing and independently delayed duplicate copies.
// Review: docs/architecture/networking-reference-review.md.
#include <ludus/network/core/link_simulator.h>

#include <cstring>

namespace ludus::network
{
namespace
{
uint32 NextRandom(uint32& state) noexcept
{
    state = state * 1664525U + 1013904223U;
    return state;
}
} // namespace
LinkStatus LinkSimulator::Reset(const LinkConfig& config) noexcept
{
    if (config.LossPerTenThousand > 10000 || config.DuplicatePerTenThousand > 10000 ||
        config.DelayUs > ~uint64{0} - config.JitterUs)
    {
        return LinkStatus::InvalidArgument;
    }
    mConfig = config;
    mRandom = config.Seed;
    mPending = 0;
    mNowUs = 0;
    mOrder = 0;
    mCounters = {};
    for (auto& packet : mPackets)
    {
        packet.Active = false;
    }
    return LinkStatus::Ok;
}
bool LinkSimulator::AdvanceTime(uint64 nowUs) noexcept
{
    if (nowUs < mNowUs)
    {
        return false;
    }
    mNowUs = nowUs;
    return true;
}
LinkStatus LinkSimulator::Send(uint64 nowUs, std::span<const uint8> message) noexcept
{
    if (message.empty() || message.size() > MAX_MESSAGE_BYTES)
    {
        return LinkStatus::InvalidArgument;
    }
    if (AdvanceTime(nowUs) == false)
    {
        return LinkStatus::InvalidTime;
    }
    uint32 random = mRandom;
    if (NextRandom(random) % 10000 < mConfig.LossPerTenThousand)
    {
        mRandom = random;
        ++mCounters.Dropped;
        return LinkStatus::Dropped;
    }
    const uint64 jitter = static_cast<uint64>(NextRandom(random)) % (static_cast<uint64>(mConfig.JitterUs) + 1);
    const uint64 delay = mConfig.DelayUs + jitter;
    if (nowUs > ~uint64{0} - delay)
    {
        return LinkStatus::TimeOverflow;
    }
    const usize copies = NextRandom(random) % 10000 < mConfig.DuplicatePerTenThousand ? 2 : 1;
    if (copies > SIMULATOR_CAPACITY - mPending)
    {
        ++mCounters.RejectedFull;
        return LinkStatus::Full;
    }
    if (mOrder > ~uint64{0} - copies)
    {
        return LinkStatus::SequenceExhausted;
    }
    usize added = 0;
    for (auto& packet : mPackets)
    {
        if (packet.Active)
        {
            continue;
        }
        std::memcpy(packet.Message.Bytes, message.data(), message.size());
        packet.Message.Size = message.size();
        packet.DueUs = nowUs + delay;
        packet.Order = mOrder++;
        packet.Active = true;
        if (++added == copies)
        {
            break;
        }
    }
    mPending += copies;
    mRandom = random;
    ++mCounters.Accepted;
    if (copies == 2)
    {
        ++mCounters.Duplicated;
    }
    return LinkStatus::Ok;
}
LinkStatus LinkSimulator::Receive(uint64 nowUs, OwnedMessage& message) noexcept
{
    if (AdvanceTime(nowUs) == false)
    {
        return LinkStatus::InvalidTime;
    }
    Pending* selected = nullptr;
    for (auto& packet : mPackets)
    {
        if (packet.Active == false || packet.DueUs > nowUs)
        {
            continue;
        }
        if (selected == nullptr || packet.DueUs < selected->DueUs ||
            (packet.DueUs == selected->DueUs && packet.Order < selected->Order))
        {
            selected = &packet;
        }
    }
    if (selected == nullptr)
    {
        return LinkStatus::Empty;
    }
    std::memcpy(message.Bytes, selected->Message.Bytes, selected->Message.Size);
    message.Size = selected->Message.Size;
    selected->Active = false;
    --mPending;
    ++mCounters.Delivered;
    return LinkStatus::Ok;
}
} // namespace ludus::network
