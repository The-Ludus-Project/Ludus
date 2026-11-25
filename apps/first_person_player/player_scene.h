#pragma once

#include "playable_snapshot.h"
#include "walking_controller.h"
#include <ludus/graphics/renderer/render_graph.hpp>

namespace ludus::first_person
{
struct PlayerScene final
{
    foundation::Array<graphics::renderer::RenderGraphMesh> Meshes;
    foundation::Array<graphics::renderer::RenderGraphItem> Items;
    foundation::Array<graphics::renderer::SceneShaderMaterial> Materials;
    foundation::Array<graphics::renderer::DirectLight> Lights;
    foundation::Array<foundation::uint32> MaterialBindings;
    graphics::renderer::SceneShaderInputs Inputs;
    // Geometry and texture bytes borrow the immutable decoded snapshot.
    [[nodiscard]] bool Build(const PlayableSnapshot& snapshot) noexcept;
};

[[nodiscard]] WalkingController::StartStatus
StartWalking(const PlayableSnapshot& snapshot, const PlayerSpawn& spawn, WalkingController& outController) noexcept;
} // namespace ludus::first_person
