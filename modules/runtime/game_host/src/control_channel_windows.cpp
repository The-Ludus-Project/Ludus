#include "internal/control_channel.h"

#include <windows.h>

// Windows sessions run without a supervisor transport until the hosted play channel is
// ported (D6c/D6d). HostSession never calls these with a descriptor there: Run refuses
// a control descriptor, and sessions constructed without one skip the channel.
namespace ludus::runtime::game_host::control_channel
{
using foundation::int32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;
bool IsSupported() noexcept
{
    return false;
}
TransferStatus Send(int32 channel, const uint8* bytes, usize size, usize& outCount) noexcept
{
    static_cast<void>(channel);
    static_cast<void>(bytes);
    static_cast<void>(size);
    outCount = 0;
    return TransferStatus::Failed;
}
TransferStatus Receive(int32 channel, uint8* bytes, usize size, usize& outCount) noexcept
{
    static_cast<void>(channel);
    static_cast<void>(bytes);
    static_cast<void>(size);
    outCount = 0;
    return TransferStatus::Closed;
}
// The channel and timeout are distinct roles of one fixed signature.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void WaitWritable(int32 channel, int32 timeoutMilliseconds) noexcept
{
    static_cast<void>(channel);
    static_cast<void>(timeoutMilliseconds);
}
uint64 GetCurrentProcessIdentifier() noexcept
{
    return static_cast<uint64>(::GetCurrentProcessId());
}
} // namespace ludus::runtime::game_host::control_channel
