#include "internal/inspector_fields.h"

#include "design_tokens.h"
#include "internal/theme_colour.h"

#include <ludus/foundation/math/scalar.hpp>

#include <QApplication>
#include <QClipboard>
#include <QColorDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QToolButton>

#include <cmath>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr float64 FINE_FACTOR = 0.1;
constexpr float64 COARSE_FACTOR = 10;
/// Decimals kept after a drag, so repeated dragging never accumulates floating-point noise.
constexpr int32 SCRUB_DECIMALS = 6;
constexpr float64 DEGREE_STEP = 1;
constexpr float64 DISTANCE_STEP = 0.01;
constexpr float64 UNIT_RANGE_STEP = 0.005;
constexpr float64 SMALL_RANGE_LIMIT = 1.01;
constexpr float64 MINIMUM_STEP = 0.001;
constexpr float64 MAXIMUM_STEP = 1;
constexpr float64 RANGE_DIVISOR = 1000;
constexpr int32 SHOWN_HEAD = 8;
constexpr int32 SHOWN_TAIL = 4;
constexpr int32 FIELD_MINIMUM_WIDTH = design::SPACING_PAGE + design::SPACING_EXTRA_SMALL;
constexpr int32 AXIS_WIDTH = design::SPACING_MEDIUM;
constexpr int32 SWATCH_WIDTH = design::SPACING_EXTRA_LARGE + design::SPACING_MEDIUM;
constexpr int32 SWATCH_HEIGHT = design::SPACING_EXTRA_LARGE;
constexpr int32 CHIP_SIZE = design::SPACING_LARGE;
constexpr int32 COPIED_FEEDBACK_MILLISECONDS = 1200;
constexpr int32 MAXIMUM_DISPLAY_DIGITS = 17;
constexpr int32 DISPLAY_CHANNEL_MAXIMUM = 255;
} // namespace

float64 ComputeScrubbedValue(float64 startValue,
                             int32 deltaPixels,
                             const ScrubSettings& settings,
                             Qt::KeyboardModifiers modifiers)
{
    float64 step = settings.Step;
    if (modifiers.testFlag(Qt::ShiftModifier) == true)
    {
        step *= FINE_FACTOR;
    }
    else if (modifiers.testFlag(Qt::ControlModifier) == true)
    {
        step *= COARSE_FACTOR;
    }
    float64 value = startValue + static_cast<float64>(deltaPixels) * step;
    if (settings.Maximum > settings.Minimum)
    {
        value = math::Clamp(value, settings.Minimum, settings.Maximum);
    }
    const float64 scale = std::pow(10.0, static_cast<float64>(SCRUB_DECIMALS));
    return std::round(value * scale) / scale;
}

float64 GetScrubStep(const QString& unit, float64 minimum, float64 maximum)
{
    const float64 range = maximum - minimum;
    if (unit == QStringLiteral("°"))
    {
        return DEGREE_STEP;
    }
    if (range > 0 && range <= SMALL_RANGE_LIMIT)
    {
        return UNIT_RANGE_STEP;
    }
    if (unit == QStringLiteral("m") || unit == QStringLiteral("×"))
    {
        return DISTANCE_STEP;
    }
    return math::Clamp(range / RANGE_DIVISOR, MINIMUM_STEP, MAXIMUM_STEP);
}

QString ShortenIdentifier(const QString& identifier)
{
    if (identifier.size() <= SHOWN_HEAD + SHOWN_TAIL + 1)
    {
        return identifier;
    }
    return identifier.left(SHOWN_HEAD) + QStringLiteral("…") + identifier.right(SHOWN_TAIL);
}

ScrubLabel::ScrubLabel(const QString& text, QLineEdit& inoutField, const ScrubSettings& settings, QWidget* parentOrNull)
    : QLabel(text, parentOrNull), mField(inoutField), mSettings(settings)
{
    setCursor(Qt::SizeHorCursor);
    setFixedWidth(AXIS_WIDTH + design::SPACING_EXTRA_SMALL);
    setAlignment(Qt::AlignCenter);
    setToolTip(QStringLiteral("Drag left or right to change the value. Shift is fine, Control is coarse."));
    setObjectName(QStringLiteral("inspectorScrub:") + text);
}

void ScrubLabel::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
    {
        QLabel::mousePressEvent(event);
        return;
    }
    bool isNumber = false;
    mStartValue = mField.text().toDouble(&isNumber);
    if (isNumber == false)
    {
        mField.setFocus();
        return;
    }
    mStartX = event->globalPosition().toPoint().x();
    mIsScrubbing = true;
    mHasMoved = false;
    event->accept();
}

