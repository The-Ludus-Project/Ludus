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
//             [--config FILE | --startup-config FILE] [--preferences FILE]
//
// This executable intentionally links no Qt. The editor supervises it over the
// protocol; it never embeds the editor.

#include <ludus/runtime/game_host/host.h>

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/config/json.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/parsing/json.hpp>
#include <ludus/runtime/configuration/host_options.hpp>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>

#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
#    include <windows.h>
#else
#    include <fcntl.h>
#    include <sys/stat.h>
#    include <unistd.h>
#endif

namespace
{
namespace foundation_config = ludus::foundation::config;
namespace parsing = ludus::foundation::parsing;
using ludus::foundation::AllocationDomain;
using ludus::foundation::int32;
using ludus::foundation::uint64;
using ludus::foundation::uint8;
using ludus::foundation::usize;
constexpr ludus::foundation::logging::LogCategory kConfigurationLog{"Configuration"};
/// Smallest and largest authored gameplay document the host reads, in bytes.
constexpr uint64 MIN_AUTHORED_FILE_BYTES = 20;
constexpr uint64 MAX_AUTHORED_FILE_BYTES = 262144;
/// Largest control descriptor accepted from the command line: it must fit an int.
constexpr uint64 MAX_FILE_DESCRIPTOR = 0x7fffffffU;

void WriteStandardOutput(std::string_view text) noexcept
{
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    const HANDLE outputOrNull = GetStdHandle(STD_OUTPUT_HANDLE);
    if (outputOrNull != nullptr && outputOrNull != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        (void)WriteFile(outputOrNull, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    }
#else
    (void)::write(STDOUT_FILENO, text.data(), text.size());
#endif
}

struct AuthoredBytes final
{
    uint8* Data = nullptr;
    usize Size = 0;
    ~AuthoredBytes() noexcept
    {
        delete[] Data;
    }
    [[nodiscard]] bool Read(const char* path,
                            usize minimumBytes = MIN_AUTHORED_FILE_BYTES,
                            usize maximumBytes = MAX_AUTHORED_FILE_BYTES) noexcept
    {
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
        namespace filesystem = ludus::foundation::filesystem;
        const std::string_view nativePath(path);
        const usize separator = nativePath.find_last_of("/\\");
        const std::string_view root =
            separator == std::string_view::npos ? std::string_view(".") : nativePath.substr(0, separator + 1);
        const std::string_view leaf =
            separator == std::string_view::npos ? nativePath : nativePath.substr(separator + 1);
        filesystem::Directory directory;
        filesystem::File file;
        if (directory.Open(root).Succeeded() == false || directory.OpenRead(leaf, file).Succeeded() == false ||
            file.Size() < minimumBytes || file.Size() > maximumBytes)
        {
            return false;
        }
        Size = static_cast<usize>(file.Size());
        Data = new (std::nothrow) uint8[Size];
        if (Data == nullptr)
        {
            return false;
        }
        const filesystem::ReadResult read = file.ReadAt(0, {Data, Size});
        return read.Outcome.Succeeded() == true && read.BytesRead == Size;
#else
        const int fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
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
        if (::fstat(fd, &info) != 0 || S_ISREG(info.st_mode) == false ||
            info.st_size < static_cast<off_t>(minimumBytes) || info.st_size > static_cast<off_t>(maximumBytes))
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
#endif
    }
};

[[nodiscard]] bool ReadConfigurationFile(const char* path, AuthoredBytes& outBytes) noexcept
{
    if (outBytes.Read(path, 1, foundation_config::MAX_BUNDLE_BYTES) == true)
    {
        return true;
    }
    LUDUS_LOG_ERROR(kConfigurationLog,
                    "Cannot read configuration '{}'; supply a regular cooked file no larger than {} bytes",
                    path,
                    foundation_config::MAX_BUNDLE_BYTES);
    return false;
}

[[nodiscard]] bool CommitConfigurationLayer(foundation_config::Context& inoutSettings,
                                            foundation_config::Status status,
                                            const char* path,
                                            foundation_config::Layer layer,
                                            foundation_config::Diagnostic& inoutDiagnostic) noexcept
{
    if (status == foundation_config::Status::Ok)
    {
        status = inoutSettings.Commit(inoutSettings.Revision(), inoutDiagnostic);
    }
    if (status == foundation_config::Status::Ok)
    {
        return true;
    }
    if (inoutDiagnostic.ByteOffset != foundation_config::UNKNOWN_BYTE_OFFSET)
    {
        LUDUS_LOG_ERROR(kConfigurationLog,
                        "Configuration {} in '{}' at byte {}; recook the {} layer with the matching SDK",
                        foundation_config::StatusName(status),
                        path,
                        inoutDiagnostic.ByteOffset,
                        foundation_config::LayerName(layer));
    }
    else
    {
        LUDUS_LOG_ERROR(kConfigurationLog,
                        "Configuration {} in '{}' at key '{}' record {}; recook the {} layer with the matching SDK",
                        foundation_config::StatusName(status),
                        path,
                        inoutDiagnostic.Key.GetView(),
                        inoutDiagnostic.Record,
                        foundation_config::LayerName(layer));
    }
    return false;
}

[[nodiscard]] bool LoadConfigurationLayer(foundation_config::Context& inoutSettings,
                                          const char* pathOrNull,
                                          foundation_config::Layer layer,
                                          const AllocationDomain& domain) noexcept
{
    if (pathOrNull == nullptr)
    {
        return true;
    }
    AuthoredBytes bytes;
    if (ReadConfigurationFile(pathOrNull, bytes) == false)
    {
        return false;
    }
    foundation_config::Diagnostic diagnostic;
    const foundation_config::Status status =
        foundation_config::PrepareJson(inoutSettings,
                                       {reinterpret_cast<const char*>(bytes.Data), bytes.Size},
                                       layer,
                                       ludus::runtime::configuration::HOST_SCHEMA,
                                       inoutSettings.Revision(),
                                       domain,
                                       diagnostic);
    return CommitConfigurationLayer(inoutSettings, status, pathOrNull, layer, diagnostic);
}

[[nodiscard]] bool LoadStartupConfiguration(foundation_config::Context& inoutSettings,
                                            const char* path,
                                            const AllocationDomain& domain) noexcept
{
    AuthoredBytes bytes;
    if (ReadConfigurationFile(path, bytes) == false)
    {
        return false;
    }
    constexpr uint64 STARTUP_VERSION = 1;
    constexpr usize STARTUP_JSON_DEPTH = 6;
    constexpr usize FIRST_STARTUP_LAYER = static_cast<usize>(foundation_config::Layer::Engine);
    constexpr usize LAST_STARTUP_LAYER = static_cast<usize>(foundation_config::Layer::Quality);
    parsing::JsonLimits limits;
    limits.MaxInputBytes = foundation_config::MAX_BUNDLE_BYTES;
    limits.MaxDepth = STARTUP_JSON_DEPTH;
    parsing::JsonDocument document(domain);
    parsing::ParseError parseError;
    const parsing::ParseStatus parsed =
        document.Read({reinterpret_cast<const char*>(bytes.Data), bytes.Size}, parseError, limits);
    if (parsed != parsing::ParseStatus::Ok)
    {
        LUDUS_LOG_ERROR(kConfigurationLog,
                        "Cannot parse startup configuration '{}' at byte {}; recook the manifest",
                        path,
                        parseError.Offset);
        return false;
    }
    const parsing::JsonValue root = document.Root();
    const parsing::JsonValue layers = root.Get("layers");
    uint64 version = 0;
    if (root.Fields({"version", "layers"}) == false || root.Get("version").Integer(version) == false ||
        version != STARTUP_VERSION || layers.Object() == false)
    {
        LUDUS_LOG_ERROR(kConfigurationLog,
                        "Invalid startup configuration '{}'; recook the manifest with the matching SDK",
                        path);
        return false;
    }

    usize layerCount = 0;
    for (usize index = FIRST_STARTUP_LAYER; index <= LAST_STARTUP_LAYER; ++index)
    {
        const foundation_config::Layer layer = static_cast<foundation_config::Layer>(index);
        const parsing::JsonValue value = layers.Get(foundation_config::LayerName(layer));
        if (value.Valid() == false)
        {
            continue;
        }
        ++layerCount;
        foundation_config::Diagnostic diagnostic;
        const foundation_config::Status status =
            foundation_config::PrepareJsonLayer(inoutSettings,
                                                value,
                                                layer,
                                                ludus::runtime::configuration::HOST_SCHEMA,
                                                inoutSettings.Revision(),
                                                domain,
                                                diagnostic);
        if (CommitConfigurationLayer(inoutSettings, status, path, layer, diagnostic) == false)
        {
            return false;
        }
    }
    if (layerCount != layers.MemberCount())
    {
        LUDUS_LOG_ERROR(
            kConfigurationLog,
            "Unsupported rank in startup configuration '{}'; recook the manifest and supply preferences separately",
            path);
        return false;
    }
    return true;
}

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
    foundation_config::Context settings;
    foundation_config::Diagnostic diagnostic;
    const AllocationDomain& domain = ludus::foundation::GetSystemAllocationDomain();
    if (settings.Initialize(ludus::runtime::configuration::HostSchema(), domain, diagnostic) !=
        foundation_config::Status::Ok)
    {
        return static_cast<int>(RunResult::Internal);
    }
    const char* configurationPathOrNull = nullptr;
    const char* startupPathOrNull = nullptr;
    const char* preferencePathOrNull = nullptr;
    bool hasExplicitFrames = false;
    bool hasExplicitHeadless = false;
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
            if (nextValue(value) == false || ParseU64(value, fd) == false || fd > MAX_FILE_DESCRIPTOR)
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
            hasExplicitFrames = true;
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
        else if (arg == "--config" || arg == "--preferences" || arg == "--startup-config")
        {
            const char*& pathOrNull = arg == "--config"        ? configurationPathOrNull
                                      : arg == "--preferences" ? preferencePathOrNull
                                                               : startupPathOrNull;
            std::string_view value;
            if (pathOrNull != nullptr || nextValue(value) == false || value.empty())
            {
                LUDUS_LOG_ERROR(kConfigurationLog, "Supply {} once with a cooked file path", arg);
                return static_cast<int>(RunResult::BadArguments);
            }
            pathOrNull = argv[i];
        }
        else if (arg == "--headless")
        {
            config.Mode = ludus::runtime::game_host::Presentation::Headless;
            hasExplicitHeadless = true;
        }
        else if (arg == "--identity")
        {
            // Report the host ABI identity and exit (acceptance/debug helper).
            const std::string_view id = ludus::runtime::game_host::HostIdentity();
            WriteStandardOutput(id);
            WriteStandardOutput("\n");
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
            WriteStandardOutput(record);
        }
        return static_cast<int>(result);
    }

