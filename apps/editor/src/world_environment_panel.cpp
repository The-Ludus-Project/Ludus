// Environment World tool: edits a draft of the World climate, validates it with the engine cooker on every edit
// and previews the authored weather at a tick and World position. See
// docs/modules/gameplay/weather.md#edit-a-climate-in-the-environment-tool.
#include "internal/world_environment_panel.h"

#include "internal/world_climate.h"

#include <ludus/content/scene_format/playable_world.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/gameplay/weather/climate.h>
#include <ludus/gameplay/weather/weather_simulation.h>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
using namespace Qt::StringLiterals;
namespace weather = gameplay::weather;
namespace format = content::weather_format;
using foundation::float64;
using foundation::usize;

/// Largest authored World coordinate the domain and region fields accept, in metres (the cooker's limit).
constexpr float64 MAX_COORDINATE_METRES = 10000000.0;
/// Half extent of a new climate's domain, in metres.
constexpr float64 NEW_DOMAIN_HALF_EXTENT_METRES = 1000.0;
/// A new schedule entry holds for one minute of Play.
constexpr int NEW_ENTRY_HOLD_TICKS = 3600;
/// A new region spans this fraction of the domain around its centre, with a feather of a quarter of that.
constexpr float64 NEW_REGION_DOMAIN_FRACTION = 0.2;
constexpr float64 NEW_REGION_FEATHER_FRACTION = 0.25;
/// Largest tick count a hold or blend field accepts (one day of Play).
constexpr int MAX_FIELD_TICKS = 5184000;
/// Largest preview tick (one day of Play).
constexpr int MAX_PREVIEW_TICKS = MAX_FIELD_TICKS;
constexpr int COORDINATE_DECIMALS = 2;
constexpr int ENTRY_COLUMN_COUNT = 3;
constexpr int PERCENT = 100;
/// Range of an authored region priority in the field (the cooker accepts any int32).
constexpr int MIN_PRIORITY = -1000000;
constexpr int MAX_PRIORITY = 1000000;
/// Values of a new climate's first preset.
constexpr float64 NEW_TEMPERATURE_CELSIUS = 18.0;
constexpr float64 NEW_HUMIDITY = 0.5;
constexpr float64 NEW_GROUND_WIND_METRES_PER_SECOND = 2.0;
constexpr float64 NEW_CLOUD_WIND_METRES_PER_SECOND = 6.0;
constexpr float64 NEW_CLOUD_COVERAGE = 0.2;
constexpr float64 NEW_FOG_SCALE_HEIGHT_METRES = 100.0;

/// One numeric preset field: a scalar member, or one component of a three-component wind.
struct PresetNumberField final
{
    QLatin1StringView Key;
    const char* Label;
    float64 Minimum;
    float64 Maximum;
    int Decimals;
    const char* Suffix;
    qsizetype Component;
};

constexpr usize PRESET_NUMBER_FIELD_COUNT = 14;
constexpr qsizetype SCALAR = -1;
constexpr float64 MAX_WIND = 150.0;
constexpr float64 MAX_DENSITY = 8.0;
constexpr float64 MAX_FOG_HEIGHT = 20000.0;
constexpr float64 MAX_PRECIPITATION = 500.0;
constexpr float64 MAX_TEMPERATURE = 100.0;
constexpr int FRACTION_DECIMALS = 3;
constexpr int FOG_DECIMALS = 6;
constexpr int WIND_DECIMALS = 2;
const foundation::StaticArray<PresetNumberField, PRESET_NUMBER_FIELD_COUNT> PRESET_NUMBER_FIELDS{
    PresetNumberField{"temperature"_L1, "Temperature", -MAX_TEMPERATURE, MAX_TEMPERATURE, WIND_DECIMALS, " °C", SCALAR},
    PresetNumberField{"humidity"_L1, "Humidity", 0, 1, FRACTION_DECIMALS, "", SCALAR},
    PresetNumberField{"groundWind"_L1, "Ground wind X", -MAX_WIND, MAX_WIND, WIND_DECIMALS, " m/s", 0},
    PresetNumberField{"groundWind"_L1, "Ground wind Y", -MAX_WIND, MAX_WIND, WIND_DECIMALS, " m/s", 1},
    PresetNumberField{"groundWind"_L1, "Ground wind Z", -MAX_WIND, MAX_WIND, WIND_DECIMALS, " m/s", 2},
    PresetNumberField{"cloudWind"_L1, "Cloud wind X", -MAX_WIND, MAX_WIND, WIND_DECIMALS, " m/s", 0},
    PresetNumberField{"cloudWind"_L1, "Cloud wind Y", -MAX_WIND, MAX_WIND, WIND_DECIMALS, " m/s", 1},
    PresetNumberField{"cloudWind"_L1, "Cloud wind Z", -MAX_WIND, MAX_WIND, WIND_DECIMALS, " m/s", 2},
    PresetNumberField{"cloudCoverage"_L1, "Cloud coverage", 0, 1, FRACTION_DECIMALS, "", SCALAR},
    PresetNumberField{"cloudType"_L1, "Cloud type", 0, 1, FRACTION_DECIMALS, "", SCALAR},
    PresetNumberField{"cloudDensity"_L1, "Cloud density", 0, MAX_DENSITY, FRACTION_DECIMALS, "", SCALAR},
    PresetNumberField{"fogExtinction"_L1, "Fog extinction", 0, 1, FOG_DECIMALS, " /m", SCALAR},
    PresetNumberField{"fogScaleHeight"_L1, "Fog scale height", 0, MAX_FOG_HEIGHT, WIND_DECIMALS, " m", SCALAR},
    PresetNumberField{"precipitation"_L1, "Precipitation", 0, MAX_PRECIPITATION, WIND_DECIMALS, " mm/h", SCALAR},
};

/// A record list with the chooser that selects one of its records.
struct RecordChooser final
{
    QComboBox* Chooser;
    QLatin1StringView List;
};

/// One region bounds field: its label and the bounds member it edits.
struct BoundsField final
{
    const char* Label;
    QLatin1StringView Key;
};

