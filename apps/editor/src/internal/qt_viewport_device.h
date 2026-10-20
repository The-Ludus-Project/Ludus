#pragma once

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/graphics/rhi/native_interop.h>

class QQuickWindow;
class QSGTexture;

namespace ludus::editor
{
// Native framework conversions remain in this private, platform-specific adapter.
class QtViewportDevice final
{
public:
    QtViewportDevice() noexcept;
    ~QtViewportDevice() noexcept;
    [[nodiscard]] bool PrepareWindow(QQuickWindow& inoutWindow);
    [[nodiscard]] bool GetDescription(QQuickWindow& inoutWindow,
                                      graphics::rhi::NativeDeviceDescription& outDescription);
    [[nodiscard]] QSGTexture* WrapTexture(QQuickWindow& inoutWindow, const graphics::rhi::NativeTextureInfo& image);
    [[nodiscard]] bool IsCompatible(QQuickWindow& inoutWindow,
                                    const graphics::rhi::NativeDeviceIdentity& device) const noexcept;
    /// Whether a hosted game's images can be imported onto this device: always on Metal; on
    /// Vulkan when PrepareWindow() enabled `VK_KHR_external_memory_fd` on the adapter.
    [[nodiscard]] bool CanImportHostedImages() const noexcept;

private:
    struct State;
    foundation::UniquePtr<State> mState;
};
} // namespace ludus::editor
