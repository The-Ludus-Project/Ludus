#pragma once

#include "player_scene.h"

namespace ludus::first_person
{
class PlayerRenderer final
{
public:
    [[nodiscard]] bool Open(graphics::rhi::DeviceHandle device) noexcept;
    [[nodiscard]] bool Resize(foundation::uint32 width, foundation::uint32 height) noexcept;
    [[nodiscard]] bool Update(const PlayerScene& scene, const graphics::renderer::ViewDescription& view) noexcept;
    [[nodiscard]] graphics::rhi::RasterStatus Present(graphics::rhi::SurfaceHandle surface) noexcept;

    [[nodiscard]] bool
    ReadFixturePixel(foundation::uint32 x, foundation::uint32 y, foundation::uint32& outRgb) noexcept;
    [[nodiscard]] graphics::renderer::RenderGraphReport GetReport() const noexcept;

private:
    [[nodiscard]] bool preparePresentation() noexcept;
    graphics::rhi::DeviceHandle mDevice;
    graphics::renderer::RenderGraph mGraph;
    graphics::renderer::Renderer mCompositor;
    graphics::renderer::Presentation mPresentation;
    foundation::uint32 mColorResource = 0;
    foundation::uint64 mViewRevision = 0;
    foundation::uint32 mWidth = 0;
    foundation::uint32 mHeight = 0;
    bool mShouldPreparePresentation = false;
    bool mHasPresentation = false;
};
} // namespace ludus::first_person