constexpr usize BOUNDS_FIELD_COUNT = 4;
const foundation::StaticArray<BoundsField, BOUNDS_FIELD_COUNT> REGION_BOUNDS_FIELDS{
    BoundsField{"West (min X)", "minimumX"_L1},
    BoundsField{"North (min Z)", "minimumZ"_L1},
    BoundsField{"East (max X)", "maximumX"_L1},
    BoundsField{"South (max Z)", "maximumZ"_L1},
};

QJsonObject MakeBounds(float64 minimumX, float64 minimumZ, float64 maximumX, float64 maximumZ)
{
    return {{"minimumX"_L1, minimumX}, {"minimumZ"_L1, minimumZ}, {"maximumX"_L1, maximumX}, {"maximumZ"_L1, maximumZ}};
}

QJsonObject MakeNewClimate()
{
    const QJsonObject preset{
        {"id"_L1, 1},
        {"revision"_L1, 1},
        {"name"_L1, u"Clear"_s},
        {"temperature"_L1, NEW_TEMPERATURE_CELSIUS},
        {"humidity"_L1, NEW_HUMIDITY},
        {"groundWind"_L1, QJsonArray{NEW_GROUND_WIND_METRES_PER_SECOND, 0, 0}},
        {"cloudWind"_L1, QJsonArray{NEW_CLOUD_WIND_METRES_PER_SECOND, 0, 0}},
        {"cloudCoverage"_L1, NEW_CLOUD_COVERAGE},
        {"cloudType"_L1, 0.5},
        {"cloudDensity"_L1, 1},
        {"fogExtinction"_L1, 0},
        {"fogScaleHeight"_L1, NEW_FOG_SCALE_HEIGHT_METRES},
        {"precipitation"_L1, 0},
    };
    const QJsonObject schedule{
        {"id"_L1, 1},
        {"name"_L1, u"Default"_s},
        {"loopStart"_L1, 0},
        {"entries"_L1, QJsonArray{QJsonObject{{"preset"_L1, 1}, {"hold"_L1, NEW_ENTRY_HOLD_TICKS}, {"blend"_L1, 0}}}},
    };
    const float64 extent = NEW_DOMAIN_HALF_EXTENT_METRES;
    return {
        {"version"_L1, WORLD_CLIMATE_VERSION},
        {"id"_L1, 1},
        {"revision"_L1, 1},
        {"domain"_L1, MakeBounds(-extent, -extent, extent, extent)},
        {"defaultSchedule"_L1, 1},
        {"presets"_L1, QJsonArray{preset}},
        {"schedules"_L1, QJsonArray{schedule}},
        {"regions"_L1, QJsonArray{}},
    };
}

qint64 GetNextId(const QJsonArray& records)
{
    qint64 next = 1;
    for (const QJsonValue record : records)
    {
        next = qMax(next, record.toObject().value("id"_L1).toInteger() + 1);
    }
    return next;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): a closed range, minimum then maximum.
void ConfigureNumberField(QDoubleSpinBox& inoutField,
                          const QString& objectName,
                          float64 minimum,
                          float64 maximum,
                          int decimals)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    inoutField.setObjectName(objectName);
    inoutField.setRange(minimum, maximum);
    inoutField.setDecimals(decimals);
    inoutField.setKeyboardTracking(false);
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): a closed range, minimum then maximum.
QDoubleSpinBox*
MakeNumberField(QWidget* parent, const QString& objectName, float64 minimum, float64 maximum, int decimals)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    QDoubleSpinBox* field = new QDoubleSpinBox(parent);
    ConfigureNumberField(*field, objectName, minimum, maximum, decimals);
    return field;
}

QSpinBox* MakeTickField(QWidget* parent, const QString& objectName)
{
    QSpinBox* field = new QSpinBox(parent);
    field->setObjectName(objectName);
    field->setRange(0, MAX_FIELD_TICKS);
    field->setSuffix(QStringLiteral(" ticks"));
    field->setKeyboardTracking(false);
    return field;
}

void FillScheduleChoices(QComboBox& inoutBox, const QJsonArray& schedules, qint64 selectedId)
{
    const QSignalBlocker blocker(&inoutBox);
    inoutBox.clear();
    for (const QJsonValue schedule : schedules)
    {
        const QJsonObject record = schedule.toObject();
        inoutBox.addItem(record.value("name"_L1).toString(), record.value("id"_L1).toInteger());
    }
    inoutBox.setCurrentIndex(inoutBox.findData(selectedId));
}

QString FormatPercent(foundation::int32 weight)
{
    return QStringLiteral("%1%").arg(static_cast<float64>(weight) * PERCENT / format::FRACTION_ONE, 0, 'f', 0);
}
} // namespace

WorldEnvironmentPanel::WorldEnvironmentPanel(QWidget* parentOrNull)
    : QWidget(parentOrNull), mStatus(new QLabel(this)), mEmpty(new QWidget(this)),
      mAddClimate(new QPushButton(QStringLiteral("Add Climate"), mEmpty)), mEditor(new QWidget(this)),
      mDomainMinimumX(new QDoubleSpinBox(this)), mDomainMinimumZ(new QDoubleSpinBox(this)),
      mDomainMaximumX(new QDoubleSpinBox(this)), mDomainMaximumZ(new QDoubleSpinBox(this)),
      mDefaultSchedule(new QComboBox(this)), mPresets(new QComboBox(this)), mPresetFields(new QWidget(this)),
      mPresetForm(new QFormLayout(mPresetFields)), mSchedules(new QComboBox(this)), mScheduleName(new QLineEdit(this)),
      mLoopStart(new QSpinBox(this)), mEntries(new QTableWidget(0, ENTRY_COLUMN_COUNT, this)),
      mRegions(new QComboBox(this)), mRegionFields(new QWidget(this)), mRegionForm(new QFormLayout(mRegionFields)),
      mPreviewTick(new QSpinBox(this)), mInspectX(new QDoubleSpinBox(this)), mInspectZ(new QDoubleSpinBox(this)),
      mInspection(new QLabel(this)), mApply(new QPushButton(QStringLiteral("Apply"), this)),
      mRevert(new QPushButton(QStringLiteral("Revert"), this)),
      mRemove(new QPushButton(QStringLiteral("Remove Climate…"), this))
{
    setObjectName(QStringLiteral("worldEnvironmentPanel"));
    buildLayout();
    rebuildFields();
}

