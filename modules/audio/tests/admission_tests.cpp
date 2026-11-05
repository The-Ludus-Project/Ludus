// Admission / validation / transactional rollback tests via the production
// public path (design section 4; tasks A1 gate rows). No device, worker,
// Platform or window.

#include <ludus/audio/audio_system.h>
#include <ludus/foundation/containers/array.hpp>

#include "wav_fixture.h"

#include <catch2/catch_test_macros.hpp>

using namespace ludus::audio;
using ludus::foundation::Array;

namespace
{
void InitOffline(AudioSystem& sys, uint32 logical = 0, std::span<const GroupConfig> groups = {})
{
    SystemConfig cfg{};
    cfg.SystemMode = Mode::Offline;
    cfg.SampleRate = 48000;
    cfg.LogicalVoiceCapacity = logical;
    cfg.Groups = groups;
    const Status s = sys.Initialize(cfg);
    REQUIRE(IsOk(s));
}

ClipHandle PrepareSine(AudioSystem& sys, uint16 channels = 1, uint32 frames = 480)
{
    auto wav = test::MakeSineWav(48000, channels, frames);
    ClipHandle h{};
    ClipDescriptor d{};
    d.Format = SourceFormat::Wav;
    const Status s = sys.PrepareClip(std::span<const uint8>(wav.data(), wav.size()), d, h);
    REQUIRE(IsOk(s));
    REQUIRE(h.IsValid());
    return h;
}
} // namespace

TEST_CASE("Fabricated / cross-system / stale handles are rejected safely", "[audio][admission]")
{
    AudioSystem sys;
    InitOffline(sys);
    VoiceInfo info{};
    REQUIRE(sys.GetVoiceInfo(VoiceHandle{}, info) == Status::InvalidHandle);
    REQUIRE(sys.GetVoiceInfo(VoiceHandle{999, 5, 7}, info) == Status::InvalidHandle);
    REQUIRE(sys.Stop(VoiceHandle{999, 5, 7}) == Status::InvalidHandle);

    // A clip handle from a different (fabricated) session cannot play.
    PlayParams p{};
    p.Clip = ClipHandle{12345, 0, 1};
    VoiceHandle v{};
    REQUIRE(sys.PlayClip(p, v) == Status::InvalidHandle);
}

TEST_CASE("Handles from an ended audio session are rejected by the next session",
          "[audio][admission][native-audio-worker]")
{
    // Audio keeps its own uint32 session source (docs/architecture/object-handles-uuid.md,
    // stage 2). Each session restarts slots and generations, so the session field
    // alone separates these handles from the successor's identical slot values.
    const Array<uint8> wav = test::MakeSineWav(48000, 1, 4800);
    const ModifierValue modifierValue{ .BusIndex = 0, .TargetDb = -6.0F };
    uint32 oldSession = 0;
    ClipHandle oldClip{};
    VoiceHandle oldVoice{};
    StreamHandle oldStream{};
    ModifierHandle oldModifier{};
    {
        AudioSystem first;
        InitOffline(first);
        oldSession = first.GetSession();
        oldClip = PrepareSine(first);
        PlayParams play{};
        play.Clip = oldClip;
        REQUIRE(IsOk(first.PlayClip(play, oldVoice)));
        REQUIRE(
            IsOk(first.PrepareStream(std::span<const uint8>(wav.data(), wav.size()), StreamDescriptor{}, oldStream)));
        REQUIRE(IsOk(first.AcquireModifier(std::span<const ModifierValue>(&modifierValue, 1), 1.0F, oldModifier)));
    }

    AudioSystem second;
    InitOffline(second);
    REQUIRE(second.GetSession() != 0);
    REQUIRE(second.GetSession() != oldSession);
    const ClipHandle clip = PrepareSine(second);
    PlayParams play{};
    play.Clip = clip;
    VoiceHandle voice{};
    REQUIRE(IsOk(second.PlayClip(play, voice)));
    StreamHandle stream{};
    REQUIRE(IsOk(second.PrepareStream(std::span<const uint8>(wav.data(), wav.size()), StreamDescriptor{}, stream)));
    ModifierHandle modifier{};
    REQUIRE(IsOk(second.AcquireModifier(std::span<const ModifierValue>(&modifierValue, 1), 1.0F, modifier)));
    REQUIRE(clip.Slot == oldClip.Slot);
    REQUIRE(clip.Generation == oldClip.Generation);
    REQUIRE(voice.Slot == oldVoice.Slot);
    REQUIRE(voice.Generation == oldVoice.Generation);
    REQUIRE(stream.Slot == oldStream.Slot);
    REQUIRE(stream.Generation == oldStream.Generation);
    REQUIRE(modifier.Slot == oldModifier.Slot);
    REQUIRE(modifier.Generation == oldModifier.Generation);

    REQUIRE_FALSE(second.IsClipReady(oldClip));
    PlayParams stalePlay{};
    stalePlay.Clip = oldClip;
    VoiceHandle rejected{};
    REQUIRE(second.PlayClip(stalePlay, rejected) == Status::InvalidHandle);
    REQUIRE_FALSE(rejected.IsValid());
    REQUIRE(second.RetireClip(oldClip) == Status::InvalidHandle);
    VoiceInfo info{};
    REQUIRE(second.GetVoiceInfo(oldVoice, info) == Status::InvalidHandle);
    REQUIRE(second.Stop(oldVoice) == Status::InvalidHandle);
    REQUIRE(second.PlayStream(oldStream, 0, 6, 1.0F, rejected) == Status::InvalidHandle);
    REQUIRE_FALSE(rejected.IsValid());
    REQUIRE(second.RetireStream(oldStream) == Status::InvalidHandle);
    REQUIRE(second.UpdateModifier(oldModifier, 0.5F) == Status::InvalidHandle);
    REQUIRE(second.ReleaseModifier(oldModifier) == Status::InvalidHandle);

    // The successor's live objects in the same slots were not touched.
    REQUIRE(second.IsClipReady(clip));
    REQUIRE(IsOk(second.GetVoiceInfo(voice, info)));
    REQUIRE(IsOk(second.UpdateModifier(modifier, 0.5F)));
    REQUIRE(IsOk(second.RetireStream(stream)));
    REQUIRE(IsOk(second.ReleaseModifier(modifier)));
    REQUIRE(IsOk(second.Stop(voice)));
}

