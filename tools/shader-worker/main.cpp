#include "worker_platform.hpp"

#include <ludus/tooling/shader_compiler/compiler.hpp>

#include "codec.hpp"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <string_view>

namespace ludus::tools::shader_worker
{
using namespace foundation;
namespace compiler = tooling::shader_compiler;
namespace
{
constexpr usize FRAME_BYTES = sizeof(uint32);
} // namespace
bool ProcessConnection(compiler::Compiler& inoutCompiler, Stream& inoutStream, uint32 connectionLanes) noexcept
{
    StaticArray<uint8, FRAME_BYTES> header{};
    uint32 size = 0;
    if (inoutStream.Transfer(header.GetData(), FRAME_BYTES, false) == false ||
        TryReadLittleEndian<uint32>(header.AsSpan(), size) == false || size == 0 || size > compiler::MAX_MESSAGE_BYTES)
    {
        return false;
    }
    Array<uint8> bytes;
    if (bytes.TryResize(size) == false || inoutStream.Transfer(bytes.GetData(), size, false) == false)
    {
        return false;
    }
    Array<uint8> response;
    if (compiler::internal::Process(inoutCompiler, {bytes.GetData(), bytes.GetSize()}, connectionLanes, response) ==
        false)
    {
        return false;
    }
    static_cast<void>(TryWriteLittleEndian<uint32>(static_cast<uint32>(response.GetSize()), header.AsSpan()));
    return inoutStream.Transfer(header.GetData(), FRAME_BYTES, true) == true &&
           inoutStream.Transfer(response.GetData(), response.GetSize(), true) == true;
}
} // namespace ludus::tools::shader_worker
int main(int argc, char** argv)
{
    using namespace ludus::foundation;
    namespace compiler = ludus::tooling::shader_compiler;
    namespace worker = ludus::tools::shader_worker;
    if (argc == 2 && std::string_view(argv[1]) == "--version")
    {
        return worker::WriteVersion(LUDUS_SLANG_VERSION "\n") == true ? 0 : worker::EXIT_FAILURE_STATUS;
    }
    const bool shouldStopWithParent = argc >= 3 && std::string_view(argv[argc - 1]) == "--stop-with-parent";
    const int32 modeArgumentCount = shouldStopWithParent == true ? argc - 1 : argc;
    const bool isStdio = modeArgumentCount == 2 && std::string_view(argv[1]) == "--stdio";
    const bool isServer = modeArgumentCount == 3 && std::string_view(argv[1]) == "--serve";
    if (isStdio == false && isServer == false)
    {
        return worker::EXIT_USAGE;
    }
    compiler::Compiler shaderCompiler;
    if (shaderCompiler.Initialize() != compiler::Status::Ready || worker::InstallStopHandlers() == false)
    {
        return worker::EXIT_FAILURE_STATUS;
    }
    if (shouldStopWithParent == true && worker::WatchParent() == false)
    {
        return worker::EXIT_FAILURE_STATUS;
    }
    if (isStdio == true)
    {
        return worker::ServeStandardStreams(shaderCompiler);
    }
    return worker::ServeEndpoint(shaderCompiler, argv[2]);
}