void WorldEnvironmentPanel::buildLayout()
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    mStatus->setObjectName(QStringLiteral("worldEnvironmentStatus"));
    mStatus->setWordWrap(true);
    mStatus->setTextFormat(Qt::PlainText);

    QVBoxLayout* emptyLayout = new QVBoxLayout(mEmpty);
    QLabel* emptyText = new QLabel(
        QStringLiteral("This World has no climate. Add one to author regional weather presets, schedules and regions. "
                       "Without one the sky stays clear, with no clouds, fog or precipitation. That does not by itself "
                       "darken a frame, so check Lights if the frame looks dark."),
        mEmpty);
    emptyText->setObjectName(QStringLiteral("worldEnvironmentEmptyText"));
    emptyText->setWordWrap(true);
    emptyLayout->addWidget(emptyText);
    mAddClimate->setObjectName(QStringLiteral("worldEnvironmentAddClimate"));
    emptyLayout->addWidget(mAddClimate);
    emptyLayout->addStretch();
    connect(mAddClimate, &QPushButton::clicked, this, [this]() {
        mHasDraft = true;
        mDraft = MakeNewClimate();
        rebuildFields();
        editDraft(mDraft);
    });

    QScrollArea* scroll = new QScrollArea(mEditor);
    scroll->setWidgetResizable(true);
    QWidget* content = new QWidget(scroll);
    QVBoxLayout* contentLayout = new QVBoxLayout(content);
    scroll->setWidget(content);
    QVBoxLayout* editorLayout = new QVBoxLayout(mEditor);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->addWidget(scroll);

    // Domain and default schedule.
    QGroupBox* climateGroup = new QGroupBox(QStringLiteral("Climate"), content);
    QFormLayout* climateForm = new QFormLayout(climateGroup);
    const auto addDomain =
        [this, climateForm](QDoubleSpinBox* field, const char* label, const QString& name, QLatin1StringView key) {
            ConfigureNumberField(*field, name, -MAX_COORDINATE_METRES, MAX_COORDINATE_METRES, COORDINATE_DECIMALS);
            field->setSuffix(QStringLiteral(" m"));
            climateForm->addRow(QString::fromUtf8(label), field);
            connect(field, &QDoubleSpinBox::valueChanged, this, [this, key](float64 value) {
                if (mIsRebuilding == true)
                {
                    return;
                }
                QJsonObject draft = mDraft;
                QJsonObject domain = draft.value("domain"_L1).toObject();
                domain.insert(key, value);
                draft.insert("domain"_L1, domain);
                editDraft(draft);
            });
        };
    addDomain(mDomainMinimumX, "Domain west (min X)", u"worldEnvironmentDomainMinimumX"_s, "minimumX"_L1);
    addDomain(mDomainMinimumZ, "Domain north (min Z)", u"worldEnvironmentDomainMinimumZ"_s, "minimumZ"_L1);
    addDomain(mDomainMaximumX, "Domain east (max X)", u"worldEnvironmentDomainMaximumX"_s, "maximumX"_L1);
    addDomain(mDomainMaximumZ, "Domain south (max Z)", u"worldEnvironmentDomainMaximumZ"_s, "maximumZ"_L1);
    mDefaultSchedule->setObjectName(QStringLiteral("worldEnvironmentDefaultSchedule"));
    climateForm->addRow(QStringLiteral("Default schedule"), mDefaultSchedule);
    connect(mDefaultSchedule, &QComboBox::currentIndexChanged, this, [this]() {
        if (mIsRebuilding == false && mDefaultSchedule->currentIndex() >= 0)
        {
            QJsonObject draft = mDraft;
            draft.insert("defaultSchedule"_L1, mDefaultSchedule->currentData().toLongLong());
            editDraft(draft);
        }
    });
    contentLayout->addWidget(climateGroup);

    // A list of records with a chooser, Add and Remove, and the chosen record's fields.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the group's title, then its object-name stem.
    const auto addRecordGroup = [this, content, contentLayout](const QString& title,
                                                               const QString& name,
                                                               QLatin1StringView list,
                                                               QComboBox* chooser,
                                                               QWidget* fields,
                                                               QFormLayout* form) {
        QGroupBox* group = new QGroupBox(title, content);
        QVBoxLayout* groupLayout = new QVBoxLayout(group);
        QHBoxLayout* row = new QHBoxLayout;
        chooser->setObjectName(QStringLiteral("worldEnvironment%1Chooser").arg(name));
        QPushButton* add = new QPushButton(QStringLiteral("Add"), group);
        add->setObjectName(QStringLiteral("worldEnvironmentAdd%1").arg(name));
        QPushButton* remove = new QPushButton(QStringLiteral("Remove"), group);
        remove->setObjectName(QStringLiteral("worldEnvironmentRemove%1").arg(name));
        row->addWidget(chooser, 1);
        row->addWidget(add);
        row->addWidget(remove);
        groupLayout->addLayout(row);
        form->setContentsMargins(0, 0, 0, 0);
        groupLayout->addWidget(fields);
        contentLayout->addWidget(group);
        connect(add, &QPushButton::clicked, this, [this, list]() { addRecord(list); });
        connect(remove, &QPushButton::clicked, this, [this, list]() { removeRecord(list); });
        return groupLayout;
    };
    addRecordGroup(QStringLiteral("Presets"), u"Preset"_s, "presets"_L1, mPresets, mPresetFields, mPresetForm);
    connect(mPresets, &QComboBox::currentIndexChanged, this, [this]() {
        if (mIsRebuilding == false)
        {
            rebuildPresetFields();
        }
    });

    QWidget* scheduleFields = new QWidget(content);
    QFormLayout* scheduleForm = new QFormLayout(scheduleFields);
    QVBoxLayout* scheduleLayout = addRecordGroup(QStringLiteral("Schedules"),
                                                 u"Schedule"_s,
                                                 "schedules"_L1,
                                                 mSchedules,
                                                 scheduleFields,
                                                 scheduleForm);
    mScheduleName->setObjectName(QStringLiteral("worldEnvironmentScheduleName"));
    mScheduleName->setMaxLength(static_cast<int>(MAX_WORLD_CLIMATE_NAME_LENGTH));
    scheduleForm->addRow(QStringLiteral("Name"), mScheduleName);
    mLoopStart->setObjectName(QStringLiteral("worldEnvironmentLoopStart"));
    mLoopStart->setPrefix(QStringLiteral("entry "));
    mLoopStart->setKeyboardTracking(false);
    scheduleForm->addRow(QStringLiteral("Loop from"), mLoopStart);
    mEntries->setObjectName(QStringLiteral("worldEnvironmentEntries"));
    mEntries->setHorizontalHeaderLabels({QStringLiteral("Preset"), QStringLiteral("Hold"), QStringLiteral("Blend")});
    mEntries->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    mEntries->verticalHeader()->setVisible(false);
    scheduleLayout->addWidget(mEntries);
    QHBoxLayout* entryButtons = new QHBoxLayout;
    QPushButton* addEntry = new QPushButton(QStringLiteral("Add Entry"), scheduleFields);
    addEntry->setObjectName(QStringLiteral("worldEnvironmentAddEntry"));
    QPushButton* removeEntry = new QPushButton(QStringLiteral("Remove Entry"), scheduleFields);
    removeEntry->setObjectName(QStringLiteral("worldEnvironmentRemoveEntry"));
    entryButtons->addWidget(addEntry);
    entryButtons->addWidget(removeEntry);
    entryButtons->addStretch();
    scheduleLayout->addLayout(entryButtons);
    connect(mSchedules, &QComboBox::currentIndexChanged, this, [this]() {
        if (mIsRebuilding == false)
        {
            rebuildScheduleFields();
        }
    });
    connect(mScheduleName, &QLineEdit::editingFinished, this, [this]() {
        const qsizetype index = getSelected("schedules"_L1);
        if (mIsRebuilding == false && index >= 0)
        {
            editRecord("schedules"_L1, index, "name"_L1, mScheduleName->text().trimmed());
            mSchedules->setItemText(static_cast<int>(index), mScheduleName->text().trimmed());
        }
    });
    connect(mLoopStart, &QSpinBox::valueChanged, this, [this](int value) {
        const qsizetype index = getSelected("schedules"_L1);
        if (mIsRebuilding == false && index >= 0)
        {
            editRecord("schedules"_L1, index, "loopStart"_L1, value - 1);
        }
    });
    const auto editEntries = [this](const auto& change) {
        const qsizetype index = getSelected("schedules"_L1);
        if (index < 0)
        {
            return;
        }
        QJsonArray entries = getRecords("schedules"_L1)[index].toObject().value("entries"_L1).toArray();
        change(entries);
        editRecord("schedules"_L1, index, "entries"_L1, entries);
        rebuildScheduleFields();
    };
    connect(addEntry, &QPushButton::clicked, this, [this, editEntries]() {
        const qint64 preset = getRecords("presets"_L1).first().toObject().value("id"_L1).toInteger();
        editEntries([preset](QJsonArray& inoutEntries) {
            inoutEntries.append(QJsonObject{{"preset"_L1, preset}, {"hold"_L1, NEW_ENTRY_HOLD_TICKS}, {"blend"_L1, 0}});
        });
    });
    connect(removeEntry, &QPushButton::clicked, this, [this, editEntries]() {
        const int row = mEntries->currentRow() >= 0 ? mEntries->currentRow() : mEntries->rowCount() - 1;
        if (mEntries->rowCount() <= 1)
        {
            ShowStatus(QStringLiteral("A schedule needs at least one entry. Add another entry before removing this "
                                      "one, or remove the schedule."));
            return;
        }
        editEntries([row](QJsonArray& inoutEntries) { inoutEntries.removeAt(row); });
    });

    addRecordGroup(QStringLiteral("Regions"), u"Region"_s, "regions"_L1, mRegions, mRegionFields, mRegionForm);
    connect(mRegions, &QComboBox::currentIndexChanged, this, [this]() {
        if (mIsRebuilding == false)
        {
            rebuildRegionFields();
        }
    });

    // Preview and position inspector.
    QGroupBox* previewGroup = new QGroupBox(QStringLiteral("Preview"), content);
    QFormLayout* previewForm = new QFormLayout(previewGroup);
    mPreviewTick->setObjectName(QStringLiteral("worldEnvironmentPreviewTick"));
    mPreviewTick->setRange(0, MAX_PREVIEW_TICKS);
    mPreviewTick->setSuffix(QStringLiteral(" ticks"));
    previewForm->addRow(QStringLiteral("Preview time"), mPreviewTick);
    ConfigureNumberField(*mInspectX,
                         u"worldEnvironmentInspectX"_s,
                         -MAX_COORDINATE_METRES,
                         MAX_COORDINATE_METRES,
                         COORDINATE_DECIMALS);
    mInspectX->setSuffix(QStringLiteral(" m"));
    ConfigureNumberField(*mInspectZ,
                         u"worldEnvironmentInspectZ"_s,
                         -MAX_COORDINATE_METRES,
                         MAX_COORDINATE_METRES,
                         COORDINATE_DECIMALS);
    mInspectZ->setSuffix(QStringLiteral(" m"));
    previewForm->addRow(QStringLiteral("Inspect X"), mInspectX);
    previewForm->addRow(QStringLiteral("Inspect Z"), mInspectZ);
    mInspection->setObjectName(QStringLiteral("worldEnvironmentInspection"));
    mInspection->setWordWrap(true);
    mInspection->setTextFormat(Qt::PlainText);
    mInspection->setTextInteractionFlags(Qt::TextSelectableByMouse);
    previewForm->addRow(mInspection);
    contentLayout->addWidget(previewGroup);
    contentLayout->addStretch();
    connect(mPreviewTick, &QSpinBox::valueChanged, this, &WorldEnvironmentPanel::updateInspection);
    connect(mInspectX, &QDoubleSpinBox::valueChanged, this, &WorldEnvironmentPanel::updateInspection);
    connect(mInspectZ, &QDoubleSpinBox::valueChanged, this, &WorldEnvironmentPanel::updateInspection);

    QHBoxLayout* buttons = new QHBoxLayout;
    mApply->setObjectName(QStringLiteral("worldEnvironmentApply"));
    mRevert->setObjectName(QStringLiteral("worldEnvironmentRevert"));
    mRemove->setObjectName(QStringLiteral("worldEnvironmentRemoveClimate"));
    buttons->addWidget(mApply);
    buttons->addWidget(mRevert);
    buttons->addStretch();
    buttons->addWidget(mRemove);
    connect(mApply, &QPushButton::clicked, this, [this]() {
        if (mValidationError.isEmpty() == false)
        {
            ShowStatus(mValidationError);
            return;
        }
        Q_EMIT ApplyRequested(GetDraft());
    });
    connect(mRevert, &QPushButton::clicked, this, [this]() { SetCommittedClimate(mCommitted, mCanAuthor, true); });
    connect(mRemove, &QPushButton::clicked, this, &WorldEnvironmentPanel::confirmRemoval);

    layout->addWidget(mStatus);
    layout->addWidget(mEmpty);
    layout->addWidget(mEditor, 1);
    layout->addLayout(buttons);
}

