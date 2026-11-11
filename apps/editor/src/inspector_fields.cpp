#include "internal/inspector_fields.h"

#include "design_tokens.h"
#include "internal/theme_colour.h"

#include <ludus/foundation/math/scalar.hpp>

#include <QApplication>
#include <QClipboard>
#include <QColorDialog>
#include <QEvent>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionFrame>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>

#include <algorithm>
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
/// The shortest value a number field always has room for; it widens beyond this to fit the value it holds.
constexpr QStringView NUMBER_FIELD_SAMPLE = u"-0.000";
/// Pixels QLineEdit keeps between its contents rectangle and its text on each side (QLineEditPrivate's horizontal
/// margin), which no style reports.
constexpr int32 LINE_EDIT_TEXT_MARGIN = 2;
/// Characters an elided label keeps at each end at its narrowest, so a shortened reference still reads.
constexpr int32 ELIDED_KEPT_CHARACTERS = 3;
constexpr int32 AXIS_WIDTH = design::SPACING_MEDIUM;
constexpr int32 SWATCH_WIDTH = design::SPACING_EXTRA_LARGE + design::SPACING_MEDIUM;
constexpr int32 SWATCH_HEIGHT = design::SPACING_EXTRA_LARGE;
constexpr int32 CHIP_SIZE = design::SPACING_LARGE;
constexpr int32 COPIED_FEEDBACK_MILLISECONDS = 1200;
/// Widest range, in steps, a slider spreads linearly; 1000 positions then move at most ten steps each.
constexpr float64 LINEAR_SLIDER_STEP_LIMIT = 10000;
/// Significant digits a logarithmic slider keeps, so "0.00314" rather than "0.0031415926".
constexpr int32 LOGARITHMIC_SLIDER_DIGITS = 3;
/// Decimals a linear slider never exceeds, whatever its step.
constexpr int32 MAXIMUM_SLIDER_DECIMALS = 6;
constexpr float64 DECIMAL_BASE = 10;
/// Page Up and Page Down move the handle a tenth of its travel.
constexpr int32 SLIDER_PAGE_COUNT = 10;
constexpr int32 SLIDER_STRETCH = 3;
constexpr int32 SLIDER_FIELD_STRETCH = 2;

/// Nearest integer, halves away from zero.
float64 RoundToInteger(float64 value)
{
    return value < 0 ? -math::Floor(-value + 0.5) : math::Floor(value + 0.5);
}

/// Power of ten of value's leading digit: 2 for 314, -3 for 0.00314.
int32 GetDecimalExponent(float64 magnitude)
{
    int32 exponent = static_cast<int32>(math::Floor(math::Log(magnitude) / math::Log(DECIMAL_BASE)));
    // The quotient of logarithms can land just below an exact power of ten.
    if (math::Pow(DECIMAL_BASE, static_cast<float64>(exponent + 1)) <= magnitude)
    {
        ++exponent;
    }
    else if (math::Pow(DECIMAL_BASE, static_cast<float64>(exponent)) > magnitude)
    {
        --exponent;
    }
    return exponent;
}

/// value rounded to a multiple of 10^-decimals. Only exact powers of ten scale it (dividing by 10^d rather than
/// multiplying by 10^-d), so the result is the double nearest the decimal and prints without residue.
float64 RoundToDecimals(float64 value, int32 decimals)
{
    if (decimals < 0)
    {
        const float64 unit = math::Pow(DECIMAL_BASE, static_cast<float64>(-decimals));
        return RoundToInteger(value / unit) * unit;
    }
    const float64 scale = math::Pow(DECIMAL_BASE, static_cast<float64>(decimals));
    return RoundToInteger(value * scale) / scale;
}

/// Signed logarithmic coordinate of value: linear within step of zero, logarithmic beyond.
float64 ComputeSignedLogarithm(float64 value, float64 step)
{
    const float64 magnitude = math::Log(1 + math::Abs(value) / step);
    return value < 0 ? -magnitude : magnitude;
}

float64 InvertSignedLogarithm(float64 coordinate, float64 step)
{
    const float64 magnitude = step * (math::Exp(math::Abs(coordinate)) - 1);
    return coordinate < 0 ? -magnitude : magnitude;
}

