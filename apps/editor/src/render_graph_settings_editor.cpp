#include "internal/render_graph_settings_editor.h"

#include <ludus/foundation/base/types.h>

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QJsonObject>
#include <QLabel>
#include <QSizePolicy>
#include <QSpinBox>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr qsizetype MAX_SETTINGS = LUDUS_EDITOR_MAX_SETTINGS;
constexpr int32 MAX_SETTING_VALUE = LUDUS_EDITOR_MAX_NUMERIC_VALUE;
constexpr int32 SETTING_DECIMAL_PLACES = 6;
constexpr float64 SETTING_STEP = 0.01;
constexpr qsizetype MAX_SETTING_LABEL_LENGTH = LUDUS_EDITOR_MAX_SETTING_LABEL_LENGTH;
} // namespace
RenderGraphSettingsEditor::RenderGraphSettingsEditor(const QJsonArray& settings, QWidget* parentOrNull)
    : QWidget(parentOrNull)
{
    setObjectName(QStringLiteral("renderGraphSettingsEditor"));
    QFormLayout* fields = new QFormLayout(this);
    fields->setRowWrapPolicy(QFormLayout::WrapAllRows);
    fields->setLabelAlignment(Qt::AlignLeft);
    fields->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    fields->setContentsMargins(0, 0, 0, 0);
    QLabel* help =
        new QLabel(QStringLiteral("Settings are shader constants. Save and rebuild to apply changes."), this);
    help->setWordWrap(true);
    help->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    fields->addRow(help);
    if (settings.size() > MAX_SETTINGS)
    {
        help->setText(QStringLiteral("Too many settings. Validate and repair the manifest."));
        return;
    }
    for (const QJsonValueConstRef entry : settings)
    {
        const QJsonObject setting = entry.toObject();
        const QString identity = setting.value(QStringLiteral("id")).toString();
        const QString label = setting.value(QStringLiteral("label")).toString();
        const QString type = setting.value(QStringLiteral("type")).toString();
        const QJsonValue value = setting.value(QStringLiteral("value"));
        if (identity.isEmpty() == true || label.isEmpty() == true || label.size() > MAX_SETTING_LABEL_LENGTH)
        {
            continue;
        }
        if (type == QStringLiteral("Bool") && value.isBool() == true)
        {
            QCheckBox* control = new QCheckBox(this);
            control->setObjectName(QStringLiteral("renderGraphSetting_") + identity);
            control->setChecked(value.toBool());
            fields->addRow(label, control);
            connect(control, &QCheckBox::toggled, this, [this, identity](bool isChecked) {
                Q_EMIT SettingEdited(identity, isChecked);
            });
            continue;
        }
        const QJsonValue minimum = setting.value(QStringLiteral("minimum"));
        const QJsonValue maximum = setting.value(QStringLiteral("maximum"));
        if (minimum.isDouble() == false || maximum.isDouble() == false || value.isDouble() == false ||
            minimum.toDouble() < 0 || maximum.toDouble() > MAX_SETTING_VALUE || minimum.toDouble() > value.toDouble() ||
            value.toDouble() > maximum.toDouble())
        {
            continue;
        }
        if (type == QStringLiteral("UInt32"))
        {
            if (minimum.toDouble() != static_cast<float64>(minimum.toInteger()) ||
                maximum.toDouble() != static_cast<float64>(maximum.toInteger()) ||
                value.toDouble() != static_cast<float64>(value.toInteger()))
            {
                continue;
            }
            QSpinBox* control = new QSpinBox(this);
            control->setObjectName(QStringLiteral("renderGraphSetting_") + identity);
            control->setRange(minimum.toInt(), maximum.toInt());
            control->setValue(value.toInt());
            fields->addRow(label, control);
            connect(control, &QSpinBox::editingFinished, this, [this, identity, control]() {
                Q_EMIT SettingEdited(identity, control->value());
            });
        }
        else if (type == QStringLiteral("Float32"))
        {
            QDoubleSpinBox* control = new QDoubleSpinBox(this);
            control->setObjectName(QStringLiteral("renderGraphSetting_") + identity);
            control->setDecimals(SETTING_DECIMAL_PLACES);
            control->setRange(minimum.toDouble(), maximum.toDouble());
            control->setSingleStep(SETTING_STEP);
            control->setValue(value.toDouble());
            fields->addRow(label, control);
            connect(control, &QDoubleSpinBox::editingFinished, this, [this, identity, control]() {
                Q_EMIT SettingEdited(identity, control->value());
            });
        }
    }
    for (int32 row = 0; row < fields->rowCount(); ++row)
    {
        QLayoutItem* itemOrNull = fields->itemAt(row, QFormLayout::LabelRole);
        if (itemOrNull != nullptr)
        {
            QLabel* labelOrNull = qobject_cast<QLabel*>(itemOrNull->widget());
            if (labelOrNull != nullptr)
            {
                labelOrNull->setWordWrap(true);
                labelOrNull->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
            }
        }
    }
}
} // namespace ludus::editor
