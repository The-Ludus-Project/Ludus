#include <ludus/platform/hosted_surface.h>

#if defined(LUDUS_PLATFORM_MACOS)
#    include <mach/mach.h>
#endif
#if defined(LUDUS_PLATFORM_MACOS) || defined(LUDUS_PLATFORM_LINUX)
#    include <unistd.h>
#endif

namespace ludus::platform
{
HostedImageHandle::HostedImageHandle(HostedImageHandle&& other) noexcept
    : mKind(other.mKind), mMachPort(other.mMachPort), mFileDescriptor(other.mFileDescriptor)
{
    other.mKind = HostedHandleKind::None;
    other.mMachPort = 0;
    other.mFileDescriptor = -1;
}

HostedImageHandle& HostedImageHandle::operator=(HostedImageHandle&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        mKind = other.mKind;
        mMachPort = other.mMachPort;
        mFileDescriptor = other.mFileDescriptor;
        other.mKind = HostedHandleKind::None;
        other.mMachPort = 0;
        other.mFileDescriptor = -1;
    }
    return *this;
}

HostedImageHandle HostedImageHandle::FromMachPort(foundation::uint32 sendRight) noexcept
{
    HostedImageHandle handle;
    if (sendRight != 0)
    {
        handle.mKind = HostedHandleKind::MachPort;
        handle.mMachPort = sendRight;
    }
    return handle;
}

HostedImageHandle HostedImageHandle::FromFileDescriptor(foundation::int32 descriptor) noexcept
{
    HostedImageHandle handle;
    if (descriptor >= 0)
    {
        handle.mKind = HostedHandleKind::OpaqueFileDescriptor;
        handle.mFileDescriptor = descriptor;
    }
    return handle;
}

foundation::uint32 HostedImageHandle::ReleaseMachPort() noexcept
{
    if (mKind != HostedHandleKind::MachPort)
    {
        return 0;
    }
    const foundation::uint32 sendRight = mMachPort;
    mKind = HostedHandleKind::None;
    mMachPort = 0;
    return sendRight;
}

foundation::int32 HostedImageHandle::ReleaseFileDescriptor() noexcept
{
    if (mKind != HostedHandleKind::OpaqueFileDescriptor)
    {
        return -1;
    }
    const foundation::int32 descriptor = mFileDescriptor;
    mKind = HostedHandleKind::None;
    mFileDescriptor = -1;
    return descriptor;
}

void HostedImageHandle::Reset() noexcept
{
#if defined(LUDUS_PLATFORM_MACOS)
    if (mKind == HostedHandleKind::MachPort)
    {
        static_cast<void>(::mach_port_deallocate(::mach_task_self(), mMachPort));
    }
#endif
#if defined(LUDUS_PLATFORM_MACOS) || defined(LUDUS_PLATFORM_LINUX)
    if (mKind == HostedHandleKind::OpaqueFileDescriptor)
    {
        static_cast<void>(::close(mFileDescriptor));
    }
#endif
    mKind = HostedHandleKind::None;
    mMachPort = 0;
    mFileDescriptor = -1;
}
} // namespace ludus::platform