TEST_CASE("An invalid last record rejects the whole batch with no leaked pins", "[audio][admission]")
{
    AudioSystem sys;
    InitOffline(sys);
    ClipHandle clip = PrepareSine(sys);

    SystemSnapshot before{};
    sys.GetSystemSnapshot(before);

    Command cmds[2]{};
    cmds[0].Kind = CommandKind::Play;
    cmds[0].Play.Clip = clip;
    cmds[1].Kind = CommandKind::Play;
    cmds[1].Play.Clip = clip;
    cmds[1].Play.Priority = 99; // invalid priority => reject whole batch

    const BatchResult r = sys.TrySubmitBatch(std::span<const Command>(cmds, 2));
    REQUIRE(r.Result == Status::InvalidArgument);
    REQUIRE(r.Voices.empty());

    SystemSnapshot after{};
    sys.GetSystemSnapshot(after);
    REQUIRE(after.PendingVoices == 0); // no earlier play applied
}

TEST_CASE("Oversized batch is rejected", "[audio][admission]")
{
    AudioSystem sys;
    InitOffline(sys);
    ClipHandle clip = PrepareSine(sys);
    Array<Command> big(MAX_BATCH_RECORDS + 1);
    for (auto& c : big)
    {
        c.Kind = CommandKind::Play;
        c.Play.Clip = clip;
    }
    const BatchResult r = sys.TrySubmitBatch(std::span<const Command>(big.data(), big.size()));
    REQUIRE(r.Result == Status::InvalidArgument);
}

TEST_CASE("Group MaxAdmitted is transactional and returns GroupCapacity", "[audio][admission]")
{
    GroupConfig groups[1]{};
    groups[0].MaxAdmitted = 2;
    groups[0].MaxSelected = 2;
    AudioSystem sys;
    InitOffline(sys, 0, std::span<const GroupConfig>(groups, 1));
    ClipHandle clip = PrepareSine(sys);

    // Two plays fit.
    VoiceHandle v{};
    PlayParams p{};
    p.Clip = clip;
    REQUIRE(IsOk(sys.PlayClip(p, v)));
    REQUIRE(IsOk(sys.PlayClip(p, v)));

    // A third exceeds MaxAdmitted.
    REQUIRE(sys.PlayClip(p, v) == Status::GroupCapacity);

    GroupInfo gi{};
    REQUIRE(IsOk(sys.GetGroupInfo(0, gi)));
    REQUIRE(gi.Admitted == 2);

    // A batch of two where the second overflows the group must roll back both.
    Command cmds[2]{};
    cmds[0].Kind = CommandKind::Play;
    cmds[0].Play.Clip = clip; // would be the 3rd admitted (already full)
    cmds[1].Kind = CommandKind::Play;
    cmds[1].Play.Clip = clip;
    const BatchResult r = sys.TrySubmitBatch(std::span<const Command>(cmds, 2));
    REQUIRE(r.Result == Status::GroupCapacity);
    REQUIRE(IsOk(sys.GetGroupInfo(0, gi)));
    REQUIRE(gi.Admitted == 2); // unchanged
}

TEST_CASE("Disabled mode never creates a phantom voice", "[audio][admission]")
{
    AudioSystem sys;
    SystemConfig cfg{};
    cfg.SystemMode = Mode::Disabled;
    cfg.SampleRate = 48000;
    REQUIRE(IsOk(sys.Initialize(cfg)));

    PlayParams p{};
    p.Clip = ClipHandle{sys.GetSession(), 0, 1};
    VoiceHandle v{};
    const Status s = sys.PlayClip(p, v);
    REQUIRE((s == Status::Disabled || s == Status::InvalidHandle));
    SystemSnapshot snap{};
    sys.GetSystemSnapshot(snap);
    REQUIRE(snap.PendingVoices == 0);
    REQUIRE(snap.RenderFrame == 0);
}

TEST_CASE("Invalid config and bus topology are rejected before mutation", "[audio][admission]")
{
    // Cyclic/forward parent reference is rejected.
    BusConfig buses[2]{};
    buses[0].Id = 0;
    buses[0].ParentIndex = 1; // parent appears later => invalid (not acyclic precompute)
    buses[1].Id = 1;
    buses[1].ParentIndex = kNoParent;
    AudioSystem sys;
    SystemConfig cfg{};
    cfg.SystemMode = Mode::Offline;
    cfg.SampleRate = 48000;
    cfg.Buses = std::span<const BusConfig>(buses, 2);
    REQUIRE(sys.Initialize(cfg) == Status::InvalidArgument);

    // Zero sample rate rejected.
    AudioSystem sys2;
    SystemConfig cfg2{};
    cfg2.SampleRate = 0;
    REQUIRE(sys2.Initialize(cfg2) == Status::InvalidArgument);
}
