#include <ludus/foundation/base/config.h>
#include <ludus/foundation/config/json.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/runtime/configuration/host_options.hpp>

#include <cerrno>
#include <string_view>

#if defined(LUDUS_PLATFORM_WINDOWS)
#    include <fcntl.h>
#    include <io.h>
#else
#    include <unistd.h>
#endif

namespace
{
using namespace ludus::foundation;
constexpr logging::LogCategory kConfigurationLog{"Configuration"};
#if defined(LUDUS_PLATFORM_WINDOWS)
constexpr int32 STANDARD_INPUT = 0;
constexpr int32 STANDARD_OUTPUT = 1;
/// The CRT transfers at most this many bytes per call.
constexpr usize MAX_TRANSFER_BYTES = 0x7FFFFFFF;
#endif

/// Writes part of the bytes to standard output: the byte count, or negative on failure.
isize WriteStandardOutput(const uint8* bytes, usize count) noexcept
{
#if defined(LUDUS_PLATFORM_WINDOWS)
    const usize bounded = count < MAX_TRANSFER_BYTES ? count : MAX_TRANSFER_BYTES;
    return ::_write(STANDARD_OUTPUT, bytes, static_cast<unsigned int>(bounded));
#else
    return ::write(STDOUT_FILENO, bytes, count);
#endif
}

/// Reads part of standard input: the byte count, zero at the end, or negative on failure.
isize ReadStandardInput(uint8* bytes, usize count) noexcept
{
#if defined(LUDUS_PLATFORM_WINDOWS)
    const usize bounded = count < MAX_TRANSFER_BYTES ? count : MAX_TRANSFER_BYTES;
    return ::_read(STANDARD_INPUT, bytes, static_cast<unsigned int>(bounded));
#else
    return ::read(STDIN_FILENO, bytes, count);
#endif
}

bool Write(std::span<const uint8> bytes) noexcept
{
    usize offset = 0;
    while (offset < bytes.size())
    {
        const isize count = WriteStandardOutput(bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            return false;
        }
        offset += static_cast<usize>(count);
    }
    return true;
}
} // namespace
int main(int argc, char** argv)
{
    using namespace ludus::foundation;
    using namespace config;
    logging::LogConfig logConfig;
    logConfig.EnableConsole = true;
    logConfig.EnableFile = false;
    logConfig.Mode = logging::LogMode::Synchronous;
    (void)logging::LogSystem::Initialize(logConfig);
#if defined(LUDUS_PLATFORM_WINDOWS)
    // Bundles and schemas are byte streams; text mode would translate line endings.
    (void)::_setmode(STANDARD_INPUT, _O_BINARY);
    (void)::_setmode(STANDARD_OUTPUT, _O_BINARY);
#endif
    struct Shutdown final
    {
        ~Shutdown() noexcept
        {
            (void)logging::LogSystem::Shutdown();
        }
    } shutdown;
    if (argc < 2 || argc > 3)
    {
        LUDUS_LOG_ERROR(kConfigurationLog, "Usage: ludus-config --schema | --validate [project|preference]");
        return 2;
    }
    Context context;
    Diagnostic error;
    const auto& domain = GetSystemAllocationDomain();
    if (context.Initialize(ludus::runtime::configuration::HostSchema(), domain, error) != Status::Ok)
    {
        return 1;
    }
    uint8 bytes[MAX_BUNDLE_BYTES + 1]{};
    std::span<const uint8> output;
    const std::string_view action = argv[1];
    Status status = Status::InvalidState;
    if (action == "--schema" && argc == 2)
    {
        status = WriteSchema(context, ludus::runtime::configuration::HOST_SCHEMA, bytes, output);
    }
    else if (action == "--validate")
    {
        Layer layer = Layer::Project;
        if (argc == 3)
        {
            if (std::string_view(argv[2]) == "preference")
            {
                layer = Layer::Preference;
            }
            else if (std::string_view(argv[2]) != "project")
            {
                return 2;
            }
        }
        usize size = 0;
        while (size < sizeof(bytes))
        {
            const isize count = ReadStandardInput(bytes + size, sizeof(bytes) - size);
            if (count < 0 && errno == EINTR)
            {
                continue;
            }
            if (count < 0)
            {
                return 1;
            }
            if (count == 0)
            {
                break;
            }
            size += static_cast<usize>(count);
        }
        if (size > MAX_BUNDLE_BYTES)
        {
            return 1;
        }
        status = PrepareJson(context,
                             {reinterpret_cast<const char*>(bytes), size},
                             layer,
                             ludus::runtime::configuration::HOST_SCHEMA,
                             context.Revision(),
                             domain,
                             error);
        if (status == Status::Ok)
        {
            status = context.Commit(context.Revision(), error);
        }
        if (status == Status::Ok)
        {
            status = WriteLayer(context, layer, ludus::runtime::configuration::HOST_SCHEMA, bytes, output);
        }
    }
    if (status != Status::Ok)
    {
        if (error.ByteOffset != UNKNOWN_BYTE_OFFSET)
        {
            LUDUS_LOG_ERROR(kConfigurationLog, "Configuration {} at byte {}", StatusName(status), error.ByteOffset);
        }
        else
        {
            LUDUS_LOG_ERROR(kConfigurationLog,
                            "Configuration {} at key '{}' record {}",
                            StatusName(status),
                            error.Key.GetView(),
                            error.Record);
        }
        return 1;
    }
    return Write(output) ? 0 : 1;
}
