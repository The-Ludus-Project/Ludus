#include "internal/world_scatter_panel.h"

#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <string_view>

namespace ludus::editor
{
namespace
{
namespace procedural = content::procedural;
using foundation::float64;
using foundation::int32;
using foundation::uint16;
using foundation::uint64;
using foundation::usize;
using procedural::GenerationStageOutcome;
using procedural::GenerationStageRecord;
using procedural::GenerationStatus;
using procedural::ScatterChange;
using procedural::ScatterChangeKind;
using procedural::ScatterDecisionRule;
using procedural::ScatterIdentity;
using procedural::ScatterLockConflict;
using procedural::ScatterOverrideConflict;
using procedural::ScatterOverrideProperty;
using procedural::ScatterPlacement;

/// Placement list columns.
constexpr int32 PLACEMENT_COLUMN = 0;
constexpr int32 POSITION_COLUMN = 1;
constexpr int32 VARIANT_COLUMN = 2;
constexpr int32 ROTATION_COLUMN = 3;
constexpr int32 SCALE_COLUMN = 4;
constexpr int32 OVERRIDE_COLUMN = 5;
/// Conflict list columns.
constexpr int32 CONFLICT_PLACEMENT_COLUMN = 0;
constexpr int32 CONFLICT_PROPERTY_COLUMN = 1;
constexpr int32 CONFLICT_AUTHORED_COLUMN = 2;
constexpr int32 CONFLICT_CANDIDATE_COLUMN = 3;
/// Trace list columns.
constexpr int32 TRACE_ROOM_COLUMN = 0;
constexpr int32 TRACE_LAYER_COLUMN = 1;
constexpr int32 TRACE_OUTCOME_COLUMN = 2;
/// Rotation units per turn of a placement (1/65536 turns), and degrees per turn for display.
constexpr float64 ROTATION_UNITS_PER_TURN = 65536.0;
constexpr float64 DEGREES_PER_TURN = 360.0;
/// Largest rotation an override may hold: one unit short of a full turn.
constexpr int32 MAX_ROTATION = static_cast<int32>(ROTATION_UNITS_PER_TURN) - 1;
/// Decimals of a displayed rotation in degrees.
constexpr int32 ROTATION_DECIMALS = 1;
/// The smallest height a list keeps in a narrow sidebar.
constexpr int32 MIN_LIST_HEIGHT = 96;

[[nodiscard]] QString ToText(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

[[nodiscard]] QString DescribePosition(const ScatterPlacement& placement)
{
    return QStringLiteral("(%1, %2)").arg(placement.LatticeX).arg(placement.LatticeY);
}

[[nodiscard]] QString DescribeRotation(uint16 rotation)
{
    const float64 degrees = static_cast<float64>(rotation) * DEGREES_PER_TURN / ROTATION_UNITS_PER_TURN;
    return QStringLiteral("%1 (%2°)").arg(static_cast<int32>(rotation)).arg(degrees, 0, 'f', ROTATION_DECIMALS);
}

/// The stage counts of a trace, for status lines.
struct TraceCounts final
{
    usize Executed = 0;
    usize CacheHits = 0;
    usize Failed = 0;
};

[[nodiscard]] TraceCounts CountTrace(foundation::BorrowedView<const GenerationStageRecord> records)
{
    TraceCounts counts;
    for (const GenerationStageRecord& record : records)
    {
        counts.Executed += record.Outcome == GenerationStageOutcome::Executed ? 1 : 0;
        counts.CacheHits += record.Outcome == GenerationStageOutcome::CacheHit ? 1 : 0;
        counts.Failed += record.Outcome == GenerationStageOutcome::Failed ? 1 : 0;
    }
    return counts;
}

[[nodiscard]] QString DescribeTrace(const TraceCounts& counts)
{
    return QStringLiteral("%1 layer(s) ran and %2 came from the cache.").arg(counts.Executed).arg(counts.CacheHits);
}

/// Why staging failed, for an outcome other than Success and ConstraintFailed.
[[nodiscard]] QString DescribeStageFailure(GenerationStatus status)
{
    switch (status)
    {
        case GenerationStatus::InvalidInput:
            return QStringLiteral("The rooms' scatter recipes or overrides are invalid. Check the scatter, layer and "
                                  "variant entities of each room, then stage again.");
        case GenerationStatus::CapacityExceeded:
            return QStringLiteral("A scatter layer has more candidates than generation allows. Raise its cell size or "
                                  "lower its candidates per cell, then stage again.");
        case GenerationStatus::OutOfMemory:
            return QStringLiteral("There was not enough memory to stage a candidate. Close other Worlds, then stage "
                                  "again.");
        case GenerationStatus::SearchExhausted:
            return QStringLiteral("A scatter layer exhausted its bounded draws. Lower its candidates per cell, then "
                                  "stage again.");
        default:
            return QStringLiteral("The candidate could not be staged.");
    }
}
} // namespace

QString GetScatterPropertyLabel(ScatterOverrideProperty property)
{
    switch (property)
    {
        case ScatterOverrideProperty::Lock:
            return QStringLiteral("Override removed");
        case ScatterOverrideProperty::Presence:
            return QStringLiteral("Presence");
        case ScatterOverrideProperty::Position:
            return QStringLiteral("Position");
        case ScatterOverrideProperty::Variant:
            return QStringLiteral("Variant");
        case ScatterOverrideProperty::Rotation:
            return QStringLiteral("Rotation");
        case ScatterOverrideProperty::Scale:
            return QStringLiteral("Scale");
    }
    return QString();
}

QString DescribeScatterValue(ScatterOverrideProperty property, bool isPresent, const ScatterPlacement& placement)
{
    if (isPresent == false)
    {
        return property == ScatterOverrideProperty::Lock ? QStringLiteral("No override") : QStringLiteral("Absent");
    }
    switch (property)
    {
        case ScatterOverrideProperty::Lock:
        case ScatterOverrideProperty::Presence:
            return QStringLiteral("%1, variant %2, rotation %3, scale %4")
                .arg(DescribePosition(placement))
                .arg(placement.Variant)
                .arg(DescribeRotation(placement.Rotation))
                .arg(static_cast<int32>(placement.Scale));
        case ScatterOverrideProperty::Position:
            return DescribePosition(placement);
        case ScatterOverrideProperty::Variant:
            return QString::number(placement.Variant);
        case ScatterOverrideProperty::Rotation:
            return DescribeRotation(placement.Rotation);
        case ScatterOverrideProperty::Scale:
            return QString::number(static_cast<int32>(placement.Scale));
    }
    return QString();
}

QString DescribeScatterLockConflict(const ScatterLockConflict& conflict)
{
    if (conflict.IsMissing == false)
    {
        return QStringLiteral("Generated with other values");
    }
    switch (conflict.Decision.Rule)
    {
        case ScatterDecisionRule::CandidateIndex:
        case ScatterDecisionRule::CellOutsideRegion:
            return QStringLiteral("Not a candidate of its layer");
        case ScatterDecisionRule::OutsideRegion:
            return QStringLiteral("Outside the scatter region");
        case ScatterDecisionRule::KeepOut:
            return QStringLiteral("Inside keep-out %1 (a spawn point, exit or collision box)")
                .arg(conflict.Decision.KeepOut);
        case ScatterDecisionRule::LockOutsideRecipe:
            return QStringLiteral("Outside its cell, or variant or scale outside the layer's range");
        case ScatterDecisionRule::Conflict:
            return QStringLiteral("Too close to a higher-ranked override");
        case ScatterDecisionRule::ScopeRemoved:
            return QStringLiteral("Its room is no longer generated");
        case ScatterDecisionRule::LayerRemoved:
            return QStringLiteral("Its layer was removed");
        case ScatterDecisionRule::Accepted:
            break;
    }
    return QStringLiteral("Not generated");
}

WorldScatterPanel::WorldScatterPanel(QWidget* parentOrNull) : QWidget(parentOrNull)
{
    setObjectName(QStringLiteral("worldScatterPanel"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    mStatusOrNull = new QLabel(this);
    mStatusOrNull->setObjectName(QStringLiteral("scatterStatus"));
    mStatusOrNull->setTextFormat(Qt::PlainText);
    mStatusOrNull->setWordWrap(true);
    mStatusOrNull->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(mStatusOrNull);

    QHBoxLayout* commands = new QHBoxLayout;
    mStageOrNull = new QPushButton(QStringLiteral("Stage Candidate"), this);
    mStageOrNull->setObjectName(QStringLiteral("scatterStage"));
    mStageOrNull->setToolTip(QStringLiteral("Generate every room with the current overrides. The accepted "
                                            "placements stay unchanged until you publish."));
    mPublishOrNull = new QPushButton(QStringLiteral("Publish"), this);
    mPublishOrNull->setObjectName(QStringLiteral("scatterPublish"));
    mPublishOrNull->setToolTip(QStringLiteral("Accept the pending candidate. It is refused if the overrides or "
                                              "recipes changed after it was staged."));
    mRejectOrNull = new QPushButton(QStringLiteral("Reject"), this);
    mRejectOrNull->setObjectName(QStringLiteral("scatterReject"));
    mRejectOrNull->setToolTip(QStringLiteral("Discard the pending candidate and keep the accepted placements."));
    for (QPushButton* button : {mStageOrNull, mPublishOrNull, mRejectOrNull})
    {
        button->setProperty("density", QStringLiteral("compact"));
        button->setMinimumHeight(button->sizeHint().height());
        commands->addWidget(button);
    }
    layout->addLayout(commands);

    mPlacementsOrNull = new QTreeWidget(this);
    mPlacementsOrNull->setObjectName(QStringLiteral("scatterPlacements"));
    mPlacementsOrNull->setAccessibleName(QStringLiteral("Accepted scatter placements"));
    mPlacementsOrNull->setHeaderLabels({QStringLiteral("Placement"),
                                        QStringLiteral("Position"),
                                        QStringLiteral("Variant"),
                                        QStringLiteral("Rotation"),
                                        QStringLiteral("Scale"),
                                        QStringLiteral("Override")});
    mPlacementsOrNull->setRootIsDecorated(false);
    mPlacementsOrNull->setItemsExpandable(false);
    mPlacementsOrNull->setAllColumnsShowFocus(true);
    mPlacementsOrNull->setMinimumHeight(MIN_LIST_HEIGHT);
    mPlacementsOrNull->setTextElideMode(Qt::ElideMiddle);
    mPlacementsOrNull->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(mPlacementsOrNull, 1);

    mOverrideOrNull = new QGroupBox(QStringLiteral("Override"), this);
    mOverrideOrNull->setObjectName(QStringLiteral("scatterOverride"));
    QFormLayout* form = new QFormLayout(mOverrideOrNull);
    const auto makeSpin = [this, form](const QString& key, const QString& label) {
        QSpinBox* spin = new QSpinBox(mOverrideOrNull);
        spin->setObjectName(QStringLiteral("scatterOverride:") + key);
        spin->setAccessibleName(label);
        form->addRow(label, spin);
        return spin;
    };
    mPositionXOrNull = makeSpin(QStringLiteral("x"), QStringLiteral("Position X"));
    mPositionYOrNull = makeSpin(QStringLiteral("y"), QStringLiteral("Position Y"));
    mVariantOrNull = makeSpin(QStringLiteral("variant"), QStringLiteral("Variant"));
    mRotationOrNull = makeSpin(QStringLiteral("rotation"), QStringLiteral("Rotation"));
    mRotationOrNull->setRange(0, MAX_ROTATION);
    mRotationOrNull->setToolTip(QStringLiteral("In 1/65536 turns."));
    mScaleOrNull = makeSpin(QStringLiteral("scale"), QStringLiteral("Scale"));
    mScaleOrNull->setToolTip(QStringLiteral("In thousandths of the variant's authored size, within the layer's "
                                            "range."));
    mPositionXOrNull->setToolTip(QStringLiteral("In lattice units, within the placement's cell."));
    mPositionYOrNull->setToolTip(mPositionXOrNull->toolTip());
    QHBoxLayout* overrideCommands = new QHBoxLayout;
    mApplyOverrideOrNull = new QPushButton(QStringLiteral("Apply Override"), mOverrideOrNull);
    mApplyOverrideOrNull->setObjectName(QStringLiteral("scatterApplyOverride"));
    mApplyOverrideOrNull->setToolTip(QStringLiteral("Lock the selected placement at these values. Stage a candidate "
                                                    "to generate them."));
    mRemoveOverrideOrNull = new QPushButton(QStringLiteral("Remove Override"), mOverrideOrNull);
    mRemoveOverrideOrNull->setObjectName(QStringLiteral("scatterRemoveOverride"));
    mRemoveOverrideOrNull->setToolTip(QStringLiteral("Let generation choose the selected placement's values again."));
    for (QPushButton* button : {mApplyOverrideOrNull, mRemoveOverrideOrNull})
    {
        button->setProperty("density", QStringLiteral("compact"));
        button->setMinimumHeight(button->sizeHint().height());
        overrideCommands->addWidget(button);
    }
    form->addRow(overrideCommands);
    layout->addWidget(mOverrideOrNull);

    // The conflict area stays next to the commands that caused it, so the refusal and its repair are where the
    // author acted (AGENTS.md: report rejected edits in the surface where the user acted).
    mConflictsOrNull = new QFrame(this);
    mConflictsOrNull->setObjectName(QStringLiteral("scatterConflicts"));
    mConflictsOrNull->setFrameShape(QFrame::StyledPanel);
    QVBoxLayout* conflicts = new QVBoxLayout(mConflictsOrNull);
    mConflictMessageOrNull = new QLabel(mConflictsOrNull);
    mConflictMessageOrNull->setObjectName(QStringLiteral("scatterConflictMessage"));
    mConflictMessageOrNull->setTextFormat(Qt::PlainText);
    mConflictMessageOrNull->setWordWrap(true);
    mConflictMessageOrNull->setTextInteractionFlags(Qt::TextSelectableByMouse);
    conflicts->addWidget(mConflictMessageOrNull);
    mConflictListOrNull = new QTreeWidget(mConflictsOrNull);
    mConflictListOrNull->setObjectName(QStringLiteral("scatterConflictList"));
    mConflictListOrNull->setAccessibleName(QStringLiteral("Scatter override conflicts"));
    mConflictListOrNull->setHeaderLabels({QStringLiteral("Placement"),
                                          QStringLiteral("Property"),
                                          QStringLiteral("Authored"),
                                          QStringLiteral("Candidate")});
    mConflictListOrNull->setRootIsDecorated(false);
    mConflictListOrNull->setItemsExpandable(false);
    mConflictListOrNull->setAllColumnsShowFocus(true);
    mConflictListOrNull->setTextElideMode(Qt::ElideMiddle);
    mConflictListOrNull->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    conflicts->addWidget(mConflictListOrNull);
    QHBoxLayout* repairs = new QHBoxLayout;
    mRetryOrNull = new QPushButton(QStringLiteral("Keep Authored Values and Retry"), mConflictsOrNull);
    mRetryOrNull->setObjectName(QStringLiteral("scatterRetry"));
    mRetryOrNull->setToolTip(QStringLiteral("Stage the current overrides and publish them. Only the edited layers "
                                            "run again; every other layer comes from the cache."));
    mRejectProposalOrNull = new QPushButton(QStringLiteral("Reject Proposal"), mConflictsOrNull);
    mRejectProposalOrNull->setObjectName(QStringLiteral("scatterRejectProposal"));
    mRejectProposalOrNull->setToolTip(QStringLiteral("Discard the candidate. The accepted placements and your "
                                                     "overrides stay as they are."));
    for (QPushButton* button : {mRetryOrNull, mRejectProposalOrNull})
    {
        button->setProperty("density", QStringLiteral("compact"));
        button->setMinimumHeight(button->sizeHint().height());
        repairs->addWidget(button);
    }
    conflicts->addLayout(repairs);
    layout->addWidget(mConflictsOrNull);

    mCandidateOrNull = new QLabel(this);
    mCandidateOrNull->setObjectName(QStringLiteral("scatterCandidate"));
    mCandidateOrNull->setTextFormat(Qt::PlainText);
    mCandidateOrNull->setWordWrap(true);
    mCandidateOrNull->setProperty("visualRole", QStringLiteral("secondary"));
    layout->addWidget(mCandidateOrNull);
    mTraceOrNull = new QTreeWidget(this);
    mTraceOrNull->setObjectName(QStringLiteral("scatterTrace"));
    mTraceOrNull->setAccessibleName(QStringLiteral("Scatter stage evaluations"));
    mTraceOrNull->setHeaderLabels({QStringLiteral("Room"), QStringLiteral("Layer"), QStringLiteral("Outcome")});
    mTraceOrNull->setRootIsDecorated(false);
    mTraceOrNull->setItemsExpandable(false);
    mTraceOrNull->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(mTraceOrNull);

    connect(mStageOrNull, &QPushButton::clicked, this, &WorldScatterPanel::stage);
    connect(mPublishOrNull, &QPushButton::clicked, this, &WorldScatterPanel::publish);
    connect(mRejectOrNull, &QPushButton::clicked, this, &WorldScatterPanel::reject);
    connect(mRetryOrNull, &QPushButton::clicked, this, &WorldScatterPanel::retry);
    connect(mRejectProposalOrNull, &QPushButton::clicked, this, &WorldScatterPanel::reject);
    connect(mApplyOverrideOrNull, &QPushButton::clicked, this, &WorldScatterPanel::applyOverride);
    connect(mRemoveOverrideOrNull, &QPushButton::clicked, this, &WorldScatterPanel::removeOverride);
    connect(mPlacementsOrNull, &QTreeWidget::itemSelectionChanged, this, &WorldScatterPanel::selectionChanged);
    showStatus(QStringLiteral("Open a World-demo room with scatter to generate it."));
    refresh();
}

bool WorldScatterPanel::SetRooms(const QVector<world_demo::Level>& levels, const QStringList& sources)
{
    usize failed = 0;
    if (mSession.TrySetRooms({levels.constData(), static_cast<usize>(levels.size())}, failed) == false)
    {
        const QString source = failed < static_cast<usize>(sources.size()) ? sources[static_cast<qsizetype>(failed)]
                                                                           : QStringLiteral("a room");
        showStatus(QStringLiteral("The scatter of %1 cannot be generated: its room is not in the World demo's room "
                                  "catalog, repeats another room, or its scatter recipe is out of range. Fix its "
                                  "scatter entities; the previous rooms are still shown.")
                       .arg(source));
        refresh();
        return false;
    }
    if (mSession.GetRoomCount() == 0)
    {
        showStatus(QStringLiteral("No World-demo room with scatter is open."));
    }
    else if (mSession.GetAcceptedRevision() == 0)
    {
        showStatus(QStringLiteral("%1 room(s) ready. Stage a candidate, then publish it to accept the generated "
                                  "placements.")
                       .arg(mSession.GetRoomCount()));
    }
    refresh();
    return true;
}

void WorldScatterPanel::SetSiblingRooms(QVector<world_demo::Level> levels, QStringList sources)
{
    mSiblingLevels = foundation::Move(levels);
    mSiblingSources = foundation::Move(sources);
}

bool WorldScatterPanel::SetOpenRoom(const world_demo::Level& level, const QString& source)
{
    QVector<world_demo::Level> levels{level};
    QStringList sources{source};
    const uint16 openRoom = world_demo::FindCatalogRoom(level.Document, level.Id.View());
    for (qsizetype index = 0; index < mSiblingLevels.size(); ++index)
    {
        const world_demo::Level& sibling = mSiblingLevels[index];
        if (sibling.HasScatter == true && world_demo::FindCatalogRoom(sibling.Document, sibling.Id.View()) != openRoom)
        {
            levels.append(sibling);
            sources.append(mSiblingSources.value(index));
        }
    }
    return SetRooms(levels, sources);
}

void WorldScatterPanel::ShowRoomsUnavailable(const QString& reason)
{
    showStatus(reason);
}

QString WorldScatterPanel::describeRoom(uint16 room) const
{
    for (usize index = 0; index < mSession.GetRoomCount(); ++index)
    {
        if (mSession.GetRecipe(index).Room == room)
        {
            return ToText(mSession.GetLevel(index).Id.View());
        }
    }
    return QStringLiteral("room %1").arg(static_cast<int32>(room));
}

QString WorldScatterPanel::describeLayer(uint16 room, foundation::uint8 layer) const
{
    for (usize index = 0; index < mSession.GetRoomCount(); ++index)
    {
        if (mSession.GetRecipe(index).Room != room)
        {
            continue;
        }
        const world_demo::RoomScatter& scatter = mSession.GetLevel(index).Scatter;
        for (usize authored = 0; authored < scatter.LayerCount; ++authored)
        {
            if (scatter.Layers[authored].Layer == layer)
            {
                return ToText(scatter.Layers[authored].Id.View());
            }
        }
    }
    return QStringLiteral("layer %1").arg(static_cast<int32>(layer));
}

QString WorldScatterPanel::DescribePlacement(ScatterIdentity identity) const
{
    return QStringLiteral("%1 · %2 · cell (%3, %4) · #%5")
        .arg(describeRoom(identity.Room), describeLayer(identity.Room, identity.Layer))
        .arg(static_cast<int32>(identity.CellX))
        .arg(static_cast<int32>(identity.CellY))
        .arg(static_cast<int32>(identity.Candidate));
}

void WorldScatterPanel::stage()
{
    mIsStaleRefusal = false;
    const GenerationStatus status = mSession.Stage();
    const TraceCounts counts = CountTrace(mSession.GetTrace());
    if (status == GenerationStatus::Success)
    {
        usize changed = 0;
        usize removed = 0;
        usize added = 0;
        for (const ScatterChange& change : mSession.GetChanges())
        {
            changed += change.Kind == ScatterChangeKind::Changed ? 1 : 0;
            removed += change.Kind == ScatterChangeKind::Removed ? 1 : 0;
            added += change.Kind == ScatterChangeKind::Added ? 1 : 0;
        }
        showStatus(QStringLiteral("Candidate staged: %1 changed, %2 removed and %3 added placement(s). %4 Publish "
                                  "it to accept it.")
                       .arg(changed)
                       .arg(removed)
                       .arg(added)
                       .arg(DescribeTrace(counts)));
    }
    else if (status == GenerationStatus::ConstraintFailed)
    {
        showStatus(QStringLiteral("%1 override(s) cannot be honoured, so this candidate cannot be published. Edit or "
                                  "remove each listed override, then stage again. The accepted placements are "
                                  "unchanged.")
                       .arg(mSession.GetCandidate().Conflicts.GetSize()));
    }
    else
    {
        showStatus(DescribeStageFailure(status));
    }
    refresh();
}

void WorldScatterPanel::publish()
{
    const GenerationStatus status = mSession.Publish();
    mIsStaleRefusal = status == GenerationStatus::StaleCandidate;
    if (status == GenerationStatus::Success)
    {
        showStatus(QStringLiteral("Published revision %1 with %2 placement(s).")
                       .arg(mSession.GetAcceptedRevision())
                       .arg(mSession.GetAccepted().Placements.GetSize()));
    }
    else if (status == GenerationStatus::StaleCandidate)
    {
        // The conflict area carries the explanation and the repair.
        showStatus(QStringLiteral("Publication refused. The accepted placements (revision %1) are unchanged.")
                       .arg(mSession.GetAcceptedRevision()));
    }
    else if (status == GenerationStatus::NotReady &&
             mSession.GetCandidate().Status == GenerationStatus::ConstraintFailed)
    {
        showStatus(QStringLiteral("This candidate does not honour every override and cannot be published. Edit or "
                                  "remove each listed override, then stage again."));
    }
    else
    {
        showStatus(QStringLiteral("There is no staged candidate to publish. Stage a candidate first."));
    }
    refresh();
}

void WorldScatterPanel::retry()
{
    const GenerationStatus status = mSession.RetryWithAuthoredValues();
    mIsStaleRefusal = status == GenerationStatus::StaleCandidate;
    const TraceCounts counts = CountTrace(mSession.GetTrace());
    if (status == GenerationStatus::Success)
    {
        showStatus(QStringLiteral("Restaged with the authored values and published revision %1. %2")
                       .arg(mSession.GetAcceptedRevision())
                       .arg(DescribeTrace(counts)));
    }
    else if (status == GenerationStatus::ConstraintFailed)
    {
        showStatus(QStringLiteral("Restaged with the authored values, but %1 override(s) cannot be honoured. Edit or "
                                  "remove each listed override, then stage again. The accepted placements are "
                                  "unchanged.")
                       .arg(mSession.GetCandidate().Conflicts.GetSize()));
    }
    else
    {
        showStatus(DescribeStageFailure(status));
    }
    refresh();
}

void WorldScatterPanel::reject()
{
    mSession.Reject();
    mIsStaleRefusal = false;
    showStatus(QStringLiteral("Proposal rejected. The accepted placements (revision %1) and your overrides are "
                              "unchanged.")
                   .arg(mSession.GetAcceptedRevision()));
    refresh();
}

void WorldScatterPanel::applyOverride()
{
    ScatterIdentity identity;
    if (getSelectedIdentity(identity) == false)
    {
        return;
    }
    const ScatterPlacement placement
    {
        .Identity = identity,
        .LatticeX = mPositionXOrNull->value(),
        .LatticeY = mPositionYOrNull->value(),
        .Variant = static_cast<foundation::uint32>(mVariantOrNull->value()),
        .Rotation = static_cast<uint16>(mRotationOrNull->value()),
        .Scale = static_cast<uint16>(mScaleOrNull->value()),
    };
    if (mSession.TrySetOverride(placement) == false)
    {
        showStatus(QStringLiteral("The override of %1 could not be set: its room is no longer generated or already "
                                  "has the most overrides a room may hold. Remove an override of that room first.")
                       .arg(DescribePlacement(identity)));
        return;
    }
    // A later edit makes an earlier refusal's conflict list out of date; publishing again reports the current one.
    mIsStaleRefusal = false;
    if (mSession.HasPendingCandidate() == true)
    {
        showStatus(QStringLiteral("Override of %1 set. The pending candidate was staged before this edit, so "
                                  "publishing it will be refused; stage again or keep the authored values when "
                                  "asked.")
                       .arg(DescribePlacement(identity)));
    }
    else
    {
        showStatus(
            QStringLiteral("Override of %1 set. Stage a candidate to generate it.").arg(DescribePlacement(identity)));
    }
    refresh();
}

void WorldScatterPanel::removeOverride()
{
    ScatterIdentity identity;
    if (getSelectedIdentity(identity) == false || mSession.RemoveOverride(identity) == false)
    {
        return;
    }
    mIsStaleRefusal = false;
    showStatus(QStringLiteral("Override of %1 removed. Stage a candidate to let generation choose its values again.")
                   .arg(DescribePlacement(identity)));
    refresh();
}

void WorldScatterPanel::selectionChanged()
{
    updateOverrideEditor();
}

void WorldScatterPanel::refresh()
{
    refreshPlacements();
    refreshCandidate();
    refreshConflicts();
    const bool hasRooms = mSession.GetRoomCount() > 0;
    const bool hasPending = mSession.HasPendingCandidate();
    mStageOrNull->setEnabled(hasRooms == true);
    mPublishOrNull->setEnabled(hasPending == true && mSession.GetCandidate().Status == GenerationStatus::Success);
    mRejectOrNull->setEnabled(hasPending == true);
    updateOverrideEditor();
}

const ScatterPlacement* WorldScatterPanel::findShownPlacementOrNull(ScatterIdentity identity) const
{
    const ScatterPlacement* overrideOrNull = mSession.FindOverrideOrNull(identity);
    if (overrideOrNull != nullptr)
    {
        return overrideOrNull;
    }
    return mSession.GetAccepted().FindPlacement(identity);
}

void WorldScatterPanel::refreshPlacements()
{
    ScatterIdentity selected;
    const bool hasSelection = getSelectedIdentity(selected);
    const QSignalBlocker blocker(mPlacementsOrNull);
    mPlacementsOrNull->clear();
    QTreeWidgetItem* selectedItemOrNull = nullptr;
    const auto addRow = [this, &selectedItemOrNull, hasSelection, selected](const ScatterPlacement& accepted) {
        const ScatterPlacement* overrideOrNull = mSession.FindOverrideOrNull(accepted.Identity);
        const ScatterPlacement& shown = overrideOrNull != nullptr ? *overrideOrNull : accepted;
        QTreeWidgetItem* item = new QTreeWidgetItem(mPlacementsOrNull);
        item->setText(PLACEMENT_COLUMN, DescribePlacement(accepted.Identity));
        item->setText(POSITION_COLUMN, DescribePosition(shown));
        item->setText(VARIANT_COLUMN, QString::number(shown.Variant));
        item->setText(ROTATION_COLUMN, DescribeRotation(shown.Rotation));
        item->setText(SCALE_COLUMN, QString::number(static_cast<int32>(shown.Scale)));
        item->setText(OVERRIDE_COLUMN, overrideOrNull != nullptr ? QStringLiteral("Locked") : QString());
        item->setData(PLACEMENT_COLUMN,
                      Qt::UserRole,
                      QVariant::fromValue<qulonglong>(procedural::PackScatterScope(accepted.Identity)));
        item->setToolTip(PLACEMENT_COLUMN, item->text(PLACEMENT_COLUMN));
        if (hasSelection == true && accepted.Identity == selected)
        {
            selectedItemOrNull = item;
        }
    };
    for (const ScatterPlacement& placement : mSession.GetAccepted().Placements)
    {
        addRow(placement);
    }
    // An override of a placement the accepted manifest lacks still has a row, so it can be edited or removed.
    for (const ScatterPlacement& placement : mSession.GetOverrides())
    {
        if (mSession.GetAccepted().FindPlacement(placement.Identity) == nullptr)
        {
            addRow(placement);
        }
    }
    if (selectedItemOrNull != nullptr)
    {
        mPlacementsOrNull->setCurrentItem(selectedItemOrNull);
    }
}

void WorldScatterPanel::refreshCandidate()
{
    mTraceOrNull->clear();
    if (mSession.HasPendingCandidate() == false)
    {
        mCandidateOrNull->setText(mSession.GetAcceptedRevision() == 0
                                      ? QStringLiteral("No candidate is pending and nothing is accepted yet.")
                                      : QStringLiteral("No candidate is pending. Revision %1 is accepted.")
                                            .arg(mSession.GetAcceptedRevision()));
    }
    else
    {
        mCandidateOrNull->setText(QStringLiteral("Candidate pending, staged against revision %1. Layer evaluations "
                                                 "of the last stage:")
                                      .arg(mSession.GetCandidate().BaseRevision));
    }
    for (const GenerationStageRecord& record : mSession.GetTrace())
    {
        const uint16 room = static_cast<uint16>(record.Scope);
        QTreeWidgetItem* item = new QTreeWidgetItem(mTraceOrNull);
        item->setText(TRACE_ROOM_COLUMN, describeRoom(room));
        item->setText(TRACE_LAYER_COLUMN, describeLayer(room, static_cast<foundation::uint8>(record.Stage)));
        item->setText(TRACE_OUTCOME_COLUMN,
                      record.Outcome == GenerationStageOutcome::CacheHit
                          ? QStringLiteral("Cache hit")
                          : (record.Outcome == GenerationStageOutcome::Executed ? QStringLiteral("Executed")
                                                                                : QStringLiteral("Failed")));
    }
}

void WorldScatterPanel::refreshConflicts()
{
    mConflictListOrNull->clear();
    const foundation::BorrowedView<const ScatterOverrideConflict> overrides = mSession.GetOverrideConflicts();
    const bool hasLockConflicts =
        mSession.HasPendingCandidate() == true && mSession.GetCandidate().Status == GenerationStatus::ConstraintFailed;
    if (mIsStaleRefusal == true)
    {
        for (const ScatterOverrideConflict& conflict : overrides)
        {
            QTreeWidgetItem* item = new QTreeWidgetItem(mConflictListOrNull);
            item->setText(CONFLICT_PLACEMENT_COLUMN, DescribePlacement(conflict.Identity));
            item->setText(CONFLICT_PROPERTY_COLUMN, GetScatterPropertyLabel(conflict.Property));
            item->setText(CONFLICT_AUTHORED_COLUMN,
                          DescribeScatterValue(conflict.Property, conflict.HasAuthored, conflict.Authored));
            item->setText(CONFLICT_CANDIDATE_COLUMN,
                          DescribeScatterValue(conflict.Property, conflict.HasCandidate, conflict.Candidate));
            item->setToolTip(CONFLICT_PLACEMENT_COLUMN, item->text(CONFLICT_PLACEMENT_COLUMN));
        }
        mConflictMessageOrNull->setText(
            overrides.IsEmpty() == true
                ? QStringLiteral("The rooms' scatter recipes changed after this candidate was staged, so it was not "
                                 "published and the accepted placements are unchanged. Keep the authored values and "
                                 "retry to stage and publish the current recipes, or reject the proposal.")
                : QStringLiteral("%1 override value(s) changed after this candidate was staged, so it was not "
                                 "published and the accepted placements are unchanged. Keep the authored values and "
                                 "retry to stage and publish them, or reject the proposal.")
                      .arg(overrides.GetSize()));
    }
    else if (hasLockConflicts == true)
    {
        for (const ScatterLockConflict& conflict : mSession.GetCandidate().Conflicts)
        {
            QTreeWidgetItem* item = new QTreeWidgetItem(mConflictListOrNull);
            item->setText(CONFLICT_PLACEMENT_COLUMN, DescribePlacement(conflict.Lock.Identity));
            item->setText(CONFLICT_PROPERTY_COLUMN, QStringLiteral("Override not honoured"));
            item->setText(CONFLICT_AUTHORED_COLUMN,
                          DescribeScatterValue(ScatterOverrideProperty::Presence, true, conflict.Lock));
            item->setText(CONFLICT_CANDIDATE_COLUMN,
                          conflict.IsMissing == true
                              ? DescribeScatterLockConflict(conflict)
                              : DescribeScatterValue(ScatterOverrideProperty::Presence, true, conflict.Generated));
            item->setToolTip(CONFLICT_CANDIDATE_COLUMN, DescribeScatterLockConflict(conflict));
        }
        mConflictMessageOrNull->setText(QStringLiteral("The candidate cannot honour %1 override(s). Select each "
                                                       "placement, then edit or remove its override and stage again; "
                                                       "or reject the proposal.")
                                            .arg(mSession.GetCandidate().Conflicts.GetSize()));
    }
    mRetryOrNull->setVisible(mIsStaleRefusal == true);
    mConflictsOrNull->setVisible(mIsStaleRefusal == true || hasLockConflicts == true);
}

void WorldScatterPanel::updateOverrideEditor()
{
    ScatterIdentity identity;
    const ScatterPlacement* shownOrNull = nullptr;
    const procedural::ScatterLayer* layerOrNull = nullptr;
    if (getSelectedIdentity(identity) == true)
    {
        shownOrNull = findShownPlacementOrNull(identity);
        layerOrNull = mSession.FindLayerOrNull(identity.Room, identity.Layer);
    }
    const bool canEdit = shownOrNull != nullptr && layerOrNull != nullptr;
    mOverrideOrNull->setEnabled(canEdit == true);
    mRemoveOverrideOrNull->setEnabled(canEdit == true && mSession.FindOverrideOrNull(identity) != nullptr);
    if (canEdit == false)
    {
        mOverrideOrNull->setTitle(QStringLiteral("Override"));
        return;
    }
    mOverrideOrNull->setTitle(QStringLiteral("Override: %1").arg(DescribePlacement(identity)));
    // A lock is honoured only inside its identity's cell and the layer's variant and scale ranges.
    const int32 cellSize = layerOrNull->CellSize;
    const int32 cellX = static_cast<int32>(identity.CellX) * cellSize;
    const int32 cellY = static_cast<int32>(identity.CellY) * cellSize;
    mPositionXOrNull->setRange(cellX, cellX + cellSize - 1);
    mPositionYOrNull->setRange(cellY, cellY + cellSize - 1);
    mVariantOrNull->setRange(0, static_cast<int32>(layerOrNull->Variants.GetSize()) - 1);
    mScaleOrNull->setRange(layerOrNull->MinimumScale, layerOrNull->MaximumScale);
    mPositionXOrNull->setValue(shownOrNull->LatticeX);
    mPositionYOrNull->setValue(shownOrNull->LatticeY);
    mVariantOrNull->setValue(static_cast<int32>(shownOrNull->Variant));
    mRotationOrNull->setValue(shownOrNull->Rotation);
    mScaleOrNull->setValue(shownOrNull->Scale);
}

void WorldScatterPanel::showStatus(const QString& message)
{
    mStatusOrNull->setText(message);
}

bool WorldScatterPanel::getSelectedIdentity(ScatterIdentity& outIdentity) const
{
    const QTreeWidgetItem* itemOrNull = mPlacementsOrNull->currentItem();
    if (itemOrNull == nullptr || itemOrNull->isSelected() == false)
    {
        return false;
    }
    outIdentity = procedural::UnpackScatterScope(
        static_cast<uint64>(itemOrNull->data(PLACEMENT_COLUMN, Qt::UserRole).toULongLong()));
    return true;
}
} // namespace ludus::editor
