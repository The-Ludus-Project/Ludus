#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/audio/content/definitions.h>

#include <QObject>
#include <QString>
#include <QVector>

namespace ludus::editor
{
struct ContentImportSource;

class AudioPreview final : public QObject
{
    Q_OBJECT
public:
    explicit AudioPreview(QObject* parent = nullptr);
    ~AudioPreview() override;
    void Play(const QString& root, const audio::content::Sound& sound);
    void Play(const QString& root, const audio::content::Music& music);
    void Import(const ContentImportSource& source);
    void Stop();
    void Shutdown();
    [[nodiscard]] bool Reset();
    [[nodiscard]] bool Finished() const noexcept;
Q_SIGNALS:
    void Message(const QString& message);
    void Waveform(const QString& root, const QVector<foundation::float32>& peaks, foundation::uint64 frames);
    void Meter(foundation::float32 peak, foundation::uint64 starvations);
    void Imported(const QString& root);

private:
    [[nodiscard]] bool start();
    struct Impl;
    Impl* Impl_ = nullptr;
};
} // namespace ludus::editor
