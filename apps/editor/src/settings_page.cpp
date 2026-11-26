#include "internal/settings_page.h"
#include "design_tokens.h"
#include "internal/theme_colour.h"
#include "internal/theme_fonts.h"

#include <QEvent>
#include <QFormLayout>
#include <QLayoutItem>
#include <QPaintEvent>
#include <QPainter>
#include <QSizePolicy>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <array>

namespace ludus::editor
{
using foundation::int32;
namespace
{
/// Longest text before ": " that still reads as a row label of a setup detail line.
constexpr int32 MAX_DETAIL_LABEL_CHARACTERS = 32;
/// Dynamic property that marks the forms a SettingsColumn aligns.
constexpr const char* SETTINGS_FORM_PROPERTY = "settingsForm";

struct SetupStatusPrefix final
{
    const char* Prefix;
    StatusTone Tone;
    const char* Chip;
};

/// The controller's setup status sentences (EditorController), by the words they start with.
constexpr std::array SETUP_STATUS_PREFIXES = std::to_array<SetupStatusPrefix>({
    {"CMake setup ready: ", StatusTone::Success, "Ready"},
    {"CMake setup needs attention: ", StatusTone::Warning, "Needs attention"},
    {"Project setup needs attention: ", StatusTone::Warning, "Needs attention"},
    {"CMake setup validation unavailable: ", StatusTone::Warning, "Unavailable"},
    {"Setup validation unavailable: ", StatusTone::Warning, "Unavailable"},
    {"Browser workspace: ", StatusTone::Neutral, "Browser"},
});

design::ThemeColour GetChipText(StatusTone tone)
{
    switch (tone)
    {
        case StatusTone::Success:
            return design::ThemeColour::StatusSuccess;
        case StatusTone::Warning:
            return design::ThemeColour::StatusWarning;
        case StatusTone::Danger:
            return design::ThemeColour::StatusDanger;
        case StatusTone::Neutral:
            break;
    }
    return design::ThemeColour::TextSecondary;
}

design::ThemeColour GetChipFill(StatusTone tone)
{
    switch (tone)
    {
        case StatusTone::Success:
            return design::ThemeColour::StatusSuccessSurface;
        case StatusTone::Warning:
            return design::ThemeColour::StatusWarningSurface;
        case StatusTone::Danger:
            return design::ThemeColour::StatusDangerSurface;
        case StatusTone::Neutral:
            break;
    }
    return design::ThemeColour::SurfaceRaised;
}

QString Capitalize(const QString& text)
{
    if (text.isEmpty() == true)
    {
        return text;
    }
    return text.front().toUpper() + text.mid(1);
}
} // namespace

StatusChip::StatusChip(QWidget* parentOrNull) : QLabel(parentOrNull)
{
    setTextFormat(Qt::PlainText);
    setAlignment(Qt::AlignCenter);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    QFont font = MakeTypeFont(this->font(), TypeRole::Caption);
    font.setBold(true);
    setFont(font);
}

void StatusChip::SetStatus(StatusTone tone, const QString& text)
{
    mTone = tone;
    setText(text);
    setAccessibleName(text);
    updateGeometry();
    update();
}

QSize StatusChip::sizeHint() const
{
    const QFontMetrics metrics = fontMetrics();
    return {metrics.horizontalAdvance(text()) + 2 * design::SPACING_SMALL,
            metrics.height() + design::SPACING_EXTRA_SMALL};
}

QSize StatusChip::minimumSizeHint() const
{
    return sizeHint();
}

void StatusChip::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(GetThemeQColour(GetChipFill(mTone)));
    const qreal radius = static_cast<qreal>(height()) / 2.0;
    painter.drawRoundedRect(QRectF(rect()), radius, radius);
    painter.setPen(GetThemeQColour(GetChipText(mTone)));
    painter.drawText(rect(), Qt::AlignCenter, text());
}

SettingsSection::SettingsSection(const QString& title, QWidget* parentOrNull)
    : QWidget(parentOrNull), mTitle(new QLabel(title, this)), mForm(new QFormLayout), mFooter(new QVBoxLayout)
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    // The rule is painted in the top margin; the title sits a small step below it.
    layout->setContentsMargins(0, design::SPACING_MEDIUM, 0, 0);
    layout->setSpacing(design::SPACING_SMALL);
    mTitle->setObjectName(QStringLiteral("settingsSectionTitle"));
    mTitle->setFont(MakeTypeFont(mTitle->font(), TypeRole::Title));
    layout->addWidget(mTitle);
    ConfigureSettingsForm(*mForm);
    mForm->setProperty(SETTINGS_FORM_PROPERTY, true);
    layout->addLayout(mForm);
    mFooter->setContentsMargins(0, 0, 0, 0);
    mFooter->setSpacing(design::SPACING_SMALL);
    layout->addLayout(mFooter);
}

void SettingsSection::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setPen(GetThemeQColour(design::ThemeColour::BorderSubtle));
    painter.drawLine(0, 0, width(), 0);
}

