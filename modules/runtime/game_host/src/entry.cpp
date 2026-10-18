// Project-owned GameHost executable (project-live-reload design 1/2/3).
//
// A thin front end over Ludus::GameHost::Run. It parses argv/cwd exactly as the
// v1 executable Run contract requires (positional args and cwd are preserved and
// not reinterpreted), resolves the control fd inherited from the supervisor, and
// runs one play session. The same binary runs headless (no control fd, bounded
// frames) for local smoke and acceptance.
//
// Usage:
//   game_host --module <abs-path> [--control-fd N] [--max-frames N]
//             [--project-id HEX] [--game-id HEX] [--headless]
//
// This executable intentionally links no Qt. The editor supervises it over the
// protocol; it never embeds the editor.

#include <ludus/runtime/game_host/host.h>

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/config/json.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/runtime/configuration/host_options.hpp>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
using ludus::foundation::int32;
using ludus::foundation::uint64;
using ludus::foundation::uint8;
using ludus::foundation::usize;
constexpr ludus::foundation::logging::LogCategory kConfigurationLog{"Configuration"};

struct AuthoredBytes final
{
    uint8* Data = nullptr;
    usize Size = 0;
    ~AuthoredBytes() noexcept
    {
        delete[] Data;
    }
    [[nodiscard]] bool Read(const char* path) noexcept
    {
        const int fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0)
        {
            return false;
        }
        struct CloseFd final
        {
            int Value;
            ~CloseFd() noexcept
            {
                (void)::close(Value);
            }
        } closeFd{fd};
        struct stat info = {};
        if (::fstat(fd, &info) != 0 || S_ISREG(info.st_mode) == false || info.st_size < 20 || info.st_size > 262144)
        {
            return false;
        }
        Size = static_cast<usize>(info.st_size);
        Data = new (std::nothrow) uint8[Size];
        if (Data == nullptr)
        {
            return false;
        }
        usize offset = 0;
        while (offset < Size)
        {
            const ssize_t count = ::read(fd, Data + offset, Size - offset);
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
        uint8 extra = 0;
        return ::read(fd, &extra, 1) == 0;
    }
};

