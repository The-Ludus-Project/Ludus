// Child process of the hosted surface Mach rendezvous probe
// (hosted_mach_tests.cpp). It plays the game side: it looks up the endpoint's
// per-session bootstrap name, creates three ordinary (non-global) IOSurfaces with
// a known first pixel and sends their Mach send rights. It prints the surfaces'
// IDs (raw native-endian uint32 values on standard output) so the parent can show
// that the IDs alone do not grant access, then waits for the parent to close its
// standard input.
//
// Usage: ludus_platform_hosted_mach_child <bootstrap name> <token hex> <send|wrong-token>

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/platform/hosted_surface.h>

#include "internal/hosted_mach.hpp"
#include "internal/hosted_protocol.hpp"

#include <cstring>
#include <string_view>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOReturn.h>
#include <IOSurface/IOSurfaceRef.h>

using ludus::foundation::int32;
using ludus::foundation::uint32;
using ludus::foundation::uint8;
using ludus::foundation::usize;

namespace hosted = ludus::platform::hosted;

namespace
{
constexpr int32 EXIT_USAGE = 2;
constexpr int32 EXIT_LOOKUP = 3;
constexpr int32 EXIT_SURFACE = 4;
constexpr int32 EXIT_SEND = 5;
constexpr int32 SURFACE_EXTENT = 4;
constexpr int32 BYTES_PER_PIXEL = 4;
/// First pixel of image i is PROBE_PIXEL + i.
constexpr uint32 PROBE_PIXEL = 0x11223344U;
constexpr uint32 PROBE_GENERATION = 7;
constexpr uint8 HEX_RADIX = 16;
constexpr uint8 HEX_LETTERS_OFFSET = 10;

bool TryReadToken(std::string_view hex, hosted::Token& outToken)
{
    if (hex.size() != hosted::TOKEN_HEX_BYTES)
    {
        return false;
    }
    for (usize i = 0; i < hosted::TOKEN_BYTES; ++i)
    {
        uint8 value = 0;
        for (usize j = 0; j < 2; ++j)
        {
            const char digit = hex[(i * 2) + j];
            const uint8 nibble =
                digit >= 'a' ? static_cast<uint8>(digit - 'a' + HEX_LETTERS_OFFSET) : static_cast<uint8>(digit - '0');
            value = static_cast<uint8>((value * HEX_RADIX) + nibble);
        }
        outToken[i] = value;
    }
    return true;
}

void SetNumber(CFMutableDictionaryRef dictionary, CFStringRef key, int32 value)
{
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(dictionary, key, number);
    CFRelease(number);
}

IOSurfaceRef CreateSurface(uint32 firstPixel)
{
    CFMutableDictionaryRef properties = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                                  0,
                                                                  &kCFTypeDictionaryKeyCallBacks,
                                                                  &kCFTypeDictionaryValueCallBacks);
    SetNumber(properties, kIOSurfaceWidth, SURFACE_EXTENT);
    SetNumber(properties, kIOSurfaceHeight, SURFACE_EXTENT);
    SetNumber(properties, kIOSurfaceBytesPerElement, BYTES_PER_PIXEL);
    SetNumber(properties, kIOSurfacePixelFormat, static_cast<int32>('RGBA'));
    IOSurfaceRef surface = IOSurfaceCreate(properties);
    CFRelease(properties);
    if (surface == nullptr || IOSurfaceLock(surface, 0, nullptr) != kIOReturnSuccess)
    {
        return surface;
    }
    std::memcpy(IOSurfaceGetBaseAddress(surface), &firstPixel, sizeof(firstPixel));
    static_cast<void>(IOSurfaceUnlock(surface, 0, nullptr));
    return surface;
}
} // namespace

int main(int argumentCount, char** arguments)
{
    if (argumentCount != 4)
    {
        return EXIT_USAGE;
    }
    hosted::RendezvousName name;
    hosted::Token token{};
    const std::string_view mode = arguments[3];
    if (name.TryAssign(arguments[1]) != ludus::foundation::StringStatus::Ok ||
        TryReadToken(arguments[2], token) == false)
    {
        return EXIT_USAGE;
    }
    if (mode == "wrong-token")
    {
        token[0] = static_cast<uint8>(token[0] ^ 0xFFU);
    }
    hosted::RendezvousClient client;
    if (client.TryConnect(name) == false)
    {
        return EXIT_LOOKUP;
    }
    ludus::foundation::StaticArray<IOSurfaceRef, ludus::platform::HOSTED_SURFACE_IMAGE_COUNT> surfaces{};
    ludus::foundation::StaticArray<ludus::platform::HostedImageHandle, ludus::platform::HOSTED_SURFACE_IMAGE_COUNT>
        handles{};
    ludus::foundation::StaticArray<uint32, ludus::platform::HOSTED_SURFACE_IMAGE_COUNT> ids{};
    for (usize i = 0; i < surfaces.GetSize(); ++i)
    {
        surfaces[i] = CreateSurface(PROBE_PIXEL + static_cast<uint32>(i));
        if (surfaces[i] == nullptr)
        {
            return EXIT_SURFACE;
        }
        handles[i] = ludus::platform::HostedImageHandle::FromMachPort(IOSurfaceCreateMachPort(surfaces[i]));
        ids[i] = IOSurfaceGetID(surfaces[i]);
    }
    if (::write(STDOUT_FILENO, ids.GetData(), sizeof(uint32) * ids.GetSize()) !=
        static_cast<ssize_t>(sizeof(uint32) * ids.GetSize()))
    {
        return EXIT_SEND;
    }
    if (client.TrySend(token, PROBE_GENERATION, handles) != hosted::RendezvousSendStatus::Ok)
    {
        return EXIT_SEND;
    }
    // Keep the surfaces alive until the parent has finished with them.
    char byte = 0;
    while (::read(STDIN_FILENO, &byte, 1) > 0)
    {
    }
    for (IOSurfaceRef surface : surfaces)
    {
        CFRelease(surface);
    }
    return 0;
}
