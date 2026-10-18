#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/graphics/renderer/viewport.hpp>

#include "internal/render_preview.h"

#include <QByteArray>
#include <QPointer>
#include <QTimer>
#include <QWidget>

namespace ludus::editor
{
/// Qt layout/input proxy for a separate engine-owned WebGPU canvas.
/// Qt continues to receive pointer/keyboard input through the transparent DOM overlay.
/// The preview/device is stopped before the canvas and its stable selector are removed.
class BrowserViewport final : public QWidget
{
public:
    BrowserViewport(RenderPreview& inoutPreview, QWidget* parentOrNull);
    ~BrowserViewport() override;
    [[nodiscard]] bool Start();
    void ShowFrame();
    void ClearFrame();
    [[nodiscard]] foundation::uint64 GetPresentedFrames() const noexcept;

private:
    void service();
    void releaseFrame() noexcept;
    QPointer<RenderPreview> mPreview;
    QByteArray mSelector;
    graphics::renderer::ViewportFrame mFrame;
    QTimer mService;
    foundation::uint64 mPresentedFrames = 0;
    foundation::int32 mRequestedWidth = 0;
    foundation::int32 mRequestedHeight = 0;
    bool mHasFrame = false;
    bool mHasPresentedImage = false;
    bool mCanPresent = false;
};
} // namespace ludus::editor
