#include <ludus/runtime/behavior/behavior.h>

#include "contract.h"
#include "package.h"

namespace
{
using namespace ludus::runtime::behavior;
bool Alive(void*, EntityRef ref) noexcept
{
    return ref.Slot == 0 && ref.Generation == 1;
}
} // namespace
int main()
{
    using namespace ludus::runtime::behavior;
    LuauProvider provider;
    if (provider.Load(ludus::sample::SCHEMA, ludus::sample::PACKAGE) != Status::Completed)
    {
        return 1;
    }
    Invocation input
    {
        .Asset = 0x100,
        .Revision = 1,
        .Instance = 1,
        .World = 2,
        .Session = 3,
        .Execution = 4,
        .Tick = 1,
        .Phase = 3,
        .Capabilities = 1,
    };
    input.Config = ludus::sample::MakeConfig({});
    input.State = ludus::sample::MakeState({});
    input.Event = ludus::sample::MakeEvent({ .Target = {2, 3, 4, 0, 1}, .Amount = 1 });
    Outcome output;
    Diagnostic diagnostic;
    for (ludus::foundation::uint32 tick = 1; tick <= 2; ++tick)
    {
        input.Tick = tick;
        if (provider.Invoke(input, {nullptr, Alive}, output, diagnostic) != Status::Completed)
        {
            return 2;
        }
        input.State = output.State;
    }
    if (output.Count != 1 || output.Commands[0].Operation != 200 || output.State.Items[1].Data.Scalar != 1)
    {
        return 3;
    }
    input.Asset = 0x800;
    input.Capabilities = 3;
    if (provider.Invoke(input, {nullptr, Alive}, output, diagnostic) != Status::Completed || output.Count != 16 ||
        diagnostic.NativeStatus != static_cast<ludus::foundation::uint32>(CommandStatus::CapacityExceeded))
    {
        return 4;
    }
    input.Asset = 0x300;
    output.Count = 9;
    if (provider.Invoke(input, {nullptr, Alive}, output, diagnostic) != Status::ScriptFault || output.Count != 9 ||
        provider.LiveBytes() != 0)
    {
        return 5;
    }
    if (provider.Load(ludus::sample::SCHEMA, ludus::sample::PACKAGE) != Status::Completed)
    {
        return 6;
    }
    input.Asset = 0x400;
    if (provider.Invoke(input, {nullptr, Alive}, output, diagnostic) != Status::Completed)
    {
        return 7;
    }
    output.Count = 9;
    if (provider.Invoke(input, {nullptr, Alive}, output, diagnostic) != Status::ScriptFault || output.Count != 9 ||
        provider.LiveBytes() != 0)
    {
        return 8;
    }
    if (provider.Load(ludus::sample::SCHEMA, ludus::sample::PACKAGE, ludus::foundation::usize{8} * 1024 * 1024, 20) !=
        Status::Completed)
    {
        return 9;
    }
    input.Asset = 0x500;
    if (provider.Invoke(input, {nullptr, Alive}, output, diagnostic) != Status::Interrupted || output.Count != 9 ||
        provider.LiveBytes() != 0)
    {
        return 10;
    }
    if (provider.Load(ludus::sample::SCHEMA, ludus::sample::PACKAGE) != Status::Completed)
    {
        return 12;
    }
    input.Asset = 0x100;
    input.State = ludus::sample::MakeState({});
    const auto line = provider.Breakpoint(input.Asset, 1);
    if (line <= 0 || provider.BeginDebug(input, {nullptr, Alive}, output, diagnostic) != Status::Paused ||
        output.Count != 9)
    {
        return 13;
    }
    DebugSnapshot stop;
    if (provider.Inspect(stop) == false || stop.Paused == false || stop.Stop == 0 || stop.FrameCount == 0 ||
        provider.ResumeDebug(stop.Stop + 1, DebugMode::Continue, output, diagnostic) != Status::InvalidInput)
    {
        return 14;
    }
    if (provider.Breakpoint(input.Asset, line, false) <= 0 ||
        provider.ResumeDebug(stop.Stop, DebugMode::Continue, output, diagnostic) != Status::Completed ||
        output.State.Items[0].Data.Scalar != 1)
    {
        return 15;
    }
    return provider.Close() == Status::Completed && provider.LiveBytes() == 0 ? 0 : 11;
}