/// Where value lies along the mapping's scale, as a fraction 0 to 1 of the range.
float64 ComputeSliderFraction(float64 value, const SliderMapping& mapping)
{
    const float64 clamped = math::Clamp(value, mapping.Minimum, mapping.Maximum);
    if (mapping.Scale == SliderScale::Linear)
    {
        return (clamped - mapping.Minimum) / (mapping.Maximum - mapping.Minimum);
    }
    const float64 low = ComputeSignedLogarithm(mapping.Minimum, mapping.Step);
    const float64 high = ComputeSignedLogarithm(mapping.Maximum, mapping.Step);
    return (ComputeSignedLogarithm(clamped, mapping.Step) - low) / (high - low);
}
} // namespace

QString FormatInspectorNumber(float64 value)
{
    return QString::number(value, 'g', QLocale::FloatingPointShortest);
}

bool HasSliderRange(float64 minimum, float64 maximum)
{
    return math::IsFinite(minimum) == true && math::IsFinite(maximum) == true && maximum > minimum;
}

SliderMapping MakeSliderMapping(float64 minimum, float64 maximum, float64 step, bool isInteger)
{
    const float64 sliderStep = isInteger == true ? math::Max(step, 1.0) : step;
    const bool isNarrow = (maximum - minimum) / sliderStep <= LINEAR_SLIDER_STEP_LIMIT;
    return
    {
        .Minimum = minimum,
        .Maximum = maximum,
        .Step = sliderStep,
        .Scale = isNarrow == true ? SliderScale::Linear : SliderScale::Logarithmic,
        .IsInteger = isInteger,
    };
}

float64 ConvertSliderPositionToValue(int32 position, const SliderMapping& mapping)
{
    if (position <= 0)
    {
        return mapping.Minimum;
    }
    if (position >= SLIDER_POSITION_COUNT)
    {
        return mapping.Maximum;
    }
    const float64 fraction = static_cast<float64>(position) / static_cast<float64>(SLIDER_POSITION_COUNT);
    float64 value = 0;
    if (mapping.Scale == SliderScale::Linear)
    {
        value = mapping.Minimum + fraction * (mapping.Maximum - mapping.Minimum);
        const int32 decimals = math::Clamp(-GetDecimalExponent(mapping.Step), 0, MAXIMUM_SLIDER_DECIMALS);
        value = RoundToDecimals(value, decimals);
    }
    else
    {
        const float64 low = ComputeSignedLogarithm(mapping.Minimum, mapping.Step);
        const float64 high = ComputeSignedLogarithm(mapping.Maximum, mapping.Step);
        value = InvertSignedLogarithm(low + fraction * (high - low), mapping.Step);
        if (value != 0)
        {
            const int32 decimals = LOGARITHMIC_SLIDER_DIGITS - 1 - GetDecimalExponent(math::Abs(value));
            value = RoundToDecimals(value, decimals);
        }
    }
    if (mapping.IsInteger == true)
    {
        value = RoundToInteger(value);
    }
    return math::Clamp(value, mapping.Minimum, mapping.Maximum);
}

int32 ConvertValueToSliderPosition(float64 value, const SliderMapping& mapping)
{
    const float64 position = RoundToInteger(ComputeSliderFraction(value, mapping) * SLIDER_POSITION_COUNT);
    return math::Clamp(static_cast<int32>(position), 0, SLIDER_POSITION_COUNT);
}

BoundedSlider::BoundedSlider(QLineEdit& inoutField, const SliderMapping& mapping, QWidget* parentOrNull)
    : QSlider(Qt::Horizontal, parentOrNull), mField(inoutField), mMapping(mapping)
{
    setObjectName(QStringLiteral("inspectorSlider"));
    setRange(0, SLIDER_POSITION_COUNT);
    setPageStep(SLIDER_POSITION_COUNT / SLIDER_PAGE_COUNT);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(inoutField.accessibleName() + QStringLiteral(" slider"));
    QString tip = QStringLiteral("Drag to choose a value from %1 to %2.")
                      .arg(FormatInspectorNumber(mapping.Minimum), FormatInspectorNumber(mapping.Maximum));
    if (mapping.Scale == SliderScale::Logarithmic)
    {
        tip += QStringLiteral(" The scale is logarithmic: small values get fine steps. Type in the field for an "
                              "exact value.");
    }
    setToolTip(tip);
    setEnabled(inoutField.isEnabled());
    followField();
    connect(&mField, &QLineEdit::textChanged, this, &BoundedSlider::followField);
    connect(this, &QSlider::valueChanged, this, &BoundedSlider::writeField);
    mField.installEventFilter(this);
}

