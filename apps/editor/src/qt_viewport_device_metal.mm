#include "internal/qt_viewport_device.h"

#include <QQuickGraphicsDevice>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QtQuick/qsgtexture_platform.h>

#import <Metal/Metal.h>

namespace ludus::editor
{
struct QtViewportDevice::State final
{
};
QtViewportDevice::QtViewportDevice() noexcept = default;
QtViewportDevice::~QtViewportDevice() noexcept = default;
bool QtViewportDevice::PrepareWindow(QQuickWindow&)
{
    return true;
}
bool QtViewportDevice::GetDescription(QQuickWindow& inoutWindow, graphics::rhi::NativeDeviceDescription& outDescription)
{
    QSGRendererInterface* renderer = inoutWindow.rendererInterface();
    if (renderer->graphicsApi() != QSGRendererInterface::Metal)
    {
        return false;
    }
    void* device = renderer->getResource(&inoutWindow, QSGRendererInterface::DeviceResource);
    void* queue = renderer->getResource(&inoutWindow, QSGRendererInterface::CommandQueueResource);
    if (device == nullptr || queue == nullptr)
    {
        return false;
    }
    outDescription.Identity =
    {
        .Api = graphics::rhi::Backend::Metal,
        .Device = graphics::rhi::NativeLogicalDevice{device},
        .CommandQueue = graphics::rhi::NativeCommandQueue{queue},
    };
    outDescription.SerializedOwnerQueue = true;
    return true;
}
bool QtViewportDevice::IsCompatible(QQuickWindow& inoutWindow,
                                    const graphics::rhi::NativeDeviceIdentity& device) const noexcept
{
    QSGRendererInterface* renderer = inoutWindow.rendererInterface();
    if (renderer->graphicsApi() != QSGRendererInterface::Metal)
    {
        return false;
    }
    void* actualDevice = renderer->getResource(&inoutWindow, QSGRendererInterface::DeviceResource);
    void* actualQueue = renderer->getResource(&inoutWindow, QSGRendererInterface::CommandQueueResource);
    return actualDevice != nullptr && actualQueue != nullptr && actualDevice == device.Device.GetObject() &&
           actualQueue == device.CommandQueue.GetObject();
}
QSGTexture* QtViewportDevice::WrapTexture(QQuickWindow& inoutWindow, const graphics::rhi::NativeTextureInfo& image)
{
    if (IsCompatible(inoutWindow, image.Device.Identity) == false)
    {
        return nullptr;
    }
    return QNativeInterface::QSGMetalTexture::fromNative(
        (__bridge id<MTLTexture>)image.MetalTexture.GetObject(),
        &inoutWindow,
        QSize(static_cast<foundation::int32>(image.Width), static_cast<foundation::int32>(image.Height)));
}
// IOSurface textures need no queue family transfer, so the hosted import has no extension to enable.
bool QtViewportDevice::CanImportHostedImages() const noexcept
{
    return true;
}
} // namespace ludus::editor
