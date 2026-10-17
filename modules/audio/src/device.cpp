#include "internal/impl.hpp"
#include <ludus/foundation/base/config.h>

#include <new>

#if LUDUS_TARGET_OS == LUDUS_OS_LINUX || LUDUS_TARGET_OS == LUDUS_OS_MACOS
#    include <pthread.h>
#    include <time.h>

#    include "miniaudio.h"
#endif

namespace ludus::audio
{
#if LUDUS_TARGET_OS == LUDUS_OS_LINUX || LUDUS_TARGET_OS == LUDUS_OS_MACOS
namespace
{
struct NativeDevice final
{
    ma_context Context{};
    // miniaudio initializes these C structs itself and expects them zeroed first.
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
    ma_device Device{};
    bool ContextReady = false;
    bool DeviceReady = false;
};
struct NativeWorker final
{
    pthread_t Thread{};
};
} // namespace
#endif
AudioSystem::Impl::~Impl() noexcept
{
    CloseDevice();
    CloseWorker();
    if (Control == nullptr)
    {
        for (auto& slot : Streams)
        {
            delete slot.Data;
            slot.Data = nullptr;
        }
    }
    ReleaseStorage();
}
Status AudioSystem::Impl::StartDevice() noexcept
{
#if LUDUS_TARGET_OS == LUDUS_OS_LINUX || LUDUS_TARGET_OS == LUDUS_OS_MACOS
    auto* native = new (std::nothrow) NativeDevice();
    auto* renderer = new (std::nothrow) Impl();
    if (native == nullptr || renderer == nullptr)
    {
        delete native;
        delete renderer;
        return Status::OutOfMemory;
    }
    renderer->Control = this;
    renderer->Config = Config;
    renderer->Session = Session;
    renderer->SampleRate = SampleRate;
    renderer->State = SystemState::Ready;
    renderer->SystemMode = Mode::Device;
    renderer->LogicalCapacity = LogicalCapacity;
    renderer->MixedCapacity = MixedCapacity;
    renderer->BusCount = BusCount;
    renderer->GroupCount = GroupCount;
    for (uint32 i = 0; i < BusCount; ++i)
    {
        renderer->Buses[i] = Buses[i];
    }
    for (uint32 i = 0; i < GroupCount; ++i)
    {
        renderer->Groups[i] = Groups[i];
    }
    if (!renderer->EnsurePhysicalInitialized())
    {
        delete renderer;
        delete native;
        return Status::OutOfMemory;
    }
    // Never permit miniaudio's null backend to certify real device playback.
#    if LUDUS_TARGET_OS == LUDUS_OS_MACOS
    // Thanks to David Reid, miniaudio Programming Manual, sections 1.1 and 17
    // (https://miniaud.io/docs/manual/index.html), for explicit backend selection
    // and owner-side device teardown. Ludus retains its own callback mixer.
    const ma_backend backends[] = {ma_backend_coreaudio};
    constexpr uint32 backendCount = 1;
#    else
    const ma_backend backends[] = {ma_backend_pulseaudio, ma_backend_alsa};
    constexpr uint32 backendCount = 2;
#    endif
    if (ma_context_init(backends, backendCount, nullptr, &native->Context) != MA_SUCCESS)
    {
        delete renderer;
        delete native;
        return Status::DeviceError;
    }
    native->ContextReady = true;
    auto config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = SampleRate;
    config.periodSizeInFrames = Config.DevicePeriodFrames;
    config.pUserData = renderer;
    config.dataCallback = [](ma_device* device, void* output, const void*, ma_uint32 frames) noexcept {
        auto* state = static_cast<Impl*>(device->pUserData);
        (void)state->RenderFrames({static_cast<float32*>(output), static_cast<usize>(frames) * 2},
                                  ChannelLayout::Stereo,
                                  BufferLayout::Interleaved,
                                  frames);
    };
    config.notificationCallback = [](const ma_device_notification* notification) noexcept {
        if (notification->type == ma_device_notification_type_stopped)
        {
            auto* rendererState = static_cast<Impl*>(notification->pDevice->pUserData);
            rendererState->Control->DeviceStopped.store(true, std::memory_order_release);
        }
    };
    const auto result = ma_device_init(&native->Context, &config, &native->Device);
    if (result != MA_SUCCESS)
    {
        ma_context_uninit(&native->Context);
        delete renderer;
        delete native;
        return Status::DeviceError;
    }
    native->DeviceReady = true;
    if (ma_device_start(&native->Device) != MA_SUCCESS)
    {
        ma_device_uninit(&native->Device);
        ma_context_uninit(&native->Context);
        delete renderer;
        delete native;
        return Status::DeviceError;
    }
    Renderer = renderer;
    Device = native;
    return Status::Ok;
#else
    return Status::Unsupported;
#endif
}
void AudioSystem::Impl::CloseDevice() noexcept
{
#if LUDUS_TARGET_OS == LUDUS_OS_LINUX || LUDUS_TARGET_OS == LUDUS_OS_MACOS
    auto* native = static_cast<NativeDevice*>(Device);
    if (native != nullptr)
    {
        if (native->DeviceReady)
        {
            ma_device_uninit(&native->Device);
        }
        if (native->ContextReady)
        {
            ma_context_uninit(&native->Context);
        }
        delete native;
        Device = nullptr;
        CollectRender();
        delete Renderer;
        Renderer = nullptr;
    }
#endif
}
Status AudioSystem::Impl::StartWorker() noexcept
{
    if (Worker != nullptr)
    {
        return Status::Ok;
    }
#if LUDUS_TARGET_OS == LUDUS_OS_LINUX || LUDUS_TARGET_OS == LUDUS_OS_MACOS
    auto* worker = new (std::nothrow) NativeWorker();
    if (worker == nullptr)
    {
        return Status::OutOfMemory;
    }
    WorkerStop.store(false, std::memory_order_relaxed);
    const int result = pthread_create(
        &worker->Thread,
        nullptr,
        [](void* input) -> void* {
            auto* state = static_cast<Impl*>(input);
            uint32 first = 0;
            while (!state->WorkerStop.load(std::memory_order_acquire))
            {
                // One bounded decode unit per instance, rotate tie order. Prioritize
                // smaller published runway before every round; never decode on renderer.
                internal::StreamData* ordered[STREAM_CAPACITY]{};
                uint32 count = 0;
                for (uint32 i = 0; i < STREAM_CAPACITY; ++i)
                {
                    auto* stream = state->WorkerStreams[(i + first) % STREAM_CAPACITY].load(std::memory_order_acquire);
                    if (stream == nullptr)
                    {
                        continue;
                    }
                    uint32 pos = count;
                    const auto runway =
                        stream->Written.load(std::memory_order_relaxed) - stream->Read.load(std::memory_order_acquire);
                    while (pos > 0)
                    {
                        const auto other = ordered[pos - 1]->Written.load(std::memory_order_relaxed) -
                                           ordered[pos - 1]->Read.load(std::memory_order_acquire);
                        if (other <= runway)
                        {
                            break;
                        }
                        ordered[pos] = ordered[pos - 1];
                        --pos;
                    }
                    ordered[pos] = stream;
                    ++count;
                }
                for (uint32 i = 0; i < count; ++i)
                {
                    auto* stream = ordered[i];
                    stream->Refill();
                    if (stream->Cancel.load(std::memory_order_acquire))
                    {
                        for (uint32 slot = 0; slot < STREAM_CAPACITY; ++slot)
                        {
                            if (state->WorkerStreams[slot].load(std::memory_order_relaxed) == stream)
                            {
                                state->WorkerStreams[slot].store(nullptr, std::memory_order_release);
                                break;
                            }
                        }
                    }
                }
                first = (first + 1) % STREAM_CAPACITY;
                const timespec delay{0, 1000000};
                nanosleep(&delay, nullptr);
            }
            // Owner frees only after pthread_join; remove the publication pointers then.
            return nullptr;
        },
        this);
    if (result != 0)
    {
        delete worker;
        return Status::IoError;
    }
    Worker = worker;
    return Status::Ok;
#else
    return Status::Unsupported;
#endif
}
void AudioSystem::Impl::CloseWorker() noexcept
{
#if LUDUS_TARGET_OS == LUDUS_OS_LINUX || LUDUS_TARGET_OS == LUDUS_OS_MACOS
    auto* worker = static_cast<NativeWorker*>(Worker);
    if (worker != nullptr)
    {
        WorkerStop.store(true, std::memory_order_release);
        // Thanks to Apple, pthread_join(3), DESCRIPTION
        // (https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/pthread_join.3.html):
        // joining the sole worker precedes reclaiming its published inputs.
        pthread_join(worker->Thread, nullptr);
        delete worker;
        Worker = nullptr;
    }
#endif
}
uint32 AudioSystem::Impl::FindStream(StreamHandle handle) const noexcept
{
    if (!handle.IsValid() || handle.Session != Session || handle.Slot >= StreamInstances)
    {
        return StreamInstances;
    }
    const auto& slot = Streams[handle.Slot];
    return slot.InUse && !slot.Retiring && slot.Generation == handle.Generation ? handle.Slot : StreamInstances;
}
void AudioSystem::Impl::PublishRender() noexcept
{
    Control->PublishedFrame.store(RenderFrame, std::memory_order_release);
    RenderStats stats;
    for (uint32 i = 0; i < BusCount; ++i)
    {
        stats.Buses[i] = Buses[i];
    }
    for (uint32 i = 0; i < GroupCount; ++i)
    {
        stats.Selected[i] = Groups[i].Selected;
        stats.Fading[i] = Groups[i].Fading;
    }
    stats.Starvations = StreamStarvations;
    stats.Eof = EofEvents;
    stats.Errors = Errors;
    stats.Stale = StaleCommands;
    stats.PreClip = PreClipFrames;
    stats.NonFinite = NonFiniteFaults;
    stats.Losses = SnapshotLosses;
    if (!Control->RenderMetrics.TryPublishBatch({&stats, 1}))
    {
        ++SnapshotLosses;
    }
    for (uint32 i = 0; i < LogicalCapacity; ++i)
    {
        const auto& v = Voices[i];
        if (!v.InUse)
        {
            continue;
        }
        const RenderRecord record{i, v.Generation.Value, v.State, v.CursorFrame, v.FadeGain, v.LastCause, v.Silence};
        if (!Control->RenderSnapshots.TryPublishBatch({&record, 1}))
        {
            ++SnapshotLosses;
            break;
        }
    }
}
void AudioSystem::Impl::CollectRender() noexcept
{
    if (SystemMode != Mode::Device)
    {
        return;
    }
    RenderStats stats;
    while (RenderMetrics.Peek(stats))
    {
        RenderMetrics.ConsumeOne();
        for (uint32 i = 0; i < BusCount; ++i)
        {
            Buses[i] = stats.Buses[i];
        }
        for (uint32 i = 0; i < GroupCount; ++i)
        {
            Groups[i].Selected = stats.Selected[i];
            Groups[i].Fading = stats.Fading[i];
        }
        StreamStarvations = stats.Starvations;
        EofEvents = stats.Eof;
        Errors = stats.Errors;
        StaleCommands = stats.Stale;
        PreClipFrames = stats.PreClip;
        NonFiniteFaults = stats.NonFinite;
        SnapshotLosses = stats.Losses;
    }
    RenderFrame = PublishedFrame.load(std::memory_order_acquire);
    RenderRecord record;
    while (RenderSnapshots.Peek(record))
    {
        RenderSnapshots.ConsumeOne();
        auto& voice = Voices[record.Slot];
        if (voice.InUse && voice.Generation.Value == record.Generation)
        {
            voice.State = record.State;
            voice.CursorFrame = record.Cursor;
            voice.FadeGain = record.Gain;
            voice.LastCause = record.Cause;
            voice.Silence = record.Silence;
        }
    }
}
} // namespace ludus::audio