bool BoundedSlider::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == &mField && event->type() == QEvent::EnabledChange)
    {
        setEnabled(mField.isEnabled());
    }
    return QSlider::eventFilter(watched, event);
}

void BoundedSlider::wheelEvent(QWheelEvent* event)
{
    if (hasFocus() == false)
    {
        event->ignore();
        return;
    }
    QSlider::wheelEvent(event);
}

void BoundedSlider::followField()
{
    if (mIsWriting == true)
    {
        return;
    }
    bool isNumber = false;
    const float64 value = mField.text().toDouble(&isNumber);
    if (isNumber == false || math::IsFinite(value) == false)
    {
        return;
    }
    const QSignalBlocker blocker(this);
    setValue(ConvertValueToSliderPosition(value, mMapping));
}

void BoundedSlider::writeField(int32 position)
{
    const QString text = FormatInspectorNumber(ConvertSliderPositionToValue(position, mMapping));
    if (text == mField.text())
    {
        return;
    }
    mIsWriting = true;
    mField.setText(text);
    Q_EMIT mField.textEdited(text);
    mIsWriting = false;
}

QWidget* MakeSliderRow(QLineEdit& inoutField, const SliderMapping& mapping, QWidget* parentOrNull)
{
    QWidget* row = new QWidget(parentOrNull);
    row->setObjectName(QStringLiteral("inspectorSliderRow"));
    QHBoxLayout* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(design::SPACING_SMALL);
    inoutField.setParent(row);
    SetNumberFieldMinimumWidth(inoutField);
    inoutField.setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    BoundedSlider* slider = new BoundedSlider(inoutField, mapping, row);
    slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addWidget(slider, SLIDER_STRETCH);
    layout->addWidget(&inoutField, SLIDER_FIELD_STRETCH);
    return row;
}

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

int32 ComputeLineEditWidth(const QLineEdit& field, const QString& text)
{
    QStyleOptionFrame option;
    option.initFrom(&field);
    option.rect = field.contentsRect();
    option.lineWidth =
        field.hasFrame() == true ? field.style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &option, &field) : 0;
    option.midLineWidth = 0;
    option.state |= QStyle::State_Sunken;
    option.features = QStyleOptionFrame::None;
    const QMargins margins = field.textMargins();
    const int32 cursorWidth = field.style()->pixelMetric(QStyle::PM_TextCursorWidth, &option, &field);
    const QFontMetrics metrics = field.fontMetrics();
    const int32 contentsWidth =
        metrics.horizontalAdvance(text) + margins.left() + margins.right() + 2 * LINE_EDIT_TEXT_MARGIN + cursorWidth;
    const QSize contents(contentsWidth, metrics.height() + margins.top() + margins.bottom());
    return field.style()->sizeFromContents(QStyle::CT_LineEdit, &option, contents, &field).width();
}

void SetNumberFieldMinimumWidth(QLineEdit& inoutField)
{
    const auto fit = [&inoutField]() {
        const int32 shortest = ComputeLineEditWidth(inoutField, NUMBER_FIELD_SAMPLE.toString());
        inoutField.setMinimumWidth(std::max(shortest, ComputeLineEditWidth(inoutField, inoutField.text())));
    };
    fit();
    // Connected once: the property marks a field whose width already follows its text.
    if (inoutField.property("isFittedToValue").toBool() == false)
    {
        inoutField.setProperty("isFittedToValue", true);
        QObject::connect(&inoutField, &QLineEdit::textChanged, &inoutField, fit);
    }
}

