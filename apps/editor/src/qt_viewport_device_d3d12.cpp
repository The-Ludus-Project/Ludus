#include "internal/qt_viewport_device.h"

#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QtQuick/qsgtexture_platform.h>

#include <d3d12.h>

#undef GetObject

namespace ludus::editor
{
namespace
{
namespace rhi = graphics::rhi;

// Thanks to The Qt Company, Qt Quick QSGRendererInterface and QSGD3D12Texture
// documentation, for the device/queue queries and non-owning texture wrapper.
// https://doc.qt.io/qt-6/qsgrendererinterface.html
// https://doc.qt.io/qt-6/qnativeinterface-qsgd3d12texture.html
[[nodiscard]] bool ReadContext(QQuickWindow& inoutWindow, rhi::NativeDeviceIdentity& outIdentity)
{
    QSGRendererInterface* renderer = inoutWindow.rendererInterface();
    if (renderer->graphicsApi() != QSGRendererInterface::Direct3D12)
    {
        return false;
    }
    void* deviceOrNull = renderer->getResource(&inoutWindow, QSGRendererInterface::DeviceResource);
    void* queueOrNull = renderer->getResource(&inoutWindow, QSGRendererInterface::CommandQueueResource);
    if (deviceOrNull == nullptr || queueOrNull == nullptr)
    {
        return false;
    }
    outIdentity =
    {
        .Api = rhi::Backend::Direct3D12,
        .Device = rhi::NativeLogicalDevice{deviceOrNull},
        .CommandQueue = rhi::NativeCommandQueue{queueOrNull},
    };
    return true;
}
} // namespace

struct QtViewportDevice::State final
{
};

QtViewportDevice::QtViewportDevice() noexcept = default;
QtViewportDevice::~QtViewportDevice() noexcept = default;

bool QtViewportDevice::PrepareWindow(QQuickWindow&)
{
    return true;
}

bool QtViewportDevice::GetDescription(QQuickWindow& inoutWindow, rhi::NativeDeviceDescription& outDescription)
{
    rhi::NativeDeviceIdentity identity;
    if (ReadContext(inoutWindow, identity) == false)
    {
        return false;
    }
    outDescription = { .Identity = identity, .SerializedOwnerQueue = true };
    return true;
}

bool QtViewportDevice::IsCompatible(QQuickWindow& inoutWindow, const rhi::NativeDeviceIdentity& device) const noexcept
{
    rhi::NativeDeviceIdentity actual;
    return ReadContext(inoutWindow, actual) == true && device.Api == actual.Api && device.Device == actual.Device &&
           device.CommandQueue == actual.CommandQueue;
}

QSGTexture* QtViewportDevice::WrapTexture(QQuickWindow& inoutWindow, const rhi::NativeTextureInfo& image)
{
    if (IsCompatible(inoutWindow, image.Device.Identity) == false || image.Direct3D12Resource.IsNull() == true)
    {
        return nullptr;
    }
    return QNativeInterface::QSGD3D12Texture::fromNative(
        image.Direct3D12Resource.GetObject(),
        image.IsDirect3D12ResourceInCommonState == true ? D3D12_RESOURCE_STATE_COMMON
                                                        : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &inoutWindow,
        QSize(static_cast<foundation::int32>(image.Width), static_cast<foundation::int32>(image.Height)));
}

bool QtViewportDevice::CanImportHostedImages() const noexcept
{
    return true;
}
} // namespace ludus::editor
