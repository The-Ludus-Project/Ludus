#include <ludus/platform/hosted_surface.h>

#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
#    include <windows.h>
#endif

#if LUDUS_TARGET_OS == LUDUS_OS_MACOS
#    include <mach/mach.h>
#endif
#if LUDUS_TARGET_OS == LUDUS_OS_MACOS || LUDUS_TARGET_OS == LUDUS_OS_LINUX
#    include <unistd.h>
#endif

namespace ludus::platform
{
HostedImageHandle::HostedImageHandle(HostedImageHandle&& other) noexcept
    : mKind(other.mKind), mMachPort(other.mMachPort), mFileDescriptor(other.mFileDescriptor),
      mWin32HandleOrNull(other.mWin32HandleOrNull)
{
    other.mKind = HostedHandleKind::None;
    other.mMachPort = 0;
    other.mFileDescriptor = -1;
    other.mWin32HandleOrNull = nullptr;
}

HostedImageHandle& HostedImageHandle::operator=(HostedImageHandle&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        mKind = other.mKind;
        mMachPort = other.mMachPort;
        mFileDescriptor = other.mFileDescriptor;
        mWin32HandleOrNull = other.mWin32HandleOrNull;
        other.mKind = HostedHandleKind::None;
        other.mMachPort = 0;
        other.mFileDescriptor = -1;
        other.mWin32HandleOrNull = nullptr;
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

HostedImageHandle HostedImageHandle::FromWin32Handle(void* handleOrNull) noexcept
{
    HostedImageHandle handle;
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    if (handleOrNull != nullptr && handleOrNull != INVALID_HANDLE_VALUE)
    {
        handle.mKind = HostedHandleKind::Win32SharedResource;
        handle.mWin32HandleOrNull = handleOrNull;
    }
#else
    static_cast<void>(handleOrNull);
#endif
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
#if LUDUS_TARGET_OS == LUDUS_OS_MACOS
    if (mKind == HostedHandleKind::MachPort)
    {
        static_cast<void>(::mach_port_deallocate(::mach_task_self(), mMachPort));
    }
#endif
#if LUDUS_TARGET_OS == LUDUS_OS_MACOS || LUDUS_TARGET_OS == LUDUS_OS_LINUX
    if (mKind == HostedHandleKind::OpaqueFileDescriptor)
    {
        static_cast<void>(::close(mFileDescriptor));
    }
#endif
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    if (mWin32HandleOrNull != nullptr)
    {
        static_cast<void>(::CloseHandle(mWin32HandleOrNull));
        mWin32HandleOrNull = nullptr;
    }
#endif
    mKind = HostedHandleKind::None;
    mMachPort = 0;
    mFileDescriptor = -1;
}
} // namespace ludus::platform
