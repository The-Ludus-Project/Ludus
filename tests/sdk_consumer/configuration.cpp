#include <ludus/audio/configuration/audio_configuration.hpp>
#include <ludus/foundation/config/json.hpp>
#include <ludus/runtime/configuration/host_options.hpp>
#include <ludus/runtime/configuration/schema_catalog.hpp>

bool ExerciseInstalledConfiguration() noexcept
{
    using namespace ludus::foundation;
    using namespace config;
    Context context;
    Diagnostic error;
    if (context.Initialize(ludus::runtime::configuration::HostSchema(), GetSystemAllocationDomain(), error) !=
        Status::Ok)
    {
        return false;
    }
    const Assignment edits[]{{ .Name = "host.max_frames", .Data = Value::FromUint64(42) }};
    if (context.Prepare(Layer::Launch, edits, false, context.Revision(), error) != Status::Ok ||
        context.Commit(context.Revision(), error) != Status::Ok)
    {
        return false;
    }
    ludus::runtime::configuration::HostOptions options;
    uint8 bytes[2048]{};
    std::span<const uint8> output;
    if (ludus::runtime::configuration::ReadHostOptions(context, options) != Status::Ok || options.MaxFrames != 42 ||
        WriteLayer(context, Layer::Launch, ludus::runtime::configuration::HOST_SCHEMA, bytes, output) != Status::Ok)
    {
        return false;
    }
    // The installed catalog selects the audio schema explicitly; its owner adapters read plain options.
    namespace audio = ludus::audio::configuration;
    const ludus::runtime::configuration::SchemaEntry* schemaOrNull =
        ludus::runtime::configuration::FindSchemaOrNull(audio::AUDIO_SCHEMA);
    Context settings;
    if (schemaOrNull == nullptr ||
        settings.Initialize(schemaOrNull->Settings, GetSystemAllocationDomain(), error) != Status::Ok)
    {
        return false;
    }
    const Assignment volume[]{{ .Name = "audio.volume.music", .Data = Value::FromFloat32(0.5F) }};
    audio::AudioMixOptions mix;
    audio::AudioSessionOptions session;
    return settings.Prepare(Layer::Preference, volume, false, settings.Revision(), error) == Status::Ok &&
           settings.Commit(settings.Revision(), error) == Status::Ok &&
           audio::ReadAudioMixOptions(settings, mix) == Status::Ok && mix.MusicVolume == 0.5F &&
           audio::ReadAudioSessionOptions(settings, session) == Status::Ok &&
           session.SampleRate == ludus::audio::DEFAULT_SAMPLE_RATE;
}