namespace
{
/// Puts every label of a form above its field while the field column beside the widest label would be too narrow
/// for a number and a colour chip, so a long label such as "Reflectance (linear RGB)" does not squeeze every value
/// into a sliver. Labels above their fields take the caption colour, so the values stand out.
class FormWrapFilter final : public QObject
{
public:
    FormWrapFilter(QFormLayout& inoutForm, QWidget& inoutOwner) : QObject(&inoutOwner), mForm(inoutForm)
    {
        inoutOwner.installEventFilter(this);
    }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest)
        {
            update(*static_cast<QWidget*>(watched));
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void update(const QWidget& owner)
    {
        int32 widestLabel = 0;
        for (int32 row = 0; row < mForm.rowCount(); ++row)
        {
            const QLayoutItem* labelOrNull = mForm.itemAt(row, QFormLayout::LabelRole);
            if (labelOrNull != nullptr && labelOrNull->isEmpty() == false)
            {
                widestLabel = std::max(widestLabel, labelOrNull->sizeHint().width());
            }
        }
        const QMargins margins = mForm.contentsMargins();
        const int32 width = owner.contentsRect().width() - margins.left() - margins.right();
        const int32 fieldColumn = width - widestLabel - std::max(mForm.horizontalSpacing(), 0);
        const int32 readableField =
            owner.fontMetrics().horizontalAdvance(NUMBER_FIELD_SAMPLE.toString()) + design::SPACING_PAGE;
        const bool shouldWrapAll = widestLabel > 0 && fieldColumn < readableField;
        const QFormLayout::RowWrapPolicy policy =
            shouldWrapAll == true ? QFormLayout::WrapAllRows : QFormLayout::WrapLongRows;
        if (mForm.rowWrapPolicy() == policy)
        {
            return;
        }
        mForm.setRowWrapPolicy(policy);
        for (int32 row = 0; row < mForm.rowCount(); ++row)
        {
            const QLayoutItem* labelOrNull = mForm.itemAt(row, QFormLayout::LabelRole);
            QWidget* labelWidgetOrNull = labelOrNull != nullptr ? labelOrNull->widget() : nullptr;
            if (labelWidgetOrNull != nullptr)
            {
                labelWidgetOrNull->setForegroundRole(shouldWrapAll == true ? QPalette::PlaceholderText
                                                                           : QPalette::WindowText);
            }
        }
    }

    QFormLayout& mForm;
};
} // namespace

void ConfigureInspectorForm(QFormLayout& inoutForm)
{
    inoutForm.setRowWrapPolicy(QFormLayout::WrapLongRows);
    inoutForm.setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    QWidget* ownerOrNull = inoutForm.parentWidget();
    if (ownerOrNull != nullptr)
    {
        static_cast<void>(new FormWrapFilter(inoutForm, *ownerOrNull));
    }
}

WrappingRowLayout::WrappingRowLayout(QWidget* parentOrNull, int32 spacingPixels)
    : QLayout(parentOrNull), mSpacingPixels(spacingPixels)
{
    setContentsMargins(0, 0, 0, 0);
}

WrappingRowLayout::~WrappingRowLayout()
{
    for (QLayoutItem* item : mItems)
    {
        delete item;
    }
}

void WrappingRowLayout::AddWidget(QWidget& inoutWidget, int32 stretch)
{
    addChildWidget(&inoutWidget);
    mItems.append(new QWidgetItem(&inoutWidget));
    mStretches.append(stretch);
    invalidate();
}

void WrappingRowLayout::addItem(QLayoutItem* item)
{
    mItems.append(item);
    mStretches.append(0);
    invalidate();
}

int WrappingRowLayout::count() const
{
    return static_cast<int>(mItems.size());
}

QLayoutItem* WrappingRowLayout::itemAt(int index) const
{
    return index >= 0 && index < mItems.size() ? mItems[index] : nullptr;
}

QLayoutItem* WrappingRowLayout::takeAt(int index)
{
    if (index < 0 || index >= mItems.size())
    {
        return nullptr;
    }
    mStretches.removeAt(index);
    QLayoutItem* item = mItems.takeAt(index);
    invalidate();
    return item;
}

QSize WrappingRowLayout::sizeHint() const
{
    int32 width = 0;
    int32 height = 0;
    int32 shown = 0;
    for (const QLayoutItem* item : mItems)
    {
        if (item->isEmpty() == true)
        {
            continue;
        }
        width += item->sizeHint().width();
        height = std::max(height, item->sizeHint().height());
        ++shown;
    }
    const QMargins margins = contentsMargins();
    width += std::max(shown - 1, 0) * mSpacingPixels + margins.left() + margins.right();
    return QSize(width, height + margins.top() + margins.bottom());
}

