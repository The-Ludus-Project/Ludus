// Integration tests for the first Live configuration owner: the audio mix group
// applied through the production offline renderer, with acknowledgement,
// preservation on failed preparation, pending restart and snapshot retirement.
#include <ludus/audio/configuration/audio_configuration.hpp>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::audio;
using namespace ludus::audio::configuration;
using namespace ludus::foundation;
namespace config = ludus::foundation::config;

namespace
{
constexpr uint32 MASTER_BUS = 0;
constexpr uint32 MUSIC_BUS = 1;
constexpr uint32 EFFECTS_BUS = 2;
constexpr usize BUS_COUNT = 3;
constexpr AudioMixBuses ROLES{ .Master = MASTER_BUS, .Music = MUSIC_BUS, .Effects = EFFECTS_BUS };

config::Explanation Explain(const config::Context& context, std::string_view name)
{
    config::Binding binding;
    config::Explanation explanation;
    REQUIRE(context.Bind(name, binding) == config::Status::Ok);
    REQUIRE(context.Explain(binding, explanation) == config::Status::Ok);
    return explanation;
}

config::Status
Set(config::Context& inoutContext, config::Layer layer, std::string_view name, const config::Value& value)
{
    const config::Assignment edit{ .Name = name, .Data = value };
    config::Diagnostic error;
    return inoutContext.Prepare(layer, {&edit, 1}, false, inoutContext.Revision(), error);
}

void Startup(config::Context& outContext, float32 master)
{
    config::Diagnostic error;
    REQUIRE(outContext.Initialize(AudioSchema(), GetSystemAllocationDomain(), error) == config::Status::Ok);
    REQUIRE(Set(outContext, config::Layer::Preference, "audio.volume.master", config::Value::FromFloat32(master)) ==
            config::Status::Ok);
    REQUIRE(outContext.Commit(outContext.Revision(), error) == config::Status::Ok);
}

void StartSession(AudioSystem& outSystem, const config::Context& context)
{
    AudioSessionOptions session;
    REQUIRE(ReadAudioSessionOptions(context, session) == config::Status::Ok);
    static const StaticArray<BusConfig, BUS_COUNT> BUSES{
        BusConfig{ .Id = 0, .ParentIndex = kNoParent },
        BusConfig{ .Id = 1, .ParentIndex = MASTER_BUS },
        BusConfig{ .Id = 2, .ParentIndex = MASTER_BUS },
    };
    SystemConfig settings;
    settings.SystemMode = Mode::Offline;
    settings.SampleRate = session.SampleRate;
    settings.Buses = {BUSES.GetData(), BUSES.GetSize()};
    REQUIRE(outSystem.Initialize(settings) == Status::Ok);
}

void RenderBoundary(AudioSystem& inoutSystem)
{
    StaticArray<float32, static_cast<usize>(CONTROL_QUANTUM_FRAMES) * 2> output{};
    REQUIRE(inoutSystem.RenderOffline({output.GetData(), output.GetSize()},
                                      ChannelLayout::Stereo,
                                      BufferLayout::Interleaved,
                                      CONTROL_QUANTUM_FRAMES) == Status::Ok);
}

float32 TargetGain(const AudioSystem& system, uint32 bus)
{
    BusMeter meter;
    REQUIRE(system.GetBusMeter(bus, meter) == Status::Ok);
    return meter.TargetGain;
}

/// A started session whose startup mix the renderer has acknowledged.
struct Fixture final
{
    config::Context Settings;
    AudioSystem Audio;
    AudioMixOwner Mix;
    explicit Fixture(float32 master = 0.5F)
    {
        Startup(Settings, master);
        StartSession(Audio, Settings);
        REQUIRE(Mix.Initialize(Settings, Audio, ROLES) == MixStatus::Ok);
        REQUIRE(Settings.SealStartup() == config::Status::Ok);
        RenderBoundary(Audio);
        Mix.Service();
        REQUIRE(Mix.GetReport().HasAcknowledgedMix == true);
    }
    MixStatus Change(config::Layer layer, std::string_view name, const config::Value& value)
    {
        const uint64 revision = Settings.Revision();
        REQUIRE(Set(Settings, layer, name, value) == config::Status::Ok);
        config::Diagnostic error;
        return Mix.Apply(revision, error);
    }
};
} // namespace

