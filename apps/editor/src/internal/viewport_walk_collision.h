#pragma once

#include <walking_controller.h>

namespace ludus::editor
{
struct RenderScene;

/// Extract authored Static Collider instances and start a walk. A rejected candidate preserves the current walk.
[[nodiscard]] first_person::WalkingController::StartStatus
StartWorldWalk(const RenderScene& scene,
               foundation::math::Vector3d eyeWorld,
               first_person::WalkingController& inoutWalk) noexcept;
} // namespace ludus::editor
