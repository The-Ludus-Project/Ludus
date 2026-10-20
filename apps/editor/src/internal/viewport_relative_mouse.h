#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <QAbstractNativeEventFilter>

namespace ludus::editor
{
class PreviewViewport;

/// GUI-thread macOS relative mouse capture. One viewport may own the process-wide
/// cursor association at a time; destruction and Stop restore it and remove the filter.
class ViewportRelativeMouse final : public QAbstractNativeEventFilter
{
public:
    explicit ViewportRelativeMouse(PreviewViewport& inoutViewport) noexcept;
    ~ViewportRelativeMouse() override;
    ViewportRelativeMouse(const ViewportRelativeMouse&) = delete;
    ViewportRelativeMouse& operator=(const ViewportRelativeMouse&) = delete;
    /// Disconnect physical motion from cursor position. Failure leaves normal bounded Qt input available.
    [[nodiscard]] bool TryBegin() noexcept;
    void Stop() noexcept;
    [[nodiscard]] LUDUS_INLINE bool IsActive() const noexcept
    {
        return mIsActive;
    }
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* resultOrNull) override;

private:
    PreviewViewport& mViewport;
    foundation::float64 mCaptureBeganSeconds = 0;
    bool mIsActive = false;
};
} // namespace ludus::editor