TEST_CASE("The audio schema is valid and reads its module-owned defaults", "[audio][configuration]")
{
    config::Context context;
    config::Diagnostic error;
    REQUIRE(context.Initialize(AudioSchema(), GetSystemAllocationDomain(), error) == config::Status::Ok);
    AudioMixOptions mix{ .MasterVolume = 0.0F };
    REQUIRE(ReadAudioMixOptions(context, mix) == config::Status::Ok);
    CHECK(mix == AudioMixOptions{});
    AudioSessionOptions session{ .SampleRate = 0 };
    REQUIRE(ReadAudioSessionOptions(context, session) == config::Status::Ok);
    CHECK(session.SampleRate == DEFAULT_SAMPLE_RATE);
    for (const config::Descriptor& descriptor : AudioSchema())
    {
        CHECK(descriptor.Persistent == true);
        CHECK((descriptor.Group == AUDIO_MIX_GROUP) == (descriptor.Application == config::Apply::Live));
    }
    CHECK(Set(context, config::Layer::Preference, "audio.volume.music", config::Value::FromFloat32(1.5F)) ==
          config::Status::InvalidValue);
    CHECK(Set(context, config::Layer::Preference, "audio.session.sample_rate", config::Value::FromUint32(1000)) ==
          config::Status::InvalidValue);
}

TEST_CASE("Startup publishes the active mix and reports it only after acknowledgement", "[audio][configuration]")
{
    config::Context context;
    AudioSystem system;
    AudioMixOwner owner;
    Startup(context, 0.5F);
    StartSession(system, context);
    REQUIRE(owner.Initialize(context, system, ROLES) == MixStatus::Ok);
    CHECK(owner.Initialize(context, system, ROLES) == MixStatus::InvalidState);
    AudioMixReport report = owner.GetReport();
    CHECK(report.Published.MasterVolume == 0.5F);
    CHECK(report.IsAwaitingAcknowledgement == true);
    CHECK(report.HasAcknowledgedMix == false);

    owner.Service();
    CHECK(owner.GetReport().HasAcknowledgedMix == false);
    RenderBoundary(system);
    owner.Service();
    report = owner.GetReport();
    CHECK(report.HasAcknowledgedMix == true);
    CHECK(report.IsAwaitingAcknowledgement == false);
    CHECK(report.Applied.MasterVolume == 0.5F);
    CHECK(report.AppliedGeneration == report.PublishedGeneration);
    CHECK(TargetGain(system, MASTER_BUS) == 0.5F);
}

TEST_CASE("A live mix change publishes at the safe point and retires the old snapshot on acknowledgement",
          "[audio][configuration]")
{
    Fixture fixture;
    const AudioMixReport before = fixture.Mix.GetReport();
    REQUIRE(fixture.Change(config::Layer::Session, "audio.volume.music", config::Value::FromFloat32(0.25F)) ==
            MixStatus::Ok);
    CHECK(Explain(fixture.Settings, "audio.volume.music").Active.Real == 0.25);

    // The renderer has not reached a boundary: the previous snapshot remains the applied mix.
    AudioMixReport report = fixture.Mix.GetReport();
    CHECK(report.IsAwaitingAcknowledgement == true);
    CHECK(report.Published.MusicVolume == 0.25F);
    CHECK(report.PublishedGeneration == before.PublishedGeneration + 1);
    CHECK(report.Applied == before.Applied);
    CHECK(report.AppliedGeneration == before.AppliedGeneration);
    CHECK(TargetGain(fixture.Audio, MUSIC_BUS) == 1.0F); // each bus target carries its own user gain

    RenderBoundary(fixture.Audio);
    fixture.Mix.Service();
    report = fixture.Mix.GetReport();
    CHECK(report.IsAwaitingAcknowledgement == false);
    CHECK(report.Applied.MusicVolume == 0.25F);
    CHECK(report.AppliedGeneration == report.PublishedGeneration);
    CHECK(TargetGain(fixture.Audio, MUSIC_BUS) == 0.25F);
    CHECK(TargetGain(fixture.Audio, MASTER_BUS) == 0.5F);
}