void ScrubLabel::mouseMoveEvent(QMouseEvent* event)
{
    if (mIsScrubbing == false)
    {
        QLabel::mouseMoveEvent(event);
        return;
    }
    const int32 delta = event->globalPosition().toPoint().x() - mStartX;
    if (delta == 0 && mHasMoved == false)
    {
        return;
    }
    mHasMoved = true;
    const QString text = QString::number(ComputeScrubbedValue(mStartValue, delta, mSettings, event->modifiers()),
                                         'g',
                                         MAXIMUM_DISPLAY_DIGITS);
    if (text != mField.text())
    {
        mField.setText(text);
        Q_EMIT mField.textEdited(text);
        Q_EMIT Scrubbed();
    }
    event->accept();
}

void ScrubLabel::mouseReleaseEvent(QMouseEvent* event)
{
    if (mIsScrubbing == false)
    {
        QLabel::mouseReleaseEvent(event);
        return;
    }
    mIsScrubbing = false;
    if (mHasMoved == false)
    {
        mField.setFocus();
        mField.selectAll();
    }
    event->accept();
}

QWidget* MakeVectorRow(const QVector<VectorComponent>& components,
                       const QString& unit,
                       QWidget* parentOrNull,
                       const std::function<void()>& onScrubbed)
{
    QWidget* row = new QWidget(parentOrNull);
    row->setObjectName(QStringLiteral("inspectorVectorRow"));
    QHBoxLayout* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(design::SPACING_EXTRA_SMALL);
    for (const VectorComponent& component : components)
    {
        ScrubLabel* label = new ScrubLabel(component.Name, *component.FieldOrNull, component.Settings, row);
        QObject::connect(label, &ScrubLabel::Scrubbed, row, onScrubbed);
        component.FieldOrNull->setParent(row);
        component.FieldOrNull->setMinimumWidth(FIELD_MINIMUM_WIDTH);
        component.FieldOrNull->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        component.FieldOrNull->setProperty("density", QStringLiteral("compact"));
        layout->addWidget(label);
        layout->addWidget(component.FieldOrNull, 1);
    }
    if (unit.isEmpty() == false)
    {
        QLabel* unitLabel = new QLabel(unit, row);
        unitLabel->setObjectName(QStringLiteral("inspectorUnit"));
        unitLabel->setForegroundRole(QPalette::PlaceholderText);
        layout->addWidget(unitLabel);
    }
    return row;
}

// The sRGB transfer is FoundationMath's exact IEC 61966-2-1 piecewise curve (DecodeSrgb/EncodeSrgb), so the result
// does not depend on the Qt version. QColorTransform::map(QColor) changed between releases: Qt 6.12 returns
// ExtendedRgb colours, which never compare equal to the plain Rgb colours a picker or painter uses, and evaluates the
// transfer approximately (display (255, 0, 128) decoded green as 2^-24 rather than 0).
QColor ConvertLinearToDisplayColour(float64 red, float64 green, float64 blue)
{
    const auto encode = [](float64 linear) {
        return math::EncodeSrgb(static_cast<float32>(math::Clamp(linear, 0.0, 1.0)));
    };
    return QColor::fromRgbF(encode(red), encode(green), encode(blue));
}

void ConvertDisplayToLinearColour(const QColor& display, float64& outRed, float64& outGreen, float64& outBlue)
{
    const QColor rgb = display.toRgb();
    outRed = static_cast<float64>(math::DecodeSrgb(rgb.redF()));
    outGreen = static_cast<float64>(math::DecodeSrgb(rgb.greenF()));
    outBlue = static_cast<float64>(math::DecodeSrgb(rgb.blueF()));
}

ColourSwatch::ColourSwatch(QLineEdit& inoutRed, QLineEdit& inoutGreen, QLineEdit& inoutBlue, QWidget* parentOrNull)
    : QPushButton(parentOrNull), mRed(inoutRed), mGreen(inoutGreen), mBlue(inoutBlue)
{
    setObjectName(QStringLiteral("inspectorColourSwatch"));
    setAccessibleName(QStringLiteral("Colour"));
    setToolTip(QStringLiteral("Choose a colour. The fields hold linear RGB; the swatch shows it as displayed."));
    setFixedSize(SWATCH_WIDTH, SWATCH_HEIGHT);
    setCursor(Qt::PointingHandCursor);
    for (QLineEdit* field : {&mRed, &mGreen, &mBlue})
    {
        connect(field, &QLineEdit::textChanged, this, qOverload<>(&QWidget::update));
    }
    connect(this, &QPushButton::clicked, this, &ColourSwatch::chooseColour);
}

