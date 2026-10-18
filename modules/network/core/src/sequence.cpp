// Thanks to Robert Elz and Randy Bush, RFC 1982, "Serial Number Arithmetic",
// section 3.2 (https://www.rfc-editor.org/rfc/rfc1982.html), for wrap-aware
// comparisons and the half-range boundary. This original 64-message window
// explicitly rejects that ambiguous boundary; it does not implement DNS or ACKs.
#include <ludus/network/core/sequence.h>

namespace ludus::network
{
SequenceStatus SequenceWindow::Observe(uint32 sequence) noexcept
{
    if (mInitialized == false)
    {
        mInitialized = true;
        mNewest = sequence;
        mReceived = 1;
        return SequenceStatus::Newest;
    }
    const uint32 forward = sequence - mNewest;
    if (forward == 0)
    {
        return SequenceStatus::Duplicate;
    }
    if (forward == 0x80000000U)
    {
        return SequenceStatus::Ambiguous;
    }
    if (forward < 0x80000000U)
    {
        mReceived = forward >= 64 ? 1 : (mReceived << forward) | 1;
        mNewest = sequence;
        return SequenceStatus::Newest;
    }
    const uint32 age = mNewest - sequence;
    if (age >= 64)
    {
        return SequenceStatus::TooOld;
    }
    const uint64 bit = uint64{1} << age;
    if ((mReceived & bit) != 0)
    {
        return SequenceStatus::Duplicate;
    }
    mReceived |= bit;
    return SequenceStatus::OutOfOrder;
}
void SequenceWindow::Reset() noexcept
{
    mReceived = 0;
    mNewest = 0;
    mInitialized = false;
}
} // namespace ludus::network
