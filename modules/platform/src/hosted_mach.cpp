#include "internal/hosted_mach.hpp"

#include <cstring>

#include <bsm/libbsm.h>
#include <mach/mach.h>
#include <servers/bootstrap.h>

#if LUDUS_TARGET_OS != LUDUS_OS_MACOS
#    error "The hosted surface Mach rendezvous requires macOS"
#endif

// Thanks to Apple for the IOSurface reference, "IOSurfaceCreateMachPort" and
// "IOSurfaceLookupFromMachPort" (https://developer.apple.com/documentation/iosurface):
// a Mach port that holds a surface reference passes the surface to another task
// "without making the surface global to the entire system", and kIOSurfaceIsGlobal
// is deprecated because global surfaces are insecure. The rendezvous below only
// moves those send rights; it never creates or looks up a surface by ID.
namespace ludus::platform::hosted
{
namespace
{
constexpr std::string_view HEX_DIGITS = "0123456789abcdef";
constexpr uint8 HEX_RADIX = 16;
/// msgh_id of an images message: "LHSI" read as little-endian bytes.
constexpr mach_msg_id_t IMAGES_MESSAGE_ID = 0x4953484C;

struct ImagesMessage final
{
    mach_msg_header_t Header;
    mach_msg_body_t Body;
    mach_msg_port_descriptor_t Ports[HOSTED_SURFACE_IMAGE_COUNT];
    uint32 Generation;
    uint8 SessionToken[TOKEN_BYTES];
};

/// Receive buffer: the message plus the audit trailer requested on receive.
struct ImagesReceiveBuffer final
{
    ImagesMessage Message;
    mach_msg_audit_trailer_t Trailer;
};

bool IsAllZero(const RendezvousNonce& nonce) noexcept
{
    uint8 bits = 0;
    for (const uint8 byte : nonce)
    {
        bits = static_cast<uint8>(bits | byte);
    }
    return bits == 0;
}

bool AreTokenBytesEqual(const uint8* left, const Token& right) noexcept
{
    uint8 difference = 0;
    for (usize i = 0; i < TOKEN_BYTES; ++i)
    {
        difference = static_cast<uint8>(difference | (left[i] ^ right[i]));
    }
    return difference == 0;
}

bool IsValidPortDescriptor(const mach_msg_port_descriptor_t& descriptor) noexcept
{
    return descriptor.type == MACH_MSG_PORT_DESCRIPTOR && descriptor.disposition == MACH_MSG_TYPE_PORT_SEND &&
           MACH_PORT_VALID(descriptor.name) == true;
}
} // namespace

bool TryFormatRendezvousName(const RendezvousNonce& nonce, RendezvousName& outName) noexcept
{
    if (IsAllZero(nonce) == true)
    {
        return false;
    }
    RendezvousName name;
    if (name.TryAssign(RENDEZVOUS_NAME_PREFIX) != foundation::StringStatus::Ok)
    {
        return false;
    }
    for (const uint8 byte : nonce)
    {
        if (name.TryAppend(HEX_DIGITS.substr(byte / HEX_RADIX, 1)) != foundation::StringStatus::Ok ||
            name.TryAppend(HEX_DIGITS.substr(byte % HEX_RADIX, 1)) != foundation::StringStatus::Ok)
        {
            return false;
        }
    }
    outName = name;
    return true;
}

bool RendezvousServer::TryStart(const RendezvousName& name) noexcept
{
    if (mReceiveRight != 0)
    {
        return false;
    }
    // bootstrap_check_in(3) creates the dynamic service in this session's bootstrap
    // namespace and returns its receive right; a taken name fails.
    mach_port_t receiveRight = MACH_PORT_NULL;
    if (::bootstrap_check_in(bootstrap_port, name.GetData(), &receiveRight) != KERN_SUCCESS ||
        MACH_PORT_VALID(receiveRight) == false)
    {
        return false;
    }
    mach_port_limits_t limits{};
    limits.mpl_qlimit = RENDEZVOUS_QUEUE_LIMIT;
    // A failed limit change keeps the default queue; senders then see Busy sooner.
    static_cast<void>(::mach_port_set_attributes(::mach_task_self(),
                                                 receiveRight,
                                                 MACH_PORT_LIMITS_INFO,
                                                 reinterpret_cast<mach_port_info_t>(&limits),
                                                 MACH_PORT_LIMITS_INFO_COUNT));
    mReceiveRight = receiveRight;
    return true;
}

void RendezvousServer::Stop() noexcept
{
    if (mReceiveRight == 0)
    {
        return;
    }
    // Destroying the receive right destroys every queued message and its rights, and
    // turns the bootstrap registration into a dead name.
    static_cast<void>(::mach_port_mod_refs(::mach_task_self(), mReceiveRight, MACH_PORT_RIGHT_RECEIVE, -1));
    mReceiveRight = 0;
}

RendezvousReceiveStatus RendezvousServer::TryReceive(int32 expectedProcessId,
                                                     uint32 expectedUserId,
                                                     const Token& token,
                                                     RendezvousImages& outImages) noexcept
{
    if (mReceiveRight == 0)
    {
        return RendezvousReceiveStatus::Failed;
    }
    ImagesReceiveBuffer buffer{};
    const mach_msg_option_t options = MACH_RCV_MSG | MACH_RCV_TIMEOUT |
                                      MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
                                      MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT);
    // Without MACH_RCV_LARGE, an oversized message is dequeued and destroyed by the kernel.
    const kern_return_t result =
        ::mach_msg(&buffer.Message.Header, options, 0, sizeof(buffer), mReceiveRight, 0, MACH_PORT_NULL);
    if (result == MACH_RCV_TIMED_OUT)
    {
        return RendezvousReceiveStatus::Empty;
    }
    if (result == MACH_RCV_TOO_LARGE)
    {
        return RendezvousReceiveStatus::Rejected;
    }
    if (result != MACH_MSG_SUCCESS)
    {
        return RendezvousReceiveStatus::Failed;
    }
    const ImagesMessage& message = buffer.Message;
    const mach_msg_audit_trailer_t& trailer = buffer.Trailer;
    bool isValid = message.Header.msgh_id == IMAGES_MESSAGE_ID && message.Header.msgh_size == sizeof(ImagesMessage) &&
                   (message.Header.msgh_bits & MACH_MSGH_BITS_COMPLEX) != 0 &&
                   message.Header.msgh_remote_port == MACH_PORT_NULL &&
                   message.Body.msgh_descriptor_count == HOSTED_SURFACE_IMAGE_COUNT &&
                   trailer.msgh_trailer_size >= sizeof(mach_msg_audit_trailer_t) && message.Generation != 0;
    for (usize i = 0; i < HOSTED_SURFACE_IMAGE_COUNT && isValid == true; ++i)
    {
        isValid = IsValidPortDescriptor(message.Ports[i]);
    }
    // The kernel writes the audit token, so the sender cannot forge its pid or euid.
    isValid = isValid == true && static_cast<int32>(::audit_token_to_pid(trailer.msgh_audit)) == expectedProcessId &&
              static_cast<uint32>(::audit_token_to_euid(trailer.msgh_audit)) == expectedUserId &&
              AreTokenBytesEqual(message.SessionToken, token) == true;
    if (isValid == false)
    {
        // Releases every right the message carried, including any reply port.
        ::mach_msg_destroy(&buffer.Message.Header);
        return RendezvousReceiveStatus::Rejected;
    }
    RendezvousImages images;
    images.Generation = message.Generation;
    for (usize i = 0; i < HOSTED_SURFACE_IMAGE_COUNT; ++i)
    {
        images.Handles[i] = HostedImageHandle::FromMachPort(message.Ports[i].name);
    }
    outImages = foundation::Move(images);
    return RendezvousReceiveStatus::Ok;
}