QColor ColourSwatch::GetDisplayColour() const
{
    bool isRed = false;
    bool isGreen = false;
    bool isBlue = false;
    const float64 red = mRed.text().toDouble(&isRed);
    const float64 green = mGreen.text().toDouble(&isGreen);
    const float64 blue = mBlue.text().toDouble(&isBlue);
    if (isRed == false || isGreen == false || isBlue == false)
    {
        return {};
    }
    return ConvertLinearToDisplayColour(red, green, blue);
}

void ColourSwatch::SetDisplayColour(const QColor& display)
{
    float64 red = 0;
    float64 green = 0;
    float64 blue = 0;
    ConvertDisplayToLinearColour(display, red, green, blue);
    const QVector<QPair<QLineEdit*, float64>> channels{{&mRed, red}, {&mGreen, green}, {&mBlue, blue}};
    for (const QPair<QLineEdit*, float64>& channel : channels)
    {
        const QString text = QString::number(channel.second, 'g', MAXIMUM_DISPLAY_DIGITS);
        channel.first->setText(text);
        Q_EMIT channel.first->textEdited(text);
    }
    Q_EMIT Picked();
}

void ColourSwatch::chooseColour()
{
    const QColor initial = GetDisplayColour();
    QColorDialog* picker = new QColorDialog(initial.isValid() == true ? initial : QColor(Qt::white), this);
    picker->setObjectName(QStringLiteral("inspectorColourPicker"));
    picker->setAttribute(Qt::WA_DeleteOnClose);
    connect(picker, &QColorDialog::colorSelected, this, &ColourSwatch::SetDisplayColour);
    picker->open();
}

void ColourSwatch::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const QColor colour = GetDisplayColour();
    painter.setPen(QPen(GetThemeQColour(design::ThemeColour::BorderControl), 1));
    painter.setBrush(colour.isValid() == true ? colour : GetThemeQColour(design::ThemeColour::SurfaceRaised));
    painter.drawRoundedRect(frame, design::RADIUS_CONTROL, design::RADIUS_CONTROL);
    if (hasFocus() == true)
    {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(GetThemeQColour(design::ThemeColour::AccentPrimary), 2));
        painter.drawRoundedRect(frame.adjusted(1, 1, -1, -1), design::RADIUS_CONTROL, design::RADIUS_CONTROL);
    }
}

ColourChip::ColourChip(const QColor& displayColour, QWidget* parentOrNull)
    : QWidget(parentOrNull), mColour(displayColour)
{
    setObjectName(QStringLiteral("inspectorColourChip"));
    setFixedSize(sizeHint());
}

QSize ColourChip::sizeHint() const
{
    return QSize(CHIP_SIZE, CHIP_SIZE);
}

void ColourChip::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(GetThemeQColour(design::ThemeColour::BorderControl), 1));
    painter.setBrush(mColour);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                            design::RADIUS_CONTROL,
                            design::RADIUS_CONTROL);
}

IdentifierRow::IdentifierRow(const QString& identifier, QWidget* parentOrNull)
    : QWidget(parentOrNull), mIdentifier(identifier), mText(new QLabel(ShortenIdentifier(identifier), this)),
      mCopy(new QToolButton(this))
{
    setObjectName(QStringLiteral("inspectorIdentifier"));
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(design::SPACING_SMALL);
    mText->setObjectName(QStringLiteral("inspectorIdentifierText"));
    mText->setTextFormat(Qt::PlainText);
    mText->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    mText->setToolTip(identifier);
    mCopy->setObjectName(QStringLiteral("inspectorIdentifierCopy"));
    mCopy->setText(QStringLiteral("Copy"));
    mCopy->setToolTip(QStringLiteral("Copy the full identifier"));
    mCopy->setAccessibleName(QStringLiteral("Copy identifier"));
    mCopy->setAutoRaise(true);
    layout->addWidget(mText);
    layout->addWidget(mCopy);
    layout->addStretch();
    connect(mCopy, &QToolButton::clicked, this, &IdentifierRow::Copy);
}

void IdentifierRow::Copy()
{
    QApplication::clipboard()->setText(mIdentifier);
    mCopy->setText(QStringLiteral("Copied"));
    QTimer::singleShot(COPIED_FEEDBACK_MILLISECONDS, this, [this]() { mCopy->setText(QStringLiteral("Copy")); });
}
} // namespace ludus::editor
