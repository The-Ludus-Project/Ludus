#include "internal/qt_viewport_device.h"

#include <ludus/foundation/containers/static_array.hpp>

#include <cstring>
#include <new>

#include <QQuickGraphicsDevice>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QVulkanFunctions>
#include <QVulkanInstance>
#include <QtQuick/qsgtexture_platform.h>

namespace ludus::editor
{
namespace
{
/// Bounded enumeration storage; larger native counts are rejected, never truncated.
constexpr foundation::uint32 MAX_PHYSICAL_DEVICES = 16;
constexpr foundation::uint32 MAX_QUEUE_FAMILIES = 32;
constexpr foundation::uint32 MAX_DEVICE_EXTENSIONS = 512;
/// Swapchain, external memory and its file descriptors for a hosted game's images, and
/// calibrated timestamps for profiling.
constexpr foundation::usize MAX_ENABLED_EXTENSIONS = 4;
/// The single queue both Qt Quick and the RHI submit to, serialized on the GUI thread.
constexpr foundation::uint32 SHARED_QUEUE_INDEX = 0;
struct Candidate final
{
    VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
    foundation::uint32 QueueFamily = 0;
    foundation::uint32 Rank = 0;
};
/// Prefers hardware adapters; software rasterizers remain usable when nothing else presents.
[[nodiscard]] foundation::uint32 RankDeviceType(VkPhysicalDeviceType type) noexcept
{
    switch (type)
    {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return 4;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return 3;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return 2;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            return 1;
        default:
            return 0;
    }
}
[[nodiscard]] bool
HasDeviceExtension(QVulkanFunctions& inoutFunctions, VkPhysicalDevice physicalDevice, const char* name) noexcept
{
    foundation::uint32 count = 0;
    if (inoutFunctions.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr) != VK_SUCCESS ||
        count > MAX_DEVICE_EXTENSIONS)
    {
        return false;
    }
    foundation::StaticArray<VkExtensionProperties, MAX_DEVICE_EXTENSIONS> extensions{};
    if (inoutFunctions.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, extensions.GetData()) !=
        VK_SUCCESS)
    {
        return false;
    }
    for (foundation::uint32 i = 0; i < count; ++i)
    {
        if (std::strcmp(extensions[i].extensionName, name) == 0)
        {
            return true;
        }
    }
    return false;
}
} // namespace
// Qt before 6.6 cannot report which queue its own Vulkan device uses, but every
// Qt 6 release can adopt a device it is given (QQuickGraphicsDevice::fromDeviceObjects).
// The editor therefore creates the device on Qt's instance, on a graphics queue that
// can present to the window, and hands the same device to Qt Quick and the RHI.
struct QtViewportDevice::State final
{
    QVulkanInstance Instance;
    VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
    VkDevice Device = VK_NULL_HANDLE;
    VkQueue Queue = VK_NULL_HANDLE;
    foundation::uint32 QueueFamily = 0;
    /// `VK_KHR_external_memory_fd` is enabled, so hosted game images can be imported.
    bool IsHostedImportEnabled = false;
    /// Calibrated-timestamp extension enabled for the RHI's profiling clock mapping.
    graphics::rhi::NativeCalibratedTimestamps CalibratedTimestamps = graphics::rhi::NativeCalibratedTimestamps::None;
    ~State()
    {
        if (Device != VK_NULL_HANDLE)
        {
            // Qt Quick and the RHI have released the device: the window is destroyed and
            // the RHI lease is returned before the adapter (see PreviewViewport::destroyWindow).
            QVulkanDeviceFunctions* functions = Instance.deviceFunctions(Device);
            static_cast<void>(functions->vkDeviceWaitIdle(Device));
            functions->vkDestroyDevice(Device, nullptr);
            Instance.resetDeviceFunctions(Device);
        }
    }
    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;
};
QtViewportDevice::QtViewportDevice() noexcept = default;
QtViewportDevice::~QtViewportDevice() noexcept = default;
bool QtViewportDevice::PrepareWindow(QQuickWindow& inoutWindow)
{
    mState.Reset(new (std::nothrow) State);
    if (mState.Get() == nullptr)
    {
        return false;
    }
    mState->Instance.setApiVersion(QVersionNumber(1, 1));
    if (mState->Instance.create() == false)
    {
        return false;
    }
    inoutWindow.setVulkanInstance(&mState->Instance);
    // Presentation support is a property of the native surface, so create the
    // platform window now. The scene graph (and Qt's device choice) starts later,
    // on first expose, after setGraphicsDevice below.
    inoutWindow.create();
    if (inoutWindow.handle() == nullptr)
    {
        return false;
    }
    QVulkanFunctions* functions = mState->Instance.functions();
    const VkInstance instance = mState->Instance.vkInstance();
    foundation::uint32 count = 0;
    if (functions->vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0 ||
        count > MAX_PHYSICAL_DEVICES)
    {
        return false;
    }
    foundation::StaticArray<VkPhysicalDevice, MAX_PHYSICAL_DEVICES> physicalDevices{};
    if (functions->vkEnumeratePhysicalDevices(instance, &count, physicalDevices.GetData()) != VK_SUCCESS)
    {
        return false;
    }
    Candidate best;
    for (foundation::uint32 i = 0; i < count; ++i)
    {
        const VkPhysicalDevice physicalDevice = physicalDevices[i];
        VkPhysicalDeviceProperties properties{};
        functions->vkGetPhysicalDeviceProperties(physicalDevice, &properties);
        const foundation::uint32 rank = RankDeviceType(properties.deviceType);
        if (properties.apiVersion < VK_API_VERSION_1_1 || rank <= best.Rank ||
            HasDeviceExtension(*functions, physicalDevice, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == false)
        {
            continue;
        }
        foundation::uint32 familyCount = 0;
        functions->vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
        if (familyCount > MAX_QUEUE_FAMILIES)
        {
            continue;
        }
        foundation::StaticArray<VkQueueFamilyProperties, MAX_QUEUE_FAMILIES> families{};
        functions->vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.GetData());
        for (foundation::uint32 family = 0; family < familyCount; ++family)
        {
            if (families[family].queueCount != 0 && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
                mState->Instance.supportsPresent(physicalDevice, family, &inoutWindow) == true)
            {
                best = { .PhysicalDevice = physicalDevice, .QueueFamily = family, .Rank = rank };
                break;
            }
        }
    }
    if (best.PhysicalDevice == VK_NULL_HANDLE)
    {
        return false;
    }
    // Enable what the adapter supports so neither Qt Quick nor the RHI meets a
    // feature it assumes but the device lacks; bounds checking is a debugging aid.
    VkPhysicalDeviceFeatures features{};
    functions->vkGetPhysicalDeviceFeatures(best.PhysicalDevice, &features);
    features.robustBufferAccess = VK_FALSE;
    const foundation::float32 priority = 1;
    const VkDeviceQueueCreateInfo queue
    {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueFamilyIndex = best.QueueFamily,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    // Play in the viewport imports a hosted game's opaque-fd memory onto this device
    // (HostedSurfaceConsumer). External memory itself is core in Vulkan 1.1; its
    // extension is enabled too where the adapter still lists it. An adapter without
    // the fd extension keeps the viewport, and Play there says why it is unavailable.
    foundation::StaticArray<const char*, MAX_ENABLED_EXTENSIONS> extensions{};
    foundation::uint32 extensionCount = 0;
    extensions[extensionCount++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    const bool hasHostedImport =
        HasDeviceExtension(*functions, best.PhysicalDevice, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    if (hasHostedImport == true)
    {
        if (HasDeviceExtension(*functions, best.PhysicalDevice, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) == true)
        {
            extensions[extensionCount++] = VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME;
        }
        extensions[extensionCount++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
    }
    // Optional: lets the RHI place profiled GPU passes on the CPU trace clock. The RHI
    // still checks the time domains, so an adapter without them only stays duration-only.
    graphics::rhi::NativeCalibratedTimestamps calibratedTimestamps = graphics::rhi::NativeCalibratedTimestamps::None;
#if defined(VK_KHR_calibrated_timestamps)
    // Qt compiles against the system headers, which predate the KHR promotion on older distributions.
    if (HasDeviceExtension(*functions, best.PhysicalDevice, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) == true)
    {
        extensions[extensionCount++] = VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
        calibratedTimestamps = graphics::rhi::NativeCalibratedTimestamps::Khr;
    }
#endif
    if (calibratedTimestamps == graphics::rhi::NativeCalibratedTimestamps::None &&
        HasDeviceExtension(*functions, best.PhysicalDevice, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) == true)
    {
        extensions[extensionCount++] = VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
        calibratedTimestamps = graphics::rhi::NativeCalibratedTimestamps::Ext;
    }
    const VkDeviceCreateInfo description
    {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = extensionCount,
        .ppEnabledExtensionNames = extensions.GetData(),
        .pEnabledFeatures = &features,
    };
    if (functions->vkCreateDevice(best.PhysicalDevice, &description, nullptr, &mState->Device) != VK_SUCCESS)
    {
        mState->Device = VK_NULL_HANDLE;
        return false;
    }
    mState->PhysicalDevice = best.PhysicalDevice;
    mState->QueueFamily = best.QueueFamily;
    mState->IsHostedImportEnabled = hasHostedImport;
    mState->CalibratedTimestamps = calibratedTimestamps;
    mState->Instance.deviceFunctions(mState->Device)
        ->vkGetDeviceQueue(mState->Device, best.QueueFamily, SHARED_QUEUE_INDEX, &mState->Queue);
    inoutWindow.setGraphicsDevice(
        QQuickGraphicsDevice::fromDeviceObjects(mState->PhysicalDevice,
                                                mState->Device,
                                                static_cast<foundation::int32>(best.QueueFamily),
                                                static_cast<foundation::int32>(SHARED_QUEUE_INDEX)));
    return true;
}
bool QtViewportDevice::GetDescription(QQuickWindow& inoutWindow, graphics::rhi::NativeDeviceDescription& outDescription)
{
    QSGRendererInterface* renderer = inoutWindow.rendererInterface();
    if (mState.Get() == nullptr || mState->Device == VK_NULL_HANDLE ||
        renderer->graphicsApi() != QSGRendererInterface::Vulkan || inoutWindow.vulkanInstance() != &mState->Instance)
    {
        return false;
    }
    const QVersionNumber version = mState->Instance.apiVersion();
    outDescription.Identity =
    {
        .Api = graphics::rhi::Backend::Vulkan,
        .Instance = graphics::rhi::NativeInstance{reinterpret_cast<void*>(mState->Instance.vkInstance())},
        .PhysicalDevice = graphics::rhi::NativePhysicalDevice{reinterpret_cast<void*>(mState->PhysicalDevice)},
        .Device = graphics::rhi::NativeLogicalDevice{reinterpret_cast<void*>(mState->Device)},
        .CommandQueue = graphics::rhi::NativeCommandQueue{reinterpret_cast<void*>(mState->Queue)},
        .QueueFamily = mState->QueueFamily,
        .QueueIndex = SHARED_QUEUE_INDEX,
    };
    outDescription.InstanceApiVersion = VK_MAKE_API_VERSION(0,
                                                            static_cast<foundation::uint32>(version.majorVersion()),
                                                            static_cast<foundation::uint32>(version.minorVersion()),
                                                            static_cast<foundation::uint32>(version.microVersion()));
    outDescription.SerializedOwnerQueue = true;
    outDescription.CalibratedTimestamps = mState->CalibratedTimestamps;
    // IsCompatible confirms Qt Quick really adopted this device and queue.
    return IsCompatible(inoutWindow, outDescription.Identity);
}
bool QtViewportDevice::IsCompatible(QQuickWindow& inoutWindow,
                                    const graphics::rhi::NativeDeviceIdentity& device) const noexcept
{
    QSGRendererInterface* renderer = inoutWindow.rendererInterface();
    if (renderer->graphicsApi() != QSGRendererInterface::Vulkan || inoutWindow.vulkanInstance() == nullptr)
    {
        return false;
    }
    const VkDevice* actualDevice =
        static_cast<const VkDevice*>(renderer->getResource(&inoutWindow, QSGRendererInterface::DeviceResource));
    const VkQueue* actualQueue =
        static_cast<const VkQueue*>(renderer->getResource(&inoutWindow, QSGRendererInterface::CommandQueueResource));
    const VkPhysicalDevice* actualPhysical = static_cast<const VkPhysicalDevice*>(
        renderer->getResource(&inoutWindow, QSGRendererInterface::PhysicalDeviceResource));
    // Queue provenance comes from fromDeviceObjects in PrepareWindow, not a guessed family.
    return actualDevice != nullptr && actualQueue != nullptr && actualPhysical != nullptr &&
           reinterpret_cast<void*>(*actualDevice) == device.Device.GetObject() &&
           reinterpret_cast<void*>(*actualQueue) == device.CommandQueue.GetObject() &&
           reinterpret_cast<void*>(*actualPhysical) == device.PhysicalDevice.GetObject() &&
           reinterpret_cast<void*>(inoutWindow.vulkanInstance()->vkInstance()) == device.Instance.GetObject();
}
QSGTexture* QtViewportDevice::WrapTexture(QQuickWindow& inoutWindow, const graphics::rhi::NativeTextureInfo& image)
{
    if (IsCompatible(inoutWindow, image.Device.Identity) == false)
    {
        return nullptr;
    }
#if VK_USE_64_BIT_PTR_DEFINES
    // Reconstruct Vulkan's opaque handle from the RHI's borrowed native bits.
    // Qt forwards this identity to Vulkan; neither consumer dereferences it.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const VkImage nativeImage = reinterpret_cast<VkImage>(image.VulkanImage.GetBits());
#else
    const VkImage nativeImage = image.VulkanImage.GetBits();
#endif
    return QNativeInterface::QSGVulkanTexture::fromNative(
        nativeImage,
        static_cast<VkImageLayout>(image.VulkanLayout),
        &inoutWindow,
        QSize(static_cast<foundation::int32>(image.Width), static_cast<foundation::int32>(image.Height)));
}
bool QtViewportDevice::CanImportHostedImages() const noexcept
{
    return mState.Get() != nullptr && mState->IsHostedImportEnabled == true;
}
} // namespace ludus::editor