void WorldEnvironmentPanel::SetCommittedClimate(const QJsonValue& climate, bool canAuthor, bool shouldReplaceDraft)
{
    const bool hasChanged = climate != mCommitted || canAuthor != mCanAuthor;
    const bool shouldKeepDraft = shouldReplaceDraft == false && HasPendingEdits() == true;
    mCommitted = climate;
    mCanAuthor = canAuthor;
    if (shouldKeepDraft == false && (hasChanged == true || shouldReplaceDraft == true))
    {
        mHasDraft = climate.isObject() == true;
        mDraft = climate.toObject();
        mStatus->clear();
        rebuildFields();
        validateDraft();
        Q_EMIT DraftChanged();
        return;
    }
    if (shouldKeepDraft == true && hasChanged == true)
    {
        ShowStatus(QStringLiteral("The World's climate changed while you were editing. Apply replaces it with this "
                                  "draft; Revert shows the World's climate."));
    }
    mApply->setEnabled(mCanAuthor == true && HasPendingEdits() == true);
    mRevert->setEnabled(HasPendingEdits() == true);
}

bool WorldEnvironmentPanel::HasPendingEdits() const noexcept
{
    return GetDraft() != mCommitted;
}

QJsonValue WorldEnvironmentPanel::GetDraft() const
{
    return mHasDraft == true ? QJsonValue(mDraft) : QJsonValue(QJsonValue::Undefined);
}

