#pragma once

// Timing panel: shows the runtime timing telemetry of the running play session
// (docs/architecture/high-resolution-time.md, "Runtime timing telemetry").
//
// Private editor header (not installed). The panel renders controller state and
// owns only its refresh timer; the controller owns the protocol request and the
// decoded snapshot. Values shown are runtime-computed intervals and counts.

#include <ludus/foundation/base/types.h>

#include <QString>
#include <QTimer>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QHideEvent;
class QLabel;
class QShowEvent;
class QTableWidget;
QT_END_NAMESPACE

namespace ludus::editor
{
class EditorController;
struct PlaySnapshot;

/// Interval between timing requests while the panel is visible, in milliseconds.
inline constexpr foundation::int32 TIMING_REFRESH_MILLISECONDS = 1000;

/// Return the panel's status line for a play snapshot.
[[nodiscard]] QString DescribeTimingState(const PlaySnapshot& play);

/// Dock content listing the runtime's frame timing for the running session.
///
/// While visible it asks the controller for timing once per
/// TIMING_REFRESH_MILLISECONDS; a hidden panel sends no requests. Render() is
/// called from the main window's state-change handler.
class TimingPanel final : public QWidget
{
    Q_OBJECT
public:
    /// @param controller Borrowed; must outlive the panel.
    /// @param parentOrNull Qt parent that owns the panel, if any.
    explicit TimingPanel(EditorController* controller, QWidget* parentOrNull = nullptr);

    /// Show the controller's latest timing snapshot and session state.
    void Render();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    EditorController* mController;
    QLabel* mStatus;
    QTableWidget* mRows;
    QTimer mRefresh;
};
} // namespace ludus::editor
