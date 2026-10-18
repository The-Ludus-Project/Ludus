#include <ludus/foundation/base/core.h>

#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>

#include <ludus/content/content.h>
#include <ludus/content/json.h>

#include <ludus/audio/audio_source.h>
#include <ludus/audio/content/definitions.h>

#include <new>
#include <span>
#include <string_view>

using namespace ludus::foundation;
namespace content = ludus::content;
namespace audio = ludus::audio;
namespace
{
class Input final : public audio::StreamInput
{
public:
    content::FileReader File;
    audio::Status Read(std::span<uint8> bytes, usize& count) noexcept override
    {
        return File.Read(bytes, count) == content::Status::Ok ? audio::Status::Ok : audio::Status::IoError;
    }
    audio::Status Seek(int64 offset, bool relative) noexcept override
    {
        return File.Seek(offset, relative) == content::Status::Ok ? audio::Status::Ok : audio::Status::IoError;
    }
};
content::Text<64> Hex(content::Digest digest) noexcept
{
    content::Text<64> text;
    constexpr std::string_view hex = "0123456789abcdef";
    for (usize i = 0; i < 32; ++i)
    {
        text.Data[i * 2] = hex[digest.Data[i] >> 4];
        text.Data[i * 2 + 1] = hex[digest.Data[i] & 15];
    }
    text.Length = 64;
    return text;
}
bool Run(int argc, char** argv) noexcept
{
    if (argc < 4)
    {
        LUDUS_LOG_ERROR(ludus::foundation::logging::LOG_CORE,
                        "Usage: ludus_content_validate CONTENT_ROOT OUTPUT_DIRECTORY ROOT_ID...");
        return false;
    }
    const std::string_view root = argv[1];
    content::Catalog catalog, closure;
    content::Bytes bytes;
    content::Diagnostic diagnostic;
    if (content::ReadFile(root, "catalog.json", content::MAX_DOCUMENT_BYTES, bytes) != content::Status::Ok ||
        catalog.Read(bytes.String(), diagnostic) != content::Status::Ok)
    {
        return false;
    }
    const auto catalogHash = Hex(content::Hash(bytes.Data()));
    const std::string_view buses[] = {"master", "sfx", "music"}, groups[] = {"default", "impacts"};
    const audio::content::Routing routing{buses, groups};
    content::Digest definitionHashes[content::MAX_RESOURCES]{};
    bool validated[content::MAX_RESOURCES]{};
    for (int arg = 3; arg < argc; ++arg)
    {
        const auto* resource = catalog.Find(argv[arg]);
        if (resource == nullptr || resource->Type == content::Kind::AudioSource)
        {
            return false;
        }
        if (resource->Type != content::Kind::Sound && resource->Type != content::Kind::Music)
        {
            LUDUS_LOG_ERROR(ludus::foundation::logging::LOG_CORE,
                            "Audio packaging requires a sound or music root; mesh roots are unsupported.");
            return false;
        }
        if (content::ReadFile(root, resource->Path.View(), content::MAX_DOCUMENT_BYTES, bytes) != content::Status::Ok)
        {
            return false;
        }
        const auto resourceIndex = static_cast<usize>(resource - catalog.Entries().data());
        definitionHashes[resourceIndex] = content::Hash(bytes.Data());
        validated[resourceIndex] = true;
        audio::content::Sound sound;
        audio::content::Music music;
        const bool isMusic = resource->Type == content::Kind::Music;
        const auto status = isMusic ? audio::content::ReadMusic(bytes.String(), music, diagnostic)
                                    : audio::content::ReadSound(bytes.String(), sound, diagnostic);
        if (status != content::Status::Ok || (isMusic ? music.Id.View() : sound.Id.View()) != resource->Id.View())
        {
            return false;
        }
        uint32 bus = 0;
        audio::app::EventDescriptor descriptor;
        if ((isMusic ? audio::content::ValidateMusic(music, catalog, routing, bus, diagnostic)
                     : audio::content::Resolve(sound, catalog, routing, descriptor, diagnostic)) != content::Status::Ok)
        {
            return false;
        }
        if (closure.Put(*resource) != content::Status::Ok)
        {
            return false;
        }
        uint32 firstRate = 0;
        for (usize i = 0; i < (isMusic ? 1 : sound.VariationCount); ++i)
        {
            const auto* source = catalog.Find(isMusic ? music.Source.View() : sound.Variations[i].View());
            if (source == nullptr)
            {
                return false;
            }
            const auto path = source->Path.View();
            if (path.ends_with(".wav") == false && path.ends_with(".flac") == false)
            {
                return false;
            }
            Input input;
            audio::SourceInfo info;
            float32 peak[1];
            if (input.File.Open(root, path) != content::Status::Ok ||
                audio::InspectSource(input,
                                     path.ends_with(".flac") ? audio::SourceFormat::Flac : audio::SourceFormat::Wav,
                                     info,
                                     peak) != audio::Status::Ok)
            {
                return false;
            }
            const auto& loop = isMusic ? music.Region : sound.Region;
            if (loop.Enabled && (loop.End > info.Frames || (i > 0 && firstRate != info.SampleRate)))
            {
                return false;
            }
            firstRate = info.SampleRate;
            if (isMusic == false && info.Frames > audio::RESIDENT_PCM_CAP_BYTES / (2 * sizeof(float32)))
            {
                return false;
            }
            if (closure.Put(*source) != content::Status::Ok)
            {
                return false;
            }
        }
    }
    // Record bytes validated above. Definition digests are checked again against
    // their first snapshot so an edit during validation cannot enter the plan.
    content::Bytes catalogBytes;
    if (closure.Write(catalogBytes) != content::Status::Ok)
    {
        return false;
    }
    content::Bytes buffer;
    if (buffer.Resize(content::MAX_DOCUMENT_BYTES) == false)
    {
        return false;
    }
    content::JsonWriter writer(buffer.Data());
    writer.Raw("{\"version\":1,\"catalog\":");
    writer.Raw(catalogBytes.String());
    writer.Raw(",\"input_catalog_sha256\":");
    writer.String(catalogHash.View());
    writer.Raw(",\"files\":[");
    bool first = true;
    for (const auto& resource : closure.Entries())
    {
        content::FileReader file;
        if (file.Open(root, resource.Path.View()) != content::Status::Ok)
        {
            return false;
        }
        content::Hasher hasher;
        uint8 scratch[65536];
        uint64 remaining = file.Size();
        while (remaining != 0)
        {
            usize count = 0;
            if (file.Read(scratch, count) != content::Status::Ok || count == 0 || hasher.Add({scratch, count}) == false)
            {
                return false;
            }
            remaining -= count;
        }
        const auto* original = catalog.Find(resource.Id.View());
        const auto resourceIndex = static_cast<usize>(original - catalog.Entries().data());
        if (validated[resourceIndex] && hasher.Finish() != definitionHashes[resourceIndex])
        {
            return false;
        }
        if (first == false)
        {
            writer.Raw(",");
        }
        first = false;
        writer.Raw("{\"path\":");
        writer.String(resource.Path.View());
        writer.Raw(",\"size\":");
        writer.Integer(file.Size());
        writer.Raw(",\"sha256\":");
        writer.String(Hex(hasher.Finish()).View());
        writer.Raw("}");
    }
    writer.Raw("]}\n");
    content::Bytes output;
    if (writer.Finish(output) != content::Status::Ok)
    {
        return false;
    }
    return content::SaveFile(argv[2], "closure.json", output.Data(), nullptr) == content::Status::Ok;
}
} // namespace
int main(int argc, char** argv)
{
    ludus::foundation::logging::LogConfig log;
    log.EnableConsole = true;
    log.EnableDebugger = false;
    log.EnableFile = false;
    ludus::foundation::logging::LogSystem::Initialize(log);
    struct LoggerCleanup final
    {
        ~LoggerCleanup() noexcept
        {
            ludus::foundation::logging::LogSystem::Shutdown();
        }
    } cleanup;
    if (Run(argc, argv) == false)
    {
        LUDUS_LOG_ERROR(ludus::foundation::logging::LOG_CORE, "Content validation failed; no package published.");
        return 1;
    }
    return 0;
}