QSize WrappingRowLayout::minimumSize() const
{
    // Stacked, the layout is as narrow as its widest item.
    int32 width = 0;
    int32 height = 0;
    for (const QLayoutItem* item : mItems)
    {
        if (item->isEmpty() == true)
        {
            continue;
        }
        width = std::max(width, item->minimumSize().width());
        height = std::max(height, item->minimumSize().height());
    }
    const QMargins margins = contentsMargins();
    return QSize(width + margins.left() + margins.right(), height + margins.top() + margins.bottom());
}

Qt::Orientations WrappingRowLayout::expandingDirections() const
{
    return Qt::Horizontal;
}

bool WrappingRowLayout::hasHeightForWidth() const
{
    return true;
}

int WrappingRowLayout::heightForWidth(int width) const
{
    const QMargins margins = contentsMargins();
    const QRect area(0, 0, width - margins.left() - margins.right(), 0);
    return arrange(area, false) + margins.top() + margins.bottom();
}

void WrappingRowLayout::setGeometry(const QRect& rectangle)
{
    QLayout::setGeometry(rectangle);
    const QRect area = rectangle.marginsRemoved(contentsMargins());
    mIsStacked = fitsInOneRow(area.width()) == false;
    static_cast<void>(arrange(area, true));
}

bool WrappingRowLayout::fitsInOneRow(int32 width) const
{
    int32 needed = 0;
    int32 shown = 0;
    for (const QLayoutItem* item : mItems)
    {
        if (item->isEmpty() == false)
        {
            needed += item->minimumSize().width();
            ++shown;
        }
    }
    return needed + std::max(shown - 1, 0) * mSpacingPixels <= width;
}

int32 WrappingRowLayout::arrange(const QRect& area, bool shouldPlace) const
{
    QVector<qsizetype> shown;
    for (qsizetype i = 0; i < mItems.size(); ++i)
    {
        if (mItems[i]->isEmpty() == false)
        {
            shown.append(i);
        }
    }
    if (shown.isEmpty() == true)
    {
        return 0;
    }
    const auto getHeight = [](const QLayoutItem& item, int32 width) {
        return item.hasHeightForWidth() == true ? item.heightForWidth(width) : item.sizeHint().height();
    };
    if (fitsInOneRow(area.width()) == false)
    {
        int32 y = area.top();
        for (const qsizetype index : shown)
        {
            QLayoutItem* item = mItems[index];
            const int32 width = std::min(area.width(), item->maximumSize().width());
            const int32 height = getHeight(*item, width);
            if (shouldPlace == true)
            {
                item->setGeometry(QRect(area.left(), y, width, height));
            }
            y += height + mSpacingPixels;
        }
        return y - mSpacingPixels - area.top();
    }
    // One row: every item starts at its minimum width; items without stretch grow to their size hint, then the rest
    // of the width is shared by stretch.
    QVector<int32> widths;
    int32 spare = area.width() - std::max(static_cast<int32>(shown.size()) - 1, 0) * mSpacingPixels;
    int32 totalStretch = 0;
    for (const qsizetype index : shown)
    {
        widths.append(mItems[index]->minimumSize().width());
        spare -= widths.last();
        totalStretch += mStretches[index];
    }
    for (qsizetype i = 0; i < shown.size(); ++i)
    {
        const QLayoutItem* item = mItems[shown[i]];
        if (mStretches[shown[i]] == 0 && spare > 0)
        {
            const int32 growth = std::min(spare, std::max(item->sizeHint().width() - widths[i], 0));
            widths[i] += growth;
            spare -= growth;
        }
    }
    if (totalStretch > 0 && spare > 0)
    {
        const int32 shared = spare;
        for (qsizetype i = 0; i < shown.size(); ++i)
        {
            const int32 share = shared * mStretches[shown[i]] / totalStretch;
            widths[i] = std::min(widths[i] + share, mItems[shown[i]]->maximumSize().width());
        }
    }
    int32 rowHeight = 0;
    for (qsizetype i = 0; i < shown.size(); ++i)
    {
        rowHeight = std::max(rowHeight, getHeight(*mItems[shown[i]], widths[i]));
    }
    if (shouldPlace == true)
    {
        int32 x = area.left();
        for (qsizetype i = 0; i < shown.size(); ++i)
        {
            QLayoutItem* item = mItems[shown[i]];
            const int32 height = std::min(rowHeight, item->maximumSize().height());
            item->setGeometry(QRect(x, area.top() + (rowHeight - height) / 2, widths[i], height));
            x += widths[i] + mSpacingPixels;
        }
    }
    return rowHeight;
}

