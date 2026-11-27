#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/content/procedural/environment/scatter_generation.h>

#include "internal/level.h"
#include "internal/scatter_generation_session.h"

#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QFrame;
class QGroupBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTreeWidget;
QT_END_NAMESPACE

namespace ludus::editor
{
// The World workspace's Scatter tab: staged generation of the World-demo rooms' scatter
// with author overrides (docs/development/editor-workspace.md#repair-a-stale-scatter-candidate).
// The author locks or overrides a generated placement, stages a candidate and publishes it.
// A publication refused because an override changed while the candidate was pending names
// each conflict by placement and property with both values, and offers Keep authored values
// and retry or Reject proposal. The accepted placements change only on a valid publication.
// Offering both decisions at the refusal, instead of resolving the conflict silently, follows
// Holter et al., "Toward Agency in Human-AI Collaboration" (conceptual model of agency and
// attribution, https://www.steffenholter.com/papers/towards-agency.pdf) and Amershi et al.,
// "Guidelines for Human-AI Interaction", CHI 2019, Table 1, G8 (efficient dismissal) and
// G9 (efficient correction).

/// The conflicting property's name, as the conflict list shows it.
[[nodiscard]] QString GetScatterPropertyLabel(content::procedural::ScatterOverrideProperty property);
/// One property of a placement as the conflict list shows it; `isPresent` false reads as the value's absence.
[[nodiscard]] QString DescribeScatterValue(content::procedural::ScatterOverrideProperty property,
                                           bool isPresent,
                                           const content::procedural::ScatterPlacement& placement);
/// Why a lock could not be honoured, from the decision that removed its candidate.
[[nodiscard]] QString DescribeScatterLockConflict(const content::procedural::ScatterLockConflict& conflict);

/// Scatter placements, override editor, candidate trace and conflict repair of the World-demo rooms. GUI thread.
class WorldScatterPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit WorldScatterPanel(QWidget* parentOrNull = nullptr);
    WorldScatterPanel(const WorldScatterPanel&) = delete;
    WorldScatterPanel& operator=(const WorldScatterPanel&) = delete;

    /// @brief Generate these rooms from now on.
    ///
    /// The accepted placements and overrides are kept; a pending candidate staged from other recipes can no longer
    /// publish. A failure keeps the previous rooms and shows the reason in the panel.
    /// @param levels The World-demo rooms; levels without scatter are skipped.
    /// @param sources A label per level for the failure message, such as its file name.
    /// @return Whether every room's recipe was accepted.
    bool SetRooms(const QVector<world_demo::Level>& levels, const QStringList& sources);
    /// @brief Remember the rooms of the other World-demo Worlds beside the open one, for the next SetOpenRoom.
    /// @param levels The sibling Worlds' levels.
    /// @param sources Each level's file name.
    void SetSiblingRooms(QVector<world_demo::Level> levels, QStringList sources);
    /// @brief Generate the open World's room together with the sibling rooms.
    ///
    /// A sibling in the same catalog room as the open World is left out: the open draft is that room's input.
    /// @param level The open World's projected level.
    /// @param source The open World's file name.
    /// @return SetRooms's result.
    bool SetOpenRoom(const world_demo::Level& level, const QString& source);
    /// Show why the open World's room cannot be generated; the previous rooms stay.
    void ShowRoomsUnavailable(const QString& reason);
    /// The session the panel presents; for tests and the World workspace.
    [[nodiscard]] LUDUS_INLINE const ScatterGenerationSession& GetSession() const noexcept
    {
        return mSession;
    }
    /// The room and layer of a placement, as the lists name it: "second-room · shrubs · cell (3, -1) · #0".
    [[nodiscard]] QString DescribePlacement(content::procedural::ScatterIdentity identity) const;

private:
    void stage();
    void publish();
    void retry();
    void reject();
    void applyOverride();
    void removeOverride();
    void selectionChanged();
    /// Rebuild every list and label from the session, keeping the selected placement.
    void refresh();
    void refreshPlacements();
    void refreshCandidate();
    void refreshConflicts();
    void updateOverrideEditor();
    void showStatus(const QString& message);
    [[nodiscard]] QString describeRoom(foundation::uint16 room) const;
    [[nodiscard]] QString describeLayer(foundation::uint16 room, foundation::uint8 layer) const;
    [[nodiscard]] bool getSelectedIdentity(content::procedural::ScatterIdentity& outIdentity) const;
    [[nodiscard]] const content::procedural::ScatterPlacement*
    findShownPlacementOrNull(content::procedural::ScatterIdentity identity) const;

    ScatterGenerationSession mSession;
    QVector<world_demo::Level> mSiblingLevels;
    QStringList mSiblingSources;
    QLabel* mStatusOrNull = nullptr;
    QPushButton* mStageOrNull = nullptr;
    QPushButton* mPublishOrNull = nullptr;
    QPushButton* mRejectOrNull = nullptr;
    QTreeWidget* mPlacementsOrNull = nullptr;
    QGroupBox* mOverrideOrNull = nullptr;
    QSpinBox* mPositionXOrNull = nullptr;
    QSpinBox* mPositionYOrNull = nullptr;
    QSpinBox* mVariantOrNull = nullptr;
    QSpinBox* mRotationOrNull = nullptr;
    QSpinBox* mScaleOrNull = nullptr;
    QPushButton* mApplyOverrideOrNull = nullptr;
    QPushButton* mRemoveOverrideOrNull = nullptr;
    QLabel* mCandidateOrNull = nullptr;
    QTreeWidget* mTraceOrNull = nullptr;
    QFrame* mConflictsOrNull = nullptr;
    QLabel* mConflictMessageOrNull = nullptr;
    QTreeWidget* mConflictListOrNull = nullptr;
    QPushButton* mRetryOrNull = nullptr;
    QPushButton* mRejectProposalOrNull = nullptr;
    /// Whether the last publication was refused as stale; the conflict area then explains the refusal.
    bool mIsStaleRefusal = false;
};
} // namespace ludus::editor