void WorldEnvironmentPanel::ShowStatus(const QString& message)
{
    mStatus->setText(message);
}

QString WorldEnvironmentPanel::GetInspection() const
{
    return mInspection->text();
}

QJsonArray WorldEnvironmentPanel::getRecords(QLatin1StringView list) const
{
    return mDraft.value(list).toArray();
}

qsizetype WorldEnvironmentPanel::getSelected(QLatin1StringView list) const
{
    const QComboBox* chooser = list == "presets"_L1 ? mPresets : list == "schedules"_L1 ? mSchedules : mRegions;
    const qsizetype index = chooser->currentIndex();
    return index >= 0 && index < getRecords(list).size() ? index : -1;
}

void WorldEnvironmentPanel::rebuildFields()
{
    mIsRebuilding = true;
    mEmpty->setVisible(mHasDraft == false);
    mEditor->setVisible(mHasDraft == true);
    mAddClimate->setEnabled(mCanAuthor == true);
    mRemove->setVisible(mHasDraft == true);
    mRemove->setEnabled(mCanAuthor == true);
    mEditor->setEnabled(mCanAuthor == true);
    if (mHasDraft == true)
    {
        const QJsonObject domain = mDraft.value("domain"_L1).toObject();
        mDomainMinimumX->setValue(domain.value("minimumX"_L1).toDouble());
        mDomainMinimumZ->setValue(domain.value("minimumZ"_L1).toDouble());
        mDomainMaximumX->setValue(domain.value("maximumX"_L1).toDouble());
        mDomainMaximumZ->setValue(domain.value("maximumZ"_L1).toDouble());
        FillScheduleChoices(*mDefaultSchedule,
                            getRecords("schedules"_L1),
                            mDraft.value("defaultSchedule"_L1).toInteger());
        for (const RecordChooser& choice : {RecordChooser{mPresets, "presets"_L1},
                                            RecordChooser{mSchedules, "schedules"_L1},
                                            RecordChooser{mRegions, "regions"_L1}})
        {
            QComboBox* chooser = choice.Chooser;
            const QLatin1StringView list = choice.List;
            const int previous = chooser->currentIndex();
            const QSignalBlocker blocker(chooser);
            chooser->clear();
            for (const QJsonValue record : getRecords(list))
            {
                chooser->addItem(record.toObject().value("name"_L1).toString());
            }
            chooser->setCurrentIndex(chooser->count() == 0 ? -1 : qBound(0, previous, chooser->count() - 1));
        }
    }
    mIsRebuilding = false;
    rebuildPresetFields();
    rebuildScheduleFields();
    rebuildRegionFields();
    mApply->setEnabled(mCanAuthor == true && HasPendingEdits() == true);
    mRevert->setEnabled(HasPendingEdits() == true);
}

