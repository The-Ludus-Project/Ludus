// Packaged, exception-free acceptance executable using the SAME production
// game.cpp/static-dispatch path as the shipping player, not a model handler.
#include <ludus/foundation/base/core.h>

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/runtime/behavior/behavior.h>
#include <ludus/runtime/game_api/api.h>

#include <span>

#include "contract.h"

namespace
{
using namespace ludus::foundation;
namespace game = ludus::runtime::game_api;
struct Owner
{
    game::GameApiTable* Table;
    game::GameInstance* Instance = nullptr;
    ~Owner() noexcept
    {
        if (Instance != nullptr)
        {
            Table->Destroy(Instance);
        }
    }
};
bool Checkpoint(game::GameApiTable& table,
                game::GameInstance* instance,
                uint8* bytes,
                game::CheckpointHeader& header,
                usize& written) noexcept
{
    usize capacity = 0;
    return table.CheckpointSize(instance, &capacity) == game::Status::Ok && capacity <= 256 &&
           table.WriteCheckpoint(instance, &header, {bytes, 256}, &written) == game::Status::Ok && written == capacity;
}
} // namespace
int main()
{
    game::GameApiTable table{ .StructSize = sizeof(game::GameApiTable) };
    if (LudusGetGameApi(game::kAbiMajor, game::kAbiMinor, &table) != game::Status::Ok ||
        table.ProcessScriptDebug != nullptr)
    {
        return 1;
    }
    game::GameMetadata metadata{ .StructSize = sizeof(game::GameMetadata) };
    if (table.Query(&metadata) != game::Status::Ok ||
        (metadata.Capabilities & static_cast<uint32>(game::Capability::ScriptDebug)) != 0)
    {
        return 2;
    }
    game::CreateInfo info
    {
        .StructSize = sizeof(game::CreateInfo),
        .ProjectId = 2,
        .GameId = 3,
        .ModuleGeneration = 4,
        .AuthoredDocument = {},
    };
    Owner active{&table};
    if (table.Create(&info, &active.Instance) != game::Status::Ok)
    {
        return 3;
    }
    game::FrameInput input;
    game::RenderParams output;
    for (uint32 interaction = 0; interaction < 2; ++interaction)
    {
        input.HeldActions = 0;
        if (table.Update(active.Instance, &input, &output) != game::Status::Ok)
        {
            return 4;
        }
        input.HeldActions = 4;
        if (table.Update(active.Instance, &input, &output) != game::Status::Ok)
        {
            return 5;
        }
    }
    if (output.ClearGreen != 0.6F || output.ClearRed != 0)
    {
        return 6;
    }
    uint8 bytes[256] = {};
    usize written = 0;
    game::CheckpointHeader header;
    if (Checkpoint(table, active.Instance, bytes, header, written) == false || written < 32)
    {
        return 7;
    }
    uint64 tick = 0, applied = 0;
    const auto view = std::span<const uint8>(bytes, written);
    ludus::runtime::behavior::Record record;
    ludus::sample::State state;
    if (TryReadLittleEndian(view, tick) == false || TryReadLittleEndian(view.subspan(8), applied) == false ||
        tick != 2 || applied != 1 ||
        ludus::runtime::behavior::DecodeState(ludus::sample::SCHEMA, view.subspan(32), record) == false ||
        ludus::sample::TryReadState(record, state) == false || state.Interactions != 2 || state.OpenRequested == false)
    {
        return 8;
    }
    if (table.Quiesce(active.Instance) != game::Status::Ok)
    {
        return 9;
    }
    game::GameCandidate* candidate = nullptr;
    ++info.ModuleGeneration;
    if (table.CreateCandidate(&info, &header, {bytes, written}, &candidate) != game::Status::Ok)
    {
        return 10;
    }
    if (table.ValidateCandidate(candidate) != game::Status::Ok)
    {
        table.DiscardCandidate(candidate);
        return 11;
    }
    Owner replacement{&table};
    if (table.CommitCandidate(candidate, &replacement.Instance) != game::Status::Ok)
    {
        table.DiscardCandidate(candidate);
        return 12;
    }
    if (table.Resume(replacement.Instance) != game::Status::Ok ||
        Checkpoint(table, replacement.Instance, bytes, header, written) == false)
    {
        return 13;
    }
    if (ludus::runtime::behavior::DecodeState(ludus::sample::SCHEMA,
                                              std::span<const uint8>(bytes, written).subspan(32),
                                              record) == false ||
        ludus::sample::TryReadState(record, state) == false || state.Interactions != 2 || state.OpenRequested == false)
    {
        return 14;
    }
    return 0;
}