TEST_CASE("Failed preparation preserves the context, the applied mix and the renderer gains", "[audio][configuration]")
{
    Fixture fixture;
    const AudioMixReport before = fixture.Mix.GetReport();
    const auto expectPreserved = [&](const char* name) {
        INFO(name);
        CHECK(Explain(fixture.Settings, "audio.volume.effects").Active.Real == 1.0);
        CHECK(Explain(fixture.Settings, "audio.volume.effects").IsDefault == true);
        const AudioMixReport report = fixture.Mix.GetReport();
        CHECK(report.Applied == before.Applied);
        CHECK(report.Published == before.Published);
        CHECK(report.PublishedGeneration == before.PublishedGeneration);
        CHECK(TargetGain(fixture.Audio, EFFECTS_BUS) == 1.0F);
        // The candidate was discarded, so the caller can prepare its retained draft again.
        REQUIRE(
            Set(fixture.Settings, config::Layer::Session, "audio.volume.effects", config::Value::FromFloat32(0.75F)) ==
            config::Status::Ok);
        fixture.Settings.Discard();
    };

    SECTION("no renderer command room")
    {
        Command listener{};
        listener.Kind = CommandKind::SetListener;
        while (fixture.Audio.GetCommandRoom() >= 1)
        {
            REQUIRE(fixture.Audio.TrySubmitBatch({&listener, 1}).Result == Status::Ok);
        }
        CHECK(fixture.Change(config::Layer::Session, "audio.volume.effects", config::Value::FromFloat32(0.75F)) ==
              MixStatus::QueueFull);
        expectPreserved("queue full");
    }
    SECTION("audio session not ready")
    {
        REQUIRE(fixture.Audio.BeginShutdown() == Status::Ok);
        CHECK(fixture.Change(config::Layer::Session, "audio.volume.effects", config::Value::FromFloat32(0.75F)) ==
              MixStatus::AudioNotReady);
        CHECK(Explain(fixture.Settings, "audio.volume.effects").IsDefault == true);
        CHECK(fixture.Mix.GetReport().Published == before.Published);
    }
    SECTION("previous publication still awaiting acknowledgement")
    {
        REQUIRE(fixture.Change(config::Layer::Session, "audio.volume.master", config::Value::FromFloat32(0.4F)) ==
                MixStatus::Ok);
        const AudioMixReport pending = fixture.Mix.GetReport();
        CHECK(fixture.Change(config::Layer::Session, "audio.volume.effects", config::Value::FromFloat32(0.75F)) ==
              MixStatus::AwaitingAcknowledgement);
        CHECK(Explain(fixture.Settings, "audio.volume.effects").IsDefault == true);
        CHECK(fixture.Mix.GetReport().Published == pending.Published);
        RenderBoundary(fixture.Audio);
        fixture.Mix.Service();
        CHECK(fixture.Change(config::Layer::Session, "audio.volume.effects", config::Value::FromFloat32(0.75F)) ==
              MixStatus::Ok);
    }
    SECTION("stale context revision")
    {
        const uint64 stale = fixture.Settings.Revision() - 1;
        REQUIRE(
            Set(fixture.Settings, config::Layer::Session, "audio.volume.effects", config::Value::FromFloat32(0.75F)) ==
            config::Status::Ok);
        config::Diagnostic error;
        CHECK(fixture.Mix.Apply(stale, error) == MixStatus::ConfigurationFailed);
        CHECK(error.Result == config::Status::Conflict);
        expectPreserved("stale revision");
    }
    SECTION("no prepared transaction")
    {
        config::Diagnostic error;
        CHECK(fixture.Mix.Apply(fixture.Settings.Revision(), error) == MixStatus::InvalidState);
        expectPreserved("nothing prepared");
    }
}

