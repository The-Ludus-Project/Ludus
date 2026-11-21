#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/rhi/rhi.h>

namespace ludus::graphics::rhi::backend
{
// Kind() reports the backend currently bound for resource/frame dispatch. On a
// single-backend build it is constant; on the browser Auto build it reflects the
// backend chosen (or being attempted) by the dispatcher, so the facade's
// per-backend shader validation picks the right artifact.
Backend Kind() noexcept;
// Backends a build can provide, for the facade to validate a forced selection.
bool Supports(BackendSelection selection) noexcept;
StartupError
Start(const ApplicationInfo&, const WindowInfo&, ludus::foundation::uint32 token, BackendSelection selection) noexcept;
void Shutdown() noexcept;
FrameStatus SetTarget(const FrameTarget&) noexcept;
FrameStatus Begin() noexcept;
FrameStatus End() noexcept;
} // namespace ludus::graphics::rhi::backend