[[nodiscard]] bool ParseU64(std::string_view text, uint64& out) noexcept
{
    if (text.empty())
    {
        return false;
    }
    uint64 value = 0;
    for (const char c : text)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
        const uint64 digit = static_cast<uint64>(c - '0');
        if (value > (~uint64{0} - digit) / 10)
        {
            return false;
        }
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

[[nodiscard]] bool ParseHex(std::string_view text, uint64& out) noexcept
{
    if (text.empty() || text.size() > 16)
    {
        return false;
    }
    uint64 value = 0;
    for (const char c : text)
    {
        value <<= 4;
        if (c >= '0' && c <= '9')
        {
            value |= static_cast<uint64>(c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            value |= static_cast<uint64>(c - 'a' + 10);
        }
        else if (c >= 'A' && c <= 'F')
        {
            value |= static_cast<uint64>(c - 'A' + 10);
        }
        else
        {
            return false;
        }
    }
    out = value;
    return true;
}
} // namespace

ludus::foundation::int32
ludus::runtime::game_host::RunMain(ludus::foundation::int32 argc, const char* const* argv, StaticEntryFn entry) noexcept
{
    ludus::foundation::logging::LogConfig logConfig;
    logConfig.EnableConsole = true;
    logConfig.EnableFile = false;
    logConfig.Mode = ludus::foundation::logging::LogMode::Synchronous;
    (void)ludus::foundation::logging::LogSystem::Initialize(logConfig);
    struct LogShutdown final
    {
        ~LogShutdown() noexcept
        {
            (void)ludus::foundation::logging::LogSystem::Shutdown();
        }
    } shutdown;

    ludus::runtime::game_host::HostConfig config;
    config.Source = entry == nullptr ? GameplaySource::DynamicModule : GameplaySource::Static;
    config.Mode = ludus::runtime::game_host::Presentation::Windowed;

    std::string modulePath;
    std::string authoredPath;
    AuthoredBytes authored;
    // Parse selected files once, then apply explicit CLI values at Launch rank.
    // Context is cold startup state; Run receives ordinary HostConfig options.
    namespace cfg = ludus::foundation::config;
    cfg::Context settings;
    cfg::Diagnostic diagnostic;
    const auto& domain = ludus::foundation::GetSystemAllocationDomain();
    if (settings.Initialize(ludus::runtime::configuration::HostSchema(), domain, diagnostic) != cfg::Status::Ok)
    {
        return static_cast<int>(RunResult::Internal);
    }
    const char* configurationPath = nullptr;
    const char* preferencePath = nullptr;
    bool explicitFrames = false;
    bool explicitHeadless = false;
    bool inspect = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        auto nextValue = [&](std::string_view& out) -> bool {
            if (i + 1 >= argc)
            {
                return false;
            }
            out = argv[++i];
            return true;
        };

        if (arg == "--module" || arg == "--inspect-module")
        {
            std::string_view value;
            if (nextValue(value) == false)
            {
                return static_cast<int>(ludus::runtime::game_host::RunResult::BadArguments);
            }
            modulePath = std::string(value);
            inspect = arg == "--inspect-module";
        }
        else if (arg == "--control-fd")
        {
            std::string_view value;
            uint64 fd = 0;
            if (nextValue(value) == false || ParseU64(value, fd) == false || fd > 0x7fffffffU)
            {
                return static_cast<int>(ludus::runtime::game_host::RunResult::BadArguments);
            }
            config.ControlFd = static_cast<int32>(fd);
        }
        else if (arg == "--max-frames")
        {
            std::string_view value;
            uint64 frames = 0;
            if (nextValue(value) == false || ParseU64(value, frames) == false)
            {
                return static_cast<int>(ludus::runtime::game_host::RunResult::BadArguments);
            }
            config.MaxFrames = frames;
            explicitFrames = true;
        }
        else if (arg == "--project-id")
        {
            std::string_view value;
            if (nextValue(value) == false || ParseHex(value, config.ProjectId) == false)
            {
                return static_cast<int>(ludus::runtime::game_host::RunResult::BadArguments);
            }
        }
        else if (arg == "--game-id")
        {
            std::string_view value;
            if (nextValue(value) == false || ParseHex(value, config.GameId) == false)
            {
                return static_cast<int>(ludus::runtime::game_host::RunResult::BadArguments);
            }
        }
        else if (arg == "--project-epoch")
        {
            std::string_view value;
            if (nextValue(value) == false || ParseHex(value, config.ProjectEpoch) == false || config.ProjectEpoch == 0)
            {
                return static_cast<int>(ludus::runtime::game_host::RunResult::BadArguments);
            }
        }
        else if (arg == "--generation")
        {
            std::string_view value;
            if (nextValue(value) == false || ParseHex(value, config.InitialGeneration) == false ||
                config.InitialGeneration == 0)
            {
                return static_cast<int>(ludus::runtime::game_host::RunResult::BadArguments);
            }
        }
        else if (arg == "--authored-document")
        {
            std::string_view value;
            if (nextValue(value) == false || value.empty() || authoredPath.empty() == false)
            {
                return static_cast<int>(RunResult::BadArguments);
            }
            authoredPath = value;
        }
        else if (arg == "--await-load")
        {
            config.AwaitLoad = true;
        }
        else if (arg == "--config" || arg == "--preferences")
        {
            const char*& path = arg == "--config" ? configurationPath : preferencePath;
            std::string_view value;
            if (path != nullptr || nextValue(value) == false || value.empty())
            {
                return static_cast<int>(RunResult::BadArguments);
            }
            path = argv[i];
        }
        else if (arg == "--headless")
        {
            config.Mode = ludus::runtime::game_host::Presentation::Headless;
            explicitHeadless = true;
        }
        else if (arg == "--identity")
        {
            // Report the host ABI identity and exit (acceptance/debug helper).
            const std::string_view id = ludus::runtime::game_host::HostIdentity();
            (void)::write(STDOUT_FILENO, id.data(), id.size());
            (void)::write(STDOUT_FILENO, "\n", 1);
            return 0;
        }
    }

    if (entry == nullptr && modulePath.empty())
    {
        return static_cast<int>(ludus::runtime::game_host::RunResult::BadArguments);
    }
    config.ModulePath = modulePath;
    if (inspect)
    {
        ludus::runtime::game_api::GameMetadata metadata;
        const auto result = ludus::runtime::game_host::InspectModule(modulePath, metadata);
        if (result == ludus::runtime::game_host::RunResult::Ok)
        {
            // The loader has already checked this identity against the host.
            std::string identity;
            for (const char c : std::string_view(metadata.Identity, metadata.IdentityLength))
            {
                if (c == '\\' || c == '\"')
                {
                    identity.push_back('\\');
                }
                if (static_cast<ludus::foundation::uint8>(c) < 0x20)
                {
                    return static_cast<int>(ludus::runtime::game_host::RunResult::Internal);
                }
                identity.push_back(c);
            }
            const std::string record =
                "{\"sdk_identity\":\"" + identity + "\",\"abi_major\":" + std::to_string(metadata.AbiMajor) +
                ",\"abi_minor\":" + std::to_string(metadata.AbiMinor) +
                ",\"capabilities\":" + std::to_string(metadata.Capabilities) +
                ",\"property_schema\":" + std::to_string(metadata.PropertySchemaVersion) +
                ",\"checkpoint_schema\":" + std::to_string(metadata.CheckpointSchemaVersion) + "}\n";
            (void)::write(STDOUT_FILENO, record.data(), record.size());
        }
        return static_cast<int>(result);
    }

    const auto loadSettings = [&](const char* path, cfg::Layer layer) noexcept -> bool {
        if (path == nullptr)
        {
            return true;
        }
        AuthoredBytes bytes;
        if (bytes.Read(path) == false)
        {
            LUDUS_LOG_ERROR(kConfigurationLog, "Cannot read bounded configuration file '{}'", path);
            return false;
        }
        auto status = cfg::PrepareJson(settings,
                                       {reinterpret_cast<const char*>(bytes.Data), bytes.Size},
                                       layer,
                                       ludus::runtime::configuration::HOST_SCHEMA,
                                       settings.Revision(),
                                       domain,
                                       diagnostic);
        if (status == cfg::Status::Ok)
        {
            status = settings.Commit(settings.Revision(), diagnostic);
        }
        if (status != cfg::Status::Ok)
        {
            if (diagnostic.ByteOffset != cfg::UNKNOWN_BYTE_OFFSET)
            {
                LUDUS_LOG_ERROR(kConfigurationLog,
                                "Configuration {} in '{}' at byte {}",
                                cfg::StatusName(status),
                                path,
                                diagnostic.ByteOffset);
            }
            else
            {
                LUDUS_LOG_ERROR(kConfigurationLog,
                                "Configuration {} in '{}' at key '{}' record {}",
                                cfg::StatusName(status),
                                path,
                                diagnostic.Key.GetView(),
                                diagnostic.Record);
            }
        }
        return status == cfg::Status::Ok;
    };
    if (loadSettings(configurationPath, cfg::Layer::Project) == false ||
        loadSettings(preferencePath, cfg::Layer::Preference) == false)
    {
        return static_cast<int>(RunResult::BadArguments);
    }
    cfg::Assignment launch[2]{};
    usize launchCount = 0;
    if (explicitFrames)
    {
        launch[launchCount++] = { .Name = "host.max_frames", .Data = cfg::Value::FromUint64(config.MaxFrames) };
    }
    if (explicitHeadless)
    {
        launch[launchCount++] = { .Name = "host.headless", .Data = cfg::Value::FromBool(true) };
    }
    if (settings.Prepare(cfg::Layer::Launch, {launch, launchCount}, true, settings.Revision(), diagnostic) !=
            cfg::Status::Ok ||
        settings.Commit(settings.Revision(), diagnostic) != cfg::Status::Ok)
    {
        return static_cast<int>(RunResult::BadArguments);
    }
    ludus::runtime::configuration::HostOptions options;
    if (ludus::runtime::configuration::ReadHostOptions(settings, options) != cfg::Status::Ok ||
        settings.SealStartup() != cfg::Status::Ok)
    {
        return static_cast<int>(RunResult::Internal);
    }
    config.MaxFrames = options.MaxFrames;
    config.Mode = options.Headless ? Presentation::Headless : Presentation::Windowed;

    if (authoredPath.empty() == false)
    {
        if (authored.Read(authoredPath.c_str()) == false)
        {
            return static_cast<int>(RunResult::BadArguments);
        }
        config.AuthoredDocument = {authored.Data, authored.Size};
    }
    const RunResult result = entry == nullptr ? Run(config) : RunStatic(config, entry);
    return static_cast<int>(result);
}
