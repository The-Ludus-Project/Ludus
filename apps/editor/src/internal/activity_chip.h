#pragma once

// Right side of the status bar: what the Editor is doing now and how many problems there are
// (docs/architecture/editor-art-direction.md, status bar). The running job shows as a static dot, or a
// determinate bar when its source reports progress; nothing animates and nothing pops up. The Problems and
// Warnings counts open the Error List.

#include "internal/workspace.h"

#include <ludus/foundation/base/types.h>

#include <QElapsedTimer>
#include <QString>
#include <QTimer>
#include <QWidget>

class QToolButton;

namespace ludus::editor
{
/// Shortest interval between two changes of what the chip shows while a job runs: 10 Hz.
inline constexpr int ACTIVITY_UPDATE_INTERVAL_MILLISECONDS = 100;

/// What the chip says for the Editor's current work, in priority order: the project operation (build, configure,
/// launch...), a render-graph job, then a content import. Empty when nothing is running.
[[nodiscard]] QString GetActivityLabel(Phase phase, bool isImporting, bool isGraphJobActive);

/// "1 Problem" or "3 Problems": count followed by singular or plural.
[[nodiscard]] QString FormatProblemCount(foundation::int32 count, const QString& singular, const QString& plural);

/// The status bar's right side. UI-thread only.
class StatusActivityBar final : public QWidget
{
    Q_OBJECT
public:
    explicit StatusActivityBar(QWidget* parentOrNull = nullptr);

    /// Show label with a static dot, or with a bar filled to progressOrNegative (0 to 1) when it is not negative.
    /// An empty label hides the job part. Changes are applied at most every ACTIVITY_UPDATE_INTERVAL_MILLISECONDS.
    void SetActivity(const QString& label, double progressOrNegative);
    /// Show the problem counts, applied together with the activity.
    void SetCounts(foundation::int32 errors, foundation::int32 warnings);
    /// Apply any pending change now.
    void Flush();

    /// How many times the shown state changed; tests use it to prove the rate limit.
    [[nodiscard]] foundation::int32 GetAppliedChangeCount() const noexcept
    {
        return mAppliedChangeCount;
    }
    [[nodiscard]] QString GetActivityText() const;
    [[nodiscard]] bool IsActivityShown() const;
    /// Fill of the shown bar, or a negative value for a dot.
    [[nodiscard]] double GetShownProgress() const noexcept
    {
        return mShownProgress;
    }

Q_SIGNALS:
    /// The user asked to see the Error List.
    void ProblemsRequested();

private:
    void schedule();
    void applyPending();
    QWidget* mMarker;
    QWidget* mActivityGroup;
    QToolButton* mProblems;
    QToolButton* mWarnings;
    QTimer mTimer;
    QElapsedTimer mSinceApplied;
    QString mPendingLabel;
    double mPendingProgress = -1.0;
    foundation::int32 mPendingErrors = 0;
    foundation::int32 mPendingWarnings = 0;
    double mShownProgress = -1.0;
    foundation::int32 mAppliedChangeCount = 0;
    bool mHasPending = false;
};
} // namespace ludus::editor
