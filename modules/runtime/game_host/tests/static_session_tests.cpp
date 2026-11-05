#include <ludus/foundation/base/types.h>
#include <ludus/runtime/game_api/api.h>
#include <ludus/runtime/game_host/host.h>

#include <catch2/catch_test_macros.hpp>

namespace ludus::runtime::game_api
{
struct GameInstance final
{
    foundation::uint64 Updates = 0;
    bool IsQuiesced = false;
    bool IsDestroyed = false;
};
} // namespace ludus::runtime::game_api

namespace
{
namespace game = ludus::runtime::game_api;
namespace host = ludus::runtime::game_host;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
constexpr uint64 FRAME_COUNT = 3;
game::GameInstance gGame;

game::Status Create(const game::CreateInfo* information, game::GameInstance** outInstance) noexcept
{
    if (information->Services == nullptr)
    {
        return game::Status::InvalidArgument;
    }
    gGame = {};
    *outInstance = &gGame;
    return game::Status::Ok;
}

game::Status Update(game::GameInstance* instance, const game::FrameInput* input, game::RenderParams* outRender) noexcept
{
    if (input->FrameIndex != instance->Updates)
    {
        return game::Status::InvalidArgument;
    }
    *outRender = {};
    ++instance->Updates;
    return game::Status::Ok;
}

game::Status Quiesce(game::GameInstance* instance) noexcept
{
    instance->IsQuiesced = true;
    return game::Status::Ok;
}

void Destroy(game::GameInstance* instance) noexcept
{
    instance->IsDestroyed = true;
}

game::Status GetApi(uint32, uint32, game::GameApiTable* outTable) noexcept
{
    outTable->Create = Create;
    outTable->Update = Update;
    outTable->Quiesce = Quiesce;
    outTable->Destroy = Destroy;
    return game::Status::Ok;
}
} // namespace

TEST_CASE("A standalone static game runs and retires through GameHost", "[game-host]")
{
    host::HostConfig config;
    config.Source = host::GameplaySource::Static;
    config.Mode = host::Presentation::Headless;
    config.MaxFrames = FRAME_COUNT;

    REQUIRE(host::RunStatic(config, GetApi) == host::RunResult::Ok);
    CHECK(gGame.Updates == FRAME_COUNT);
    CHECK(gGame.IsQuiesced == true);
    CHECK(gGame.IsDestroyed == true);
}
