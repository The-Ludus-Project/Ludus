#pragma once

#include <ludus/foundation/base/types.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QWidget;
QT_END_NAMESPACE

// The World's Environment tool: edits a draft of the World climate (WORLD_CLIMATE) and previews it, then
// requests one undoable commit (docs/modules/gameplay/weather.md#edit-a-climate-in-the-environment-tool).
// The draft is validated on every edit by the engine cooker, and failures stay in this panel with their repair.
namespace ludus::editor
{
/// Environment tab of the World sidebar: domain, presets, schedules and regions of the World climate, a
/// preview tick and a position inspector. The panel never edits the document; ApplyRequested carries the
/// draft to WorldDocument::SetClimate.
class WorldEnvironmentPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit WorldEnvironmentPanel(QWidget* parentOrNull = nullptr);

    /// Show the World's committed climate. Unapplied edits are kept unless shouldReplaceDraft is true, so a
    /// document refresh never discards them; Revert and a successful Apply replace them.
    /// @param climate The World's climate member; undefined when the World has none.
    /// @param canAuthor Whether the World is an editable built-in World.
    /// @param shouldReplaceDraft Discard unapplied edits and show the committed climate.
    void SetCommittedClimate(const QJsonValue& climate, bool canAuthor, bool shouldReplaceDraft);

    /// Whether the draft differs from the committed climate.
    [[nodiscard]] bool HasPendingEdits() const noexcept;

    /// The draft climate member; undefined when the draft has no climate.
    [[nodiscard]] QJsonValue GetDraft() const;

    /// Show a message, such as a refused Apply with its repair, in this panel.
    void ShowStatus(const QString& message);

    /// The inspector's current text, for tests and accessibility.
    [[nodiscard]] QString GetInspection() const;

Q_SIGNALS:
    /// Commit the climate (undefined removes it) in one undoable World edit.
    void ApplyRequested(const QJsonValue& climate);
    /// The draft changed, so the region overlay should redraw.
    void DraftChanged();

private:
    void buildLayout();
    void rebuildFields();
    void rebuildPresetFields();
    void rebuildScheduleFields();
    void rebuildRegionFields();
    void editDraft(const QJsonObject& draft);
    void editRecord(QLatin1StringView list, qsizetype index, QLatin1StringView key, const QJsonValue& value);
    void addRecord(QLatin1StringView list);
    void removeRecord(QLatin1StringView list);
    void validateDraft();
    void updateInspection();
    void confirmRemoval();
    [[nodiscard]] QJsonArray getRecords(QLatin1StringView list) const;
    [[nodiscard]] qsizetype getSelected(QLatin1StringView list) const;

    QJsonValue mCommitted;
    QJsonObject mDraft;
    bool mHasDraft = false;
    bool mCanAuthor = false;
    bool mIsRebuilding = false;
    QString mValidationError;

    QLabel* mStatus;
    QWidget* mEmpty;
    QPushButton* mAddClimate;
    QWidget* mEditor;
    QDoubleSpinBox* mDomainMinimumX;
    QDoubleSpinBox* mDomainMinimumZ;
    QDoubleSpinBox* mDomainMaximumX;
    QDoubleSpinBox* mDomainMaximumZ;
    QComboBox* mDefaultSchedule;
    QComboBox* mPresets;
    QWidget* mPresetFields;
    QFormLayout* mPresetForm;
    QComboBox* mSchedules;
    QLineEdit* mScheduleName;
    QSpinBox* mLoopStart;
    QTableWidget* mEntries;
    QComboBox* mRegions;
    QWidget* mRegionFields;
    QFormLayout* mRegionForm;
    QSpinBox* mPreviewTick;
    QDoubleSpinBox* mInspectX;
    QDoubleSpinBox* mInspectZ;
    QLabel* mInspection;
    QPushButton* mApply;
    QPushButton* mRevert;
    QPushButton* mRemove;
};
} // namespace ludus::editor
