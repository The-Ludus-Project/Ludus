#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/input/keyboard.h>
#include <ludus/platform/base/window.h>
#include <ludus/runtime/game_api/services.h>
#include <ludus/runtime/game_host/host.h>

namespace ludus::runtime::game_host
{
// Owns the native surface. Shutdown precedes window destruction on every path.
// The same presenter drives dynamic editor sessions and static shipping games.
class FramePresenter final
{
public:
    ~FramePresenter() noexcept;
    [[nodiscard]] RunResult Start(Presentation mode) noexcept;
    [[nodiscard]] bool Poll() noexcept;
    [[nodiscard]] RunResult Present(const game_api::RenderParams& render) noexcept;
    void FillInput(game_api::FrameInput& frame) const noexcept;
    void ResetInput() noexcept;
    [[nodiscard]] bool ReplaceClearConfiguration(game_api::ByteView artifact,
                                                 ludus::foundation::uint64 digest) noexcept;
    [[nodiscard]] ludus::foundation::uint64 ClearConfigurationGeneration() const noexcept
    {
        return ClearDigest_;
    }
    [[nodiscard]] ludus::foundation::uint64 PresentedFrames() const noexcept
    {
        return PresentedFrames_;
    }
    [[nodiscard]] bool Windowed() const noexcept
    {
        return Rendering_;
    }
    /// Whether the window can be seen as of the latest Poll(); a headless presenter is always visible.
    /// The host lowers its pacing rate while this is false (high-resolution-time.md, "Native frame pacing").
    [[nodiscard]] bool IsWindowVisible() const noexcept
    {
        return Window_.Get() == nullptr || Window_->GetVisibility() == platform::WindowVisibility::Visible;
    }

private:
    ludus::input::InputSystem Input_;
    ludus::foundation::uint64 InputStep_ = 0;
    ludus::foundation::core::UniquePtr<ludus::platform::Window> Window_;
    bool Rendering_ = false;
    game_api::RenderParams ClearConfiguration_ = {};
    ludus::foundation::uint64 ClearDigest_ = 0;
    ludus::foundation::uint64 PresentedFrames_ = 0;
};
} // namespace ludus::runtime::game_host
