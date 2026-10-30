#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/texture_inspection.h"

#include <QElapsedTimer>
#include <QObject>

namespace ludus::editor
{
/// Private conversion boundary. A custom function/context must outlive all accepted work,
/// including cancellation after destruction of the owner. Widgets and RHI objects are forbidden.
using TextureDisplayConversionFunction = QImage (*)(const InspectedTexture& texture,
                                                    const TextureDisplaySettings& settings,
                                                    TextureValueRange& outRange,
                                                    const TextureConversionCancellation& cancellation,
                                                    void* contextOrNull);

/// An immutable converted snapshot, published only on the GUI thread.
struct TextureDisplayConversionResult final
{
    InspectedTexture Texture;        ///< Owned raw bytes paired with Image, including for pixel probes.
    TextureDisplaySettings Settings; ///< Controls captured for this request.
    QImage Image;                    ///< Null on failure; a caller may preserve its last drawable image.
    TextureValueRange Range;         ///< Range applied to Image; default when conversion failed.
    /// Active worker-call duration, including scheduling preemption, in milliseconds.
    foundation::float64 WorkMilliseconds = 0;
    /// Request through GUI completion delivery, including queue delay, in milliseconds.
    foundation::float64 ElapsedMilliseconds = 0;
};

class TextureDisplayWorker;

/// One native conversion at a time, with one replaceable latest request.
/// A lazy worker is reused and sleeps between requests.
/// GUI-thread owner. Requests retain immutable CPU bytes, never a viewer or GPU resource.
/// Cancellation is cooperative between rows. Destruction cancels and disconnects without
/// joining; a retiring worker owns its inputs and deletes itself after finishing.
class TextureDisplayConversion final : public QObject
{
    Q_OBJECT
public:
    explicit TextureDisplayConversion(QObject* parentOrNull = nullptr,
                                      TextureDisplayConversionFunction convertOrNull = nullptr,
                                      void* contextOrNull = nullptr);
    ~TextureDisplayConversion() override;
    /// Replace queued work and cancel a superseded conversion. Never converts or waits here.
    void Request(const InspectedTexture& texture, const TextureDisplaySettings& settings);
    /// Cancel active/queued work. Preserves the last accepted result and never joins.
    void Cancel() noexcept;
    [[nodiscard]] bool IsBusy() const noexcept;
    /// Valid until the next Completed signal. Cancelled/stale results are never published.
    [[nodiscard]] const TextureDisplayConversionResult& GetResult() const noexcept;

Q_SIGNALS:
    void Completed();

private:
    void startPending();
    void finish(TextureDisplayWorker* worker);

    TextureDisplayConversionFunction mConvert;
    void* mContextOrNull = nullptr;
    TextureDisplayWorker* mWorkerOrNull = nullptr;
    bool mIsBusy = false;
    foundation::uint64 mRequestSerial = 0;
    bool mHasPending = false;
    InspectedTexture mPendingTexture;
    TextureDisplaySettings mPendingSettings;
    QElapsedTimer mPendingTimer;
    TextureDisplayConversionResult mResult;
};
} // namespace ludus::editor