ElidedLabel::ElidedLabel(const QString& text, QWidget* parentOrNull) : QLabel(text, parentOrNull)
{
    setTextFormat(Qt::PlainText);
    setWordWrap(false);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

bool ElidedLabel::IsElided() const
{
    return GetShownText() != text();
}

QString ElidedLabel::GetShownText() const
{
    return fontMetrics().elidedText(text(), Qt::ElideMiddle, contentsRect().width() - 2 * margin() - indent());
}

QSize ElidedLabel::minimumSizeHint() const
{
    const QString whole = text();
    if (whole.size() <= 2 * ELIDED_KEPT_CHARACTERS)
    {
        return QLabel::minimumSizeHint();
    }
    const QString shortest =
        whole.left(ELIDED_KEPT_CHARACTERS) + QStringLiteral("…") + whole.right(ELIDED_KEPT_CHARACTERS);
    const QMargins margins = contentsMargins();
    return QSize(fontMetrics().horizontalAdvance(shortest) + 2 * margin() + margins.left() + margins.right(),
                 QLabel::minimumSizeHint().height());
}

void ElidedLabel::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    const QRect area = contentsRect().adjusted(margin(), margin(), -margin(), -margin());
    style()->drawItemText(&painter, area, alignment(), palette(), isEnabled(), GetShownText(), foregroundRole());
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
    const QString text = FormatInspectorNumber(ComputeScrubbedValue(mStartValue, delta, mSettings, event->modifiers()));
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
    WrappingRowLayout* layout = new WrappingRowLayout(row, design::SPACING_SMALL);
    for (qsizetype i = 0; i < components.size(); ++i)
    {
        const VectorComponent& component = components[i];
        // Each letter stays beside its field, so a stacked row still reads "X 0", "Y 1", "Z 0".
        QWidget* pair = new QWidget(row);
        pair->setObjectName(QStringLiteral("inspectorVectorPair:") + component.Name);
        QHBoxLayout* pairLayout = new QHBoxLayout(pair);
        pairLayout->setContentsMargins(0, 0, 0, 0);
        pairLayout->setSpacing(design::SPACING_EXTRA_SMALL);
        ScrubLabel* label = new ScrubLabel(component.Name, *component.FieldOrNull, component.Settings, pair);
        QObject::connect(label, &ScrubLabel::Scrubbed, row, onScrubbed);
        component.FieldOrNull->setParent(pair);
        component.FieldOrNull->setProperty("density", QStringLiteral("compact"));
        SetNumberFieldMinimumWidth(*component.FieldOrNull);
        component.FieldOrNull->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        pairLayout->addWidget(label);
        pairLayout->addWidget(component.FieldOrNull, 1);
        if (i + 1 == components.size() && unit.isEmpty() == false)
        {
            QLabel* unitLabel = new QLabel(unit, pair);
            unitLabel->setObjectName(QStringLiteral("inspectorUnit"));
            unitLabel->setForegroundRole(QPalette::PlaceholderText);
            pairLayout->addWidget(unitLabel);
        }
        layout->AddWidget(*pair, 1);
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

QSize ColourSwatch::sizeHint() const
{
    return QSize(SWATCH_WIDTH, SWATCH_HEIGHT);
}

QSize ColourSwatch::minimumSizeHint() const
{
    return sizeHint();
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
        const QString text = FormatInspectorNumber(channel.second);
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
    : QWidget(parentOrNull), mIdentifier(identifier), mText(new ElidedLabel(ShortenIdentifier(identifier), this)),
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
    layout->addWidget(mText, 1);
    layout->addWidget(mCopy);
    connect(mCopy, &QToolButton::clicked, this, &IdentifierRow::Copy);
}

void IdentifierRow::Copy()
{
    QApplication::clipboard()->setText(mIdentifier);
    mCopy->setText(QStringLiteral("Copied"));
    QTimer::singleShot(COPIED_FEEDBACK_MILLISECONDS, this, [this]() { mCopy->setText(QStringLiteral("Copy")); });
}
} // namespace ludus::editor