TEST_CASE("Restart-only audio settings stay pending until a fresh session", "[audio][configuration]")
{
    Fixture fixture;
    const AudioMixReport before = fixture.Mix.GetReport();
    REQUIRE(fixture.Change(config::Layer::Preference, "audio.session.sample_rate", config::Value::FromUint32(44100)) ==
            MixStatus::PendingRestart);
    const config::Explanation rate = Explain(fixture.Settings, "audio.session.sample_rate");
    CHECK(rate.Pending == true);
    CHECK(rate.Requested.Unsigned == 44100);
    CHECK(rate.Active.Unsigned == DEFAULT_SAMPLE_RATE);
    AudioSessionOptions session;
    REQUIRE(ReadAudioSessionOptions(fixture.Settings, session) == config::Status::Ok);
    CHECK(session.SampleRate == DEFAULT_SAMPLE_RATE);
    CHECK(fixture.Audio.GetSampleRate() == DEFAULT_SAMPLE_RATE);
    CHECK(fixture.Mix.GetReport().IsAwaitingAcknowledgement == false);
    CHECK(fixture.Mix.GetReport().Published == before.Published);

    // One sealed transaction may not change the live and restart groups together.
    const config::Assignment mixed[]{
        { .Name = "audio.volume.master", .Data = config::Value::FromFloat32(0.1F) },
        { .Name = "audio.session.sample_rate", .Data = config::Value::FromUint32(32000) },
    };
    config::Diagnostic error;
    CHECK(fixture.Settings.Prepare(config::Layer::Session, mixed, false, fixture.Settings.Revision(), error) ==
          config::Status::MixedApplyGroups);

    // Restart: retire the old owner and session, then rebuild from the persisted preference layer.
    config::Value persisted;
    config::Origin origin;
    config::Binding binding;
    REQUIRE(fixture.Settings.Bind("audio.session.sample_rate", binding) == config::Status::Ok);
    REQUIRE(fixture.Settings.ReadLayer(binding, config::Layer::Preference, persisted, origin) == config::Status::Ok);
    REQUIRE(fixture.Audio.BeginShutdown() == Status::Ok);

    config::Context restarted;
    Startup(restarted, 0.5F);
    REQUIRE(Set(restarted, config::Layer::Preference, "audio.session.sample_rate", persisted) == config::Status::Ok);
    REQUIRE(restarted.Commit(restarted.Revision(), error) == config::Status::Ok);
    AudioSystem system;
    StartSession(system, restarted);
    CHECK(system.GetSampleRate() == 44100);
    AudioMixOwner owner;
    REQUIRE(owner.Initialize(restarted, system, ROLES) == MixStatus::Ok);
    RenderBoundary(system);
    owner.Service();
    CHECK(owner.GetReport().Applied.MasterVolume == 0.5F);
}

TEST_CASE("Provenance-only and unmapped changes commit without a renderer publication", "[audio][configuration]")
{
    config::Context context;
    AudioSystem system;
    AudioMixOwner owner;
    Startup(context, 0.5F);
    StartSession(system, context);
    CHECK(owner.Initialize(context, system, {}) == MixStatus::InvalidBus);
    CHECK(owner.Initialize(context, system, { .Master = MASTER_BUS, .Music = 7 }) == MixStatus::InvalidBus);
    CHECK(owner.IsInitialized() == false);
    REQUIRE(owner.Initialize(context, system, { .Master = MASTER_BUS }) == MixStatus::Ok);
    REQUIRE(context.SealStartup() == config::Status::Ok);
    RenderBoundary(system);
    owner.Service();
    const uint64 sequence = system.GetAppliedSequence();

    // The same value from a higher rank changes provenance, not the active mix.
    uint64 revision = context.Revision();
    REQUIRE(Set(context, config::Layer::Launch, "audio.volume.master", config::Value::FromFloat32(0.5F)) ==
            config::Status::Ok);
    config::Diagnostic error;
    CHECK(owner.Apply(revision, error) == MixStatus::Ok);
    CHECK(Explain(context, "audio.volume.master").Winner == config::Layer::Launch);
    CHECK(owner.GetReport().IsAwaitingAcknowledgement == false);

    // An unmapped role still publishes the whole snapshot, but only mapped buses receive commands.
    revision = context.Revision();
    REQUIRE(Set(context, config::Layer::Session, "audio.volume.music", config::Value::FromFloat32(0.2F)) ==
            config::Status::Ok);
    CHECK(owner.Apply(revision, error) == MixStatus::Ok);
    RenderBoundary(system);
    owner.Service();
    CHECK(system.GetAppliedSequence() == sequence + 1);
    CHECK(owner.GetReport().Applied.MusicVolume == 0.2F);
    CHECK(TargetGain(system, MUSIC_BUS) == 1.0F);
}

TEST_CASE("Mix status names are stable", "[audio][configuration]")
{
    CHECK(MixStatusName(MixStatus::Ok) == "Ok");
    CHECK(MixStatusName(MixStatus::AwaitingAcknowledgement) == "AwaitingAcknowledgement");
    CHECK(MixStatusName(MixStatus::ConfigurationFailed) == "ConfigurationFailed");
}
