#pragma once
#include "file_input.hpp"
#include <ludus/audio/content/music_score.h>

#include <atomic>

namespace ludus::audio::content::internal
{
struct ScoreLayer final
{
    FileInput Input;
    SourceFormat Format = SourceFormat::Wav;
};

/// Cold revision storage. Open descriptors pin the exact files inspected on the worker.
struct ScoreRevision final
{
    MusicScore Score;
    ludus::foundation::StaticArray<ludus::foundation::StaticArray<ScoreLayer, STREAM_CAPACITY>, MAX_MUSIC_SECTIONS>
        Layers;

    [[nodiscard]] ContentStatus
    Open(std::string_view root, const ludus::content::Catalog& catalog, const std::atomic<bool>& isCancelled) noexcept
    {
        for (usize i = 0; i < Score.SectionCount; ++i)
        {
            const MusicSection& section = Score.Sections[i];
            SourceInfo first;
            for (usize j = 0; j < section.LayerCount; ++j)
            {
                if (isCancelled.load(std::memory_order_acquire) == true)
                {
                    return ContentStatus::Cancelled;
                }
                const ludus::content::Resource* resourceOrNull = catalog.Find(section.Layers[j].Source.View());
                if (resourceOrNull == nullptr || resourceOrNull->Type != ludus::content::Kind::AudioSource)
                {
                    return ContentStatus::NotFound;
                }
                ScoreLayer& layer = Layers[i][j];
                const std::string_view path = resourceOrNull->Path.View();
                layer.Format = path.ends_with(".flac") ? SourceFormat::Flac : SourceFormat::Wav;
                const ContentStatus result = layer.Input.File.Open(root, path);
                if (result != ContentStatus::Ok)
                {
                    return result;
                }
                SourceInfo info;
                if (InspectSource(layer.Input, layer.Format, info) != Status::Ok || section.LoopEnd > info.Frames ||
                    (j != 0 && (info.Frames != first.Frames || info.SampleRate != first.SampleRate)))
                {
                    return ContentStatus::Invalid;
                }
                first = info;
            }
        }
        for (usize i = 0; i < Score.StingerCount; ++i)
        {
            const ludus::content::Resource* soundOrNull = catalog.Find(Score.Stingers[i].Sound.View());
            if (soundOrNull == nullptr || soundOrNull->Type != ludus::content::Kind::Sound)
            {
                return ContentStatus::NotFound;
            }
        }
        return ContentStatus::Ok;
    }
};
} // namespace ludus::audio::content::internal