void WorldEnvironmentPanel::rebuildPresetFields()
{
    while (mPresetForm->rowCount() > 0)
    {
        mPresetForm->removeRow(0);
    }
    const qsizetype index = getSelected("presets"_L1);
    if (index < 0)
    {
        return;
    }
    const QJsonObject preset = getRecords("presets"_L1)[index].toObject();
    QLineEdit* name = new QLineEdit(preset.value("name"_L1).toString(), mPresetFields);
    name->setObjectName(QStringLiteral("worldEnvironmentPresetName"));
    name->setMaxLength(static_cast<int>(MAX_WORLD_CLIMATE_NAME_LENGTH));
    mPresetForm->addRow(QStringLiteral("Name"), name);
    connect(name, &QLineEdit::editingFinished, this, [this, index, name]() {
        editRecord("presets"_L1, index, "name"_L1, name->text().trimmed());
        mPresets->setItemText(static_cast<int>(index), name->text().trimmed());
    });
    for (const PresetNumberField& field : PRESET_NUMBER_FIELDS)
    {
        QDoubleSpinBox* spin =
            MakeNumberField(mPresetFields,
                            QStringLiteral("worldEnvironmentPreset:%1:%2").arg(field.Key).arg(field.Component),
                            field.Minimum,
                            field.Maximum,
                            field.Decimals);
        spin->setSuffix(QString::fromUtf8(field.Suffix));
        const QJsonValue value = preset.value(field.Key);
        spin->setValue(field.Component == SCALAR ? value.toDouble() : value.toArray()[field.Component].toDouble());
        mPresetForm->addRow(QString::fromUtf8(field.Label), spin);
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this, index, field](float64 number) {
            if (field.Component == SCALAR)
            {
                editRecord("presets"_L1, index, field.Key, number);
                return;
            }
            QJsonArray vector = getRecords("presets"_L1)[index].toObject().value(field.Key).toArray();
            vector[field.Component] = number;
            editRecord("presets"_L1, index, field.Key, vector);
        });
    }
}

void WorldEnvironmentPanel::rebuildScheduleFields()
{
    mIsRebuilding = true;
    const qsizetype index = getSelected("schedules"_L1);
    const QJsonObject schedule = index >= 0 ? getRecords("schedules"_L1)[index].toObject() : QJsonObject{};
    const QJsonArray entries = schedule.value("entries"_L1).toArray();
    mScheduleName->setText(schedule.value("name"_L1).toString());
    mLoopStart->setRange(1, static_cast<int>(qMax<qsizetype>(1, entries.size())));
    mLoopStart->setValue(static_cast<int>(schedule.value("loopStart"_L1).toInteger()) + 1);
    mEntries->setRowCount(static_cast<int>(entries.size()));
    const QJsonArray presets = getRecords("presets"_L1);
    for (qsizetype row = 0; row < entries.size(); ++row)
    {
        const QJsonObject entry = entries[row].toObject();
        QComboBox* preset = new QComboBox(mEntries);
        preset->setObjectName(QStringLiteral("worldEnvironmentEntryPreset:%1").arg(row));
        for (const QJsonValue record : presets)
        {
            preset->addItem(record.toObject().value("name"_L1).toString(),
                            record.toObject().value("id"_L1).toInteger());
        }
        preset->setCurrentIndex(preset->findData(entry.value("preset"_L1).toInteger()));
        QSpinBox* hold = MakeTickField(mEntries, QStringLiteral("worldEnvironmentEntryHold:%1").arg(row));
        hold->setValue(static_cast<int>(entry.value("hold"_L1).toInteger()));
        QSpinBox* blend = MakeTickField(mEntries, QStringLiteral("worldEnvironmentEntryBlend:%1").arg(row));
        blend->setValue(static_cast<int>(entry.value("blend"_L1).toInteger()));
        mEntries->setCellWidget(static_cast<int>(row), 0, preset);
        mEntries->setCellWidget(static_cast<int>(row), 1, hold);
        mEntries->setCellWidget(static_cast<int>(row), 2, blend);
        const auto editEntry = [this, index, row](QLatin1StringView key, const QJsonValue& value) {
            QJsonArray current = getRecords("schedules"_L1)[index].toObject().value("entries"_L1).toArray();
            QJsonObject item = current[row].toObject();
            item.insert(key, value);
            current[row] = item;
            editRecord("schedules"_L1, index, "entries"_L1, current);
        };
        connect(preset, &QComboBox::currentIndexChanged, this, [editEntry, preset]() {
            editEntry("preset"_L1, preset->currentData().toLongLong());
        });
        connect(hold, &QSpinBox::valueChanged, this, [editEntry](int ticks) { editEntry("hold"_L1, ticks); });
        connect(blend, &QSpinBox::valueChanged, this, [editEntry](int ticks) { editEntry("blend"_L1, ticks); });
    }
    mIsRebuilding = false;
}