    if (startupPathOrNull != nullptr && configurationPathOrNull != nullptr)
    {
        LUDUS_LOG_ERROR(kConfigurationLog,
                        "Choose either --config for a project layer or --startup-config for a cooked manifest");
        return static_cast<int>(RunResult::BadArguments);
    }
    if ((startupPathOrNull != nullptr && LoadStartupConfiguration(settings, startupPathOrNull, domain) == false) ||
        LoadConfigurationLayer(settings, configurationPathOrNull, foundation_config::Layer::Project, domain) == false ||
        LoadConfigurationLayer(settings, preferencePathOrNull, foundation_config::Layer::Preference, domain) == false)
    {
        return static_cast<int>(RunResult::BadArguments);
    }
    foundation_config::Assignment launch[2]{};
    usize launchCount = 0;
    if (hasExplicitFrames == true)
    {
        launch[launchCount++] =
        {
            .Name = "host.max_frames",
            .Data = foundation_config::Value::FromUint64(config.MaxFrames),
        };
    }
    if (hasExplicitHeadless == true)
    {
        launch[launchCount++] = { .Name = "host.headless", .Data = foundation_config::Value::FromBool(true) };
    }
    if (settings.Prepare(foundation_config::Layer::Launch,
                         {launch, launchCount},
                         true,
                         settings.Revision(),
                         diagnostic) != foundation_config::Status::Ok ||
        settings.Commit(settings.Revision(), diagnostic) != foundation_config::Status::Ok)
    {
        return static_cast<int>(RunResult::BadArguments);
    }
    ludus::runtime::configuration::HostOptions options;
    if (ludus::runtime::configuration::ReadHostOptions(settings, options) != foundation_config::Status::Ok ||
        settings.SealStartup() != foundation_config::Status::Ok)
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
