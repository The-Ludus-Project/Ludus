#include <ludus/tooling/shader_compiler/compiler.hpp>

#include "codec.hpp"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <cerrno>
#include <cstring>
#include <string_view>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace
{
using namespace ludus::foundation;
namespace compiler = ludus::tooling::shader_compiler;
constexpr usize FRAME_BYTES = sizeof(uint32);
constexpr int32 CONNECTION_BACKLOG = 16;
constexpr int32 IO_TIMEOUT_MILLISECONDS = 30000;
volatile sig_atomic_t gShouldStop = 0;
void HandleStop(int32) noexcept
{
    gShouldStop = 1;
}
bool Transfer(int32 descriptor, uint8* bytes, usize size, bool isWrite, bool isSocket) noexcept
{
    usize offset = 0;
    while (offset < size && gShouldStop == 0)
    {
        pollfd pending{descriptor, static_cast<short>(isWrite == true ? POLLOUT : POLLIN), 0};
        const int32 ready = ::poll(&pending, 1, IO_TIMEOUT_MILLISECONDS);
        if (ready < 0 && errno == EINTR)
        {
            continue;
        }
        if (ready <= 0)
        {
            return false;
        }
        ssize_t count = 0;
        if (isWrite == true)
        {
            if (isSocket == true)
            {
#if defined(__linux__)
                count = ::send(descriptor, bytes + offset, size - offset, MSG_NOSIGNAL);
#else
                count = ::send(descriptor, bytes + offset, size - offset, 0);
#endif
            }
            else
            {
                count = ::write(descriptor, bytes + offset, size - offset);
            }
        }
        else
        {
            count = ::read(descriptor, bytes + offset, size - offset);
        }
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            continue;
        }
        if (count <= 0)
        {
            return false;
        }
        offset += static_cast<usize>(count);
    }
    return offset == size;
}
bool ProcessConnection(compiler::Compiler& inoutCompiler, int32 input, int32 output, bool isSocket) noexcept
{
    StaticArray<uint8, FRAME_BYTES> header{};
    uint32 size = 0;
    if (Transfer(input, header.GetData(), FRAME_BYTES, false, isSocket) == false ||
        TryReadLittleEndian<uint32>(header.AsSpan(), size) == false || size == 0 || size > compiler::MAX_MESSAGE_BYTES)
    {
        return false;
    }
    Array<uint8> bytes;
    if (bytes.TryResize(size) == false || Transfer(input, bytes.GetData(), size, false, isSocket) == false)
    {
        return false;
    }
    Array<uint8> response;
    if (compiler::internal::Process(inoutCompiler, {bytes.GetData(), bytes.GetSize()}, response) == false)
    {
        return false;
    }
    static_cast<void>(TryWriteLittleEndian<uint32>(static_cast<uint32>(response.GetSize()), header.AsSpan()));
    return Transfer(output, header.GetData(), FRAME_BYTES, true, isSocket) == true &&
           Transfer(output, response.GetData(), response.GetSize(), true, isSocket) == true;
}
} // namespace
int main(int argc, char** argv)
{
    using namespace ludus::foundation;
    namespace compiler = ludus::tooling::shader_compiler;
    if (argc == 2 && std::string_view(argv[1]) == "--version")
    {
        constexpr const char* VERSION = LUDUS_SLANG_VERSION "\n";
        constexpr usize VERSION_BYTES = sizeof(LUDUS_SLANG_VERSION "\n") - 1;
        return ::write(STDOUT_FILENO, VERSION, VERSION_BYTES) == static_cast<ssize_t>(VERSION_BYTES) ? 0 : 1;
    }
    const bool isStdio = argc == 2 && std::string_view(argv[1]) == "--stdio";
    const bool isServer = argc == 3 && std::string_view(argv[1]) == "--serve";
    if (isStdio == false && isServer == false)
    {
        return 2;
    }
    compiler::Compiler shaderCompiler;
    if (shaderCompiler.Initialize() != compiler::Status::Ready)
    {
        return 1;
    }
    struct sigaction action{};
    action.sa_handler = HandleStop;
    sigemptyset(&action.sa_mask);
    if (::sigaction(SIGTERM, &action, nullptr) != 0 || ::sigaction(SIGINT, &action, nullptr) != 0)
    {
        return 1;
    }
    if (isStdio == true)
    {
        while (gShouldStop == 0 && ProcessConnection(shaderCompiler, STDIN_FILENO, STDOUT_FILENO, false) == true)
        {
        }
        return 0;
    }
    sockaddr_un address{};
    const std::string_view endpoint = argv[2];
    if (endpoint.empty() == true || endpoint.size() >= sizeof(address.sun_path))
    {
        return 2;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, endpoint.data(), endpoint.size());
    const int32 listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0)
    {
        return 1;
    }
    // Refuse occupied paths. Never unlink another worker's socket on startup.
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        static_cast<void>(::close(listener));
        return 1;
    }
    if (::chmod(address.sun_path, S_IRUSR | S_IWUSR) != 0 || ::listen(listener, CONNECTION_BACKLOG) != 0)
    {
        static_cast<void>(::close(listener));
        static_cast<void>(::unlink(address.sun_path));
        return 1;
    }
    while (gShouldStop == 0)
    {
        const int32 peer = ::accept(listener, nullptr, nullptr);
        if (peer < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        const int32 peerFlags = ::fcntl(peer, F_GETFL, 0);
        if (peerFlags < 0 || ::fcntl(peer, F_SETFL, peerFlags | O_NONBLOCK) < 0)
        {
            static_cast<void>(::close(peer));
            continue;
        }
#if defined(__APPLE__)
        const int32 shouldSuppressPipeSignal = 1;
        if (::setsockopt(peer, SOL_SOCKET, SO_NOSIGPIPE, &shouldSuppressPipeSignal, sizeof(shouldSuppressPipeSignal)) !=
            0)
        {
            static_cast<void>(::close(peer));
            continue;
        }
#endif
        static_cast<void>(ProcessConnection(shaderCompiler, peer, peer, true));
        static_cast<void>(::close(peer));
    }
    static_cast<void>(::close(listener));
    static_cast<void>(::unlink(address.sun_path));
    return 0;
}