SettingsColumn::SettingsColumn(QWidget* parentOrNull) : QWidget(parentOrNull)
{
    setObjectName(QStringLiteral("settingsColumn"));
    setMaximumWidth(fontMetrics().averageCharWidth() * SETTINGS_COLUMN_CHARACTERS);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

void SettingsColumn::ShareLabelColumn(QFormLayout& inoutForm)
{
    ConfigureSettingsForm(inoutForm);
    mSharedForms.append(&inoutForm);
    AlignLabelColumn();
}

QList<QFormLayout*> SettingsColumn::getForms() const
{
    QList<QFormLayout*> forms = mSharedForms;
    for (QFormLayout* form : findChildren<QFormLayout*>())
    {
        if (form->property(SETTINGS_FORM_PROPERTY).toBool() == true && forms.contains(form) == false)
        {
            forms.append(form);
        }
    }
    return forms;
}

void SettingsColumn::AlignLabelColumn()
{
    const QList<QFormLayout*> forms = getForms();
    int32 width = 0;
    QList<QWidget*> labels;
    for (QFormLayout* form : forms)
    {
        for (int32 row = 0; row < form->rowCount(); ++row)
        {
            const QLayoutItem* itemOrNull = form->itemAt(row, QFormLayout::LabelRole);
            if (itemOrNull == nullptr || itemOrNull->widget() == nullptr)
            {
                continue;
            }
            QWidget* label = itemOrNull->widget();
            // Measure the text, not a width an earlier pass set.
            label->setMinimumWidth(0);
            width = std::max(width, label->sizeHint().width());
            labels.append(label);
        }
    }
    mLabelWidth = width;
    for (QWidget* label : labels)
    {
        label->setMinimumWidth(width);
    }
    setMaximumWidth(fontMetrics().averageCharWidth() * SETTINGS_COLUMN_CHARACTERS);
}

void SettingsColumn::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
    {
        AlignLabelColumn();
    }
    QWidget::changeEvent(event);
}

bool SettingsColumn::event(QEvent* event)
{
    if (event->type() == QEvent::Polish)
    {
        AlignLabelColumn();
    }
    return QWidget::event(event);
}

void MarkSelectableValue(QLabel& inoutValue)
{
    inoutValue.setTextFormat(Qt::PlainText);
    inoutValue.setWordWrap(true);
    inoutValue.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    inoutValue.setFocusPolicy(Qt::ClickFocus);
    inoutValue.setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}

void ConfigureSettingsForm(QFormLayout& inoutForm)
{
    inoutForm.setContentsMargins(0, 0, 0, 0);
    inoutForm.setHorizontalSpacing(design::SPACING_LARGE);
    inoutForm.setVerticalSpacing(design::SPACING_SMALL);
    inoutForm.setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    inoutForm.setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    inoutForm.setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    inoutForm.setRowWrapPolicy(QFormLayout::DontWrapRows);
}

ProjectSetupSummary DescribeProjectSetup(const QString& setupStatus, const QString& engineProblem)
{
    ProjectSetupSummary summary;
    QStringList lines = setupStatus.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (lines.isEmpty() == true)
    {
        summary.Chip = QStringLiteral("Not checked");
        summary.Summary = QStringLiteral("Setup has not been checked for this project.");
    }
    else
    {
        QString first = lines.takeFirst();
        summary.Chip = QStringLiteral("Status");
        for (const SetupStatusPrefix& prefix : SETUP_STATUS_PREFIXES)
        {
            const QString words = QString::fromLatin1(prefix.Prefix);
            if (first.startsWith(words) == true)
            {
                summary.Tone = prefix.Tone;
                summary.Chip = QString::fromLatin1(prefix.Chip);
                first = first.mid(words.size());
                break;
            }
        }
        if (first.startsWith(QStringLiteral("Checking")) == true)
        {
            summary.Chip = QStringLiteral("Checking");
        }
        else if (first == QStringLiteral("CMake setup check pending"))
        {
            summary.Chip = QStringLiteral("Not checked");
            first = QStringLiteral("The setup check starts in a moment.");
        }
        // A stale bootstrap is a routine update: keep the warning tone, but say "Out of date"
        // rather than "Needs attention", which reads as a broken project.
        if (first.contains(QStringLiteral("out of date"), Qt::CaseInsensitive) == true)
        {
            summary.Chip = QStringLiteral("Out of date");
            if (summary.Tone != StatusTone::Danger)
            {
                summary.Tone = StatusTone::Warning;
            }
        }
        summary.Summary = Capitalize(first);
        for (const QString& line : lines)
        {
            const int32 separator = static_cast<int32>(line.indexOf(QStringLiteral(": ")));
            if (separator > 0 && separator <= MAX_DETAIL_LABEL_CHARACTERS)
            {
                summary.Details.append({line.left(separator), line.mid(separator + 2)});
            }
            else
            {
                summary.Details.append({QString(), line});
            }
        }
    }
    if (engineProblem.isEmpty() == false)
    {
        summary.Details.append({QStringLiteral("Engine"), engineProblem});
        if (summary.Tone == StatusTone::Success || summary.Tone == StatusTone::Neutral)
        {
            summary.Tone = StatusTone::Warning;
            summary.Chip = QStringLiteral("Needs attention");
        }
    }
    return summary;
}
} // namespace ludus::editor
