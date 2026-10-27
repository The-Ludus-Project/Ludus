#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/ray_path_capture.h"
#include "internal/ray_path_inspection.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace ludus::editor
{
/// Inspector panel of a ray path capture: its state (picking, building, tracing, ready, stale,
/// truncated, failed or unavailable), the decoded events grouped by sample, then pass, then
/// sequence, the selected event's fields, and the overlay controls (X-ray, coloring, sample).
/// Selecting a row highlights its segment; a hit row selects its World object. Observes and
/// drives the inspection it borrows, which must outlive this panel. GUI thread only.
class RayPathInspector final : public QWidget
{
    Q_OBJECT
public:
    explicit RayPathInspector(RayPathInspection& inoutInspection, QWidget* parentOrNull = nullptr);
    /// Status line as shown.
    [[nodiscard]] QString GetStatusText() const;
    /// Fields of the selected event as shown; empty without a selection.
    [[nodiscard]] QString GetDetailsText() const;
    /// Number of event rows in the tree.
    [[nodiscard]] foundation::int32 GetEventRowCount() const;
    /// Select the tree row of a decoded event, as a click does.
    /// @return False when no row shows that event.
    bool SelectEventRow(foundation::int32 event);

private:
    void refresh();
    void rebuildTree();
    void showDetails(foundation::int32 event);
    RayPathInspection& mInspection;
    QLabel* mStatus;
    QCheckBox* mXRay;
    QComboBox* mColorMode;
    QComboBox* mSample;
    QPushButton* mCaptureAgain;
    QPushButton* mLeave;
    QTreeWidget* mEvents;
    QLabel* mDetails;
    /// What the tree shows; every new capture passes through Building, so a state change rebuilds it.
    RayPathInspectionStage mShownStage = RayPathInspectionStage::Inactive;
    RayPathCaptureState mShownState = RayPathCaptureState::Idle;
};
} // namespace ludus::editor