bool RendezvousClient::TryConnect(const RendezvousName& name) noexcept
{
    Reset();
    mach_port_t sendRight = MACH_PORT_NULL;
    if (::bootstrap_look_up(bootstrap_port, name.GetData(), &sendRight) != KERN_SUCCESS ||
        MACH_PORT_VALID(sendRight) == false)
    {
        return false;
    }
    mSendRight = sendRight;
    return true;
}

void RendezvousClient::Reset() noexcept
{
    if (mSendRight != 0)
    {
        static_cast<void>(::mach_port_deallocate(::mach_task_self(), mSendRight));
        mSendRight = 0;
    }
}

RendezvousSendStatus
RendezvousClient::TrySend(const Token& token,
                          uint32 generation,
                          const foundation::StaticArray<HostedImageHandle, HOSTED_SURFACE_IMAGE_COUNT>& images) noexcept
{
    if (mSendRight == 0 || generation == 0)
    {
        return RendezvousSendStatus::Failed;
    }
    for (const HostedImageHandle& image : images)
    {
        if (image.GetKind() != HostedHandleKind::MachPort)
        {
            return RendezvousSendStatus::Failed;
        }
    }
    ImagesMessage message{};
    message.Header.msgh_bits = MACH_MSGH_BITS_SET(MACH_MSG_TYPE_COPY_SEND, 0, 0, MACH_MSGH_BITS_COMPLEX);
    message.Header.msgh_size = sizeof(ImagesMessage);
    message.Header.msgh_remote_port = mSendRight;
    message.Header.msgh_local_port = MACH_PORT_NULL;
    message.Header.msgh_id = IMAGES_MESSAGE_ID;
    message.Body.msgh_descriptor_count = HOSTED_SURFACE_IMAGE_COUNT;
    for (usize i = 0; i < HOSTED_SURFACE_IMAGE_COUNT; ++i)
    {
        message.Ports[i].name = images[i].GetMachPort();
        message.Ports[i].disposition = MACH_MSG_TYPE_COPY_SEND;
        message.Ports[i].type = MACH_MSG_PORT_DESCRIPTOR;
    }
    message.Generation = generation;
    std::memcpy(message.SessionToken, token.GetData(), TOKEN_BYTES);
    const kern_return_t result = ::mach_msg(&message.Header,
                                            MACH_SEND_MSG | MACH_SEND_TIMEOUT,
                                            sizeof(message),
                                            0,
                                            MACH_PORT_NULL,
                                            0,
                                            MACH_PORT_NULL);
    if (result == MACH_MSG_SUCCESS)
    {
        return RendezvousSendStatus::Ok;
    }
    if (result == MACH_SEND_TIMED_OUT)
    {
        // mach_msg(2) pseudo-receives an unsent message: every copied send right comes
        // back to this task and coalesces into the name it was copied from. Drop the
        // extra references so the handles keep exactly the rights they owned.
        static_cast<void>(::mach_port_deallocate(::mach_task_self(), mSendRight));
        for (const HostedImageHandle& image : images)
        {
            static_cast<void>(::mach_port_deallocate(::mach_task_self(), image.GetMachPort()));
        }
        return RendezvousSendStatus::Busy;
    }
    return RendezvousSendStatus::Failed;
}
} // namespace ludus::platform::hosted