void WorldEnvironmentPanel::rebuildRegionFields()
{
    while (mRegionForm->rowCount() > 0)
    {
        mRegionForm->removeRow(0);
    }
    const qsizetype index = getSelected("regions"_L1);
    if (index < 0)
    {
        return;
    }
    const QJsonObject region = getRecords("regions"_L1)[index].toObject();
    QLineEdit* name = new QLineEdit(region.value("name"_L1).toString(), mRegionFields);
    name->setObjectName(QStringLiteral("worldEnvironmentRegionName"));
    name->setMaxLength(static_cast<int>(MAX_WORLD_CLIMATE_NAME_LENGTH));
    mRegionForm->addRow(QStringLiteral("Name"), name);
    connect(name, &QLineEdit::editingFinished, this, [this, index, name]() {
        editRecord("regions"_L1, index, "name"_L1, name->text().trimmed());
        mRegions->setItemText(static_cast<int>(index), name->text().trimmed());
    });
    QSpinBox* priority = new QSpinBox(mRegionFields);
    priority->setObjectName(QStringLiteral("worldEnvironmentRegionPriority"));
    priority->setRange(MIN_PRIORITY, MAX_PRIORITY);
    priority->setValue(static_cast<int>(region.value("priority"_L1).toInteger()));
    priority->setKeyboardTracking(false);
    mRegionForm->addRow(QStringLiteral("Priority"), priority);
    connect(priority, &QSpinBox::valueChanged, this, [this, index](int value) {
        editRecord("regions"_L1, index, "priority"_L1, value);
    });
    QComboBox* schedule = new QComboBox(mRegionFields);
    schedule->setObjectName(QStringLiteral("worldEnvironmentRegionSchedule"));
    FillScheduleChoices(*schedule, getRecords("schedules"_L1), region.value("schedule"_L1).toInteger());
    mRegionForm->addRow(QStringLiteral("Schedule"), schedule);
    connect(schedule, &QComboBox::currentIndexChanged, this, [this, index, schedule]() {
        editRecord("regions"_L1, index, "schedule"_L1, schedule->currentData().toLongLong());
    });
    const QJsonObject bounds = region.value("bounds"_L1).toObject();
    for (const BoundsField& field : REGION_BOUNDS_FIELDS)
    {
        const char* label = field.Label;
        const QLatin1StringView key = field.Key;
        QDoubleSpinBox* edge = MakeNumberField(mRegionFields,
                                               QStringLiteral("worldEnvironmentRegionBounds:%1").arg(key),
                                               -MAX_COORDINATE_METRES,
                                               MAX_COORDINATE_METRES,
                                               COORDINATE_DECIMALS);
        edge->setSuffix(QStringLiteral(" m"));
        edge->setValue(bounds.value(key).toDouble());
        mRegionForm->addRow(QString::fromUtf8(label), edge);
        connect(edge, &QDoubleSpinBox::valueChanged, this, [this, index, key](float64 value) {
            QJsonObject current = getRecords("regions"_L1)[index].toObject().value("bounds"_L1).toObject();
            current.insert(key, value);
            editRecord("regions"_L1, index, "bounds"_L1, current);
        });
    }
    QDoubleSpinBox* feather = MakeNumberField(mRegionFields,
                                              u"worldEnvironmentRegionFeather"_s,
                                              0,
                                              MAX_COORDINATE_METRES,
                                              COORDINATE_DECIMALS);
    feather->setSuffix(QStringLiteral(" m"));
    feather->setValue(region.value("feather"_L1).toDouble());
    mRegionForm->addRow(QStringLiteral("Feather"), feather);
    connect(feather, &QDoubleSpinBox::valueChanged, this, [this, index](float64 value) {
        editRecord("regions"_L1, index, "feather"_L1, value);
    });
}

void WorldEnvironmentPanel::editDraft(const QJsonObject& draft)
{
    mDraft = draft;
    mHasDraft = true;
    mStatus->clear();
    validateDraft();
    mApply->setEnabled(mCanAuthor == true && HasPendingEdits() == true);
    mRevert->setEnabled(HasPendingEdits() == true);
    Q_EMIT DraftChanged();
}

void WorldEnvironmentPanel::editRecord(QLatin1StringView list,
                                       qsizetype index,
                                       QLatin1StringView key,
                                       const QJsonValue& value)
{
    if (mIsRebuilding == true || index < 0)
    {
        return;
    }
    QJsonObject draft = mDraft;
    QJsonArray records = draft.value(list).toArray();
    QJsonObject record = records[index].toObject();
    record.insert(key, value);
    records[index] = record;
    draft.insert(list, records);
    editDraft(draft);
}

void WorldEnvironmentPanel::addRecord(QLatin1StringView list)
{
    QJsonObject draft = mDraft;
    QJsonArray records = draft.value(list).toArray();
    if (records.size() >= static_cast<qsizetype>(list == "presets"_L1     ? format::MAX_WEATHER_PRESETS
                                                 : list == "schedules"_L1 ? format::MAX_WEATHER_SCHEDULES
                                                                          : format::MAX_WEATHER_REGIONS))
    {
        ShowStatus(QStringLiteral("This climate already has the most %1 it can hold. Remove one first.").arg(list));
        return;
    }
    const qint64 id = GetNextId(records);
    const QString number = QString::number(id);
    QJsonObject record;
    if (list == "presets"_L1)
    {
        const qsizetype selected = getSelected(list);
        record = records[selected >= 0 ? selected : 0].toObject();
        record.insert("name"_L1, QStringLiteral("Preset %1").arg(number));
        record.insert("revision"_L1, 1);
    }
    else if (list == "schedules"_L1)
    {
        const qint64 preset = getRecords("presets"_L1).first().toObject().value("id"_L1).toInteger();
        record = {
            {"name"_L1, QStringLiteral("Schedule %1").arg(number)},
            {"loopStart"_L1, 0},
            {"entries"_L1,
             QJsonArray{QJsonObject{{"preset"_L1, preset}, {"hold"_L1, NEW_ENTRY_HOLD_TICKS}, {"blend"_L1, 0}}}},
        };
    }
    else
    {
        const QJsonObject domain = draft.value("domain"_L1).toObject();
        const float64 centerX = (domain.value("minimumX"_L1).toDouble() + domain.value("maximumX"_L1).toDouble()) / 2;
        const float64 centerZ = (domain.value("minimumZ"_L1).toDouble() + domain.value("maximumZ"_L1).toDouble()) / 2;
        const float64 halfWidth = (domain.value("maximumX"_L1).toDouble() - domain.value("minimumX"_L1).toDouble()) *
                                  NEW_REGION_DOMAIN_FRACTION / 2;
        const float64 halfDepth = (domain.value("maximumZ"_L1).toDouble() - domain.value("minimumZ"_L1).toDouble()) *
                                  NEW_REGION_DOMAIN_FRACTION / 2;
        record = {
            {"name"_L1, QStringLiteral("Region %1").arg(number)},
            {"priority"_L1, 1},
            {"schedule"_L1, draft.value("defaultSchedule"_L1).toInteger()},
            {"bounds"_L1,
             MakeBounds(centerX - halfWidth, centerZ - halfDepth, centerX + halfWidth, centerZ + halfDepth)},
            {"feather"_L1, qMin(halfWidth, halfDepth) * 2 * NEW_REGION_FEATHER_FRACTION},
        };
    }
    record.insert("id"_L1, id);
    records.append(record);
    draft.insert(list, records);
    mDraft = draft;
    rebuildFields();
    QComboBox* chooser = list == "presets"_L1 ? mPresets : list == "schedules"_L1 ? mSchedules : mRegions;
    chooser->setCurrentIndex(chooser->count() - 1);
    editDraft(draft);
}

void WorldEnvironmentPanel::removeRecord(QLatin1StringView list)
{
    const qsizetype index = getSelected(list);
    QJsonArray records = getRecords(list);
    if (index < 0)
    {
        return;
    }
    if (list != "regions"_L1 && records.size() <= 1)
    {
        ShowStatus(QStringLiteral("A climate needs at least one preset and one schedule. Add another before removing "
                                  "this one, or remove the climate."));
        return;
    }
    records.removeAt(index);
    QJsonObject draft = mDraft;
    draft.insert(list, records);
    mDraft = draft;
    rebuildFields();
    editDraft(draft);
}

void WorldEnvironmentPanel::validateDraft()
{
    mValidationError.clear();
    if (mHasDraft == true)
    {
        foundation::Array<foundation::uint8> bytes;
        QString error;
        if (TryCookWorldClimate(mDraft, bytes, error) == false)
        {
            mValidationError = error;
            ShowStatus(error);
        }
    }
    updateInspection();
}

void WorldEnvironmentPanel::updateInspection()
{
    if (mHasDraft == false)
    {
        mInspection->clear();
        return;
    }
    if (mValidationError.isEmpty() == false)
    {
        mInspection->setText(QStringLiteral("Fix the climate to preview it: %1").arg(mValidationError));
        return;
    }
    foundation::Array<foundation::uint8> bytes;
    QString error;
    weather::Climate climate;
    weather::WeatherSimulation simulation;
    weather::WeatherSample sample;
    const foundation::uint32 ticksPerSecond = content::scene_format::PLAYABLE_WORLD_TICKS_PER_SECOND;
    if (TryCookWorldClimate(mDraft, bytes, error) == false ||
        weather::TryLoadClimate({bytes.GetData(), bytes.GetSize()}, climate) != format::WeatherFormatStatus::Success ||
        weather::WeatherSimulation::TryMount(climate, ticksPerSecond, 0, simulation) !=
            weather::WeatherSimulationStatus::Success)
    {
        mInspection->setText(QStringLiteral("The climate cannot be previewed. %1").arg(error));
        return;
    }
    const weather::WeatherSimulationStatus status =
        simulation.TryPreview(weather::WeatherPreviewMode::Authored,
                              static_cast<foundation::uint64>(mPreviewTick->value()),
                              mInspectX->value(),
                              mInspectZ->value(),
                              sample);
    if (status == weather::WeatherSimulationStatus::OutsideDomain)
    {
        mInspection->setText(QStringLiteral("The inspected position lies outside the climate domain. Move it inside "
                                            "the domain or enlarge the domain."));
        return;
    }
    if (status != weather::WeatherSimulationStatus::Success)
    {
        mInspection->setText(QStringLiteral("The climate cannot be previewed at this time."));
        return;
    }
    QStringList weights;
    for (const weather::WeatherRegionWeight& weight : sample.RegionWeights)
    {
        QString name = QStringLiteral("Default");
        for (const QJsonValue region : getRecords("regions"_L1))
        {
            if (region.toObject().value("id"_L1).toInteger() == weight.RegionId)
            {
                name = region.toObject().value("name"_L1).toString();
            }
        }
        weights.append(QStringLiteral("%1 %2").arg(name, FormatPercent(weight.Weight)));
    }
    const weather::WeatherConditions conditions = weather::ConvertWeatherChannels(sample.Channels);
    mInspection->setText(QStringLiteral("At %1 s: %2\n%3 °C, humidity %4, cloud coverage %5, precipitation %6 mm/h\n"
                                        "Ground wind (%7, %8, %9) m/s, fog %10 /m")
                             .arg(static_cast<float64>(mPreviewTick->value()) / ticksPerSecond, 0, 'f', 1)
                             .arg(weights.join(", "_L1))
                             .arg(conditions.TemperatureCelsius, 0, 'f', 1)
                             .arg(FormatPercent(sample.Channels.Get(format::WeatherChannel::RelativeHumidity)))
                             .arg(FormatPercent(sample.Channels.Get(format::WeatherChannel::CloudCoverage)))
                             .arg(conditions.PrecipitationMillimetresPerHour, 0, 'f', 2)
                             .arg(conditions.GroundWindWorld.X, 0, 'f', 1)
                             .arg(conditions.GroundWindWorld.Y, 0, 'f', 1)
                             .arg(conditions.GroundWindWorld.Z, 0, 'f', 1)
                             .arg(conditions.FogExtinctionPerMetre, 0, 'g', FOG_DECIMALS));
}

void WorldEnvironmentPanel::confirmRemoval()
{
    if (mHasDraft == false)
    {
        return;
    }
    const QJsonObject climate = mDraft;
    QMessageBox confirmation(QMessageBox::Question,
                             QStringLiteral("Remove Climate"),
                             QStringLiteral("Remove this World's climate, with its %1 presets, %2 schedules and %3 "
                                            "regions? The World will have no weather. Undo restores it.")
                                 .arg(climate.value("presets"_L1).toArray().size())
                                 .arg(climate.value("schedules"_L1).toArray().size())
                                 .arg(climate.value("regions"_L1).toArray().size()),
                             QMessageBox::Cancel,
                             this);
    QPushButton* remove = confirmation.addButton(QStringLiteral("Remove Climate"), QMessageBox::AcceptRole);
    remove->setObjectName(QStringLiteral("worldEnvironmentConfirmRemove"));
    confirmation.setDefaultButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != remove)
    {
        return;
    }
    Q_EMIT ApplyRequested(QJsonValue::Undefined);
}
} // namespace ludus::editor
