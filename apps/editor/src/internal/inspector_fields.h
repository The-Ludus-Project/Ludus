#pragma once

#include <ludus/foundation/base/types.h>

#include <QColor>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QSlider>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

QT_BEGIN_NAMESPACE
class QFormLayout;
class QLineEdit;
class QToolButton;
QT_END_NAMESPACE

namespace ludus::editor
{
/// How dragging a label changes the number in its field: the inclusive range and the change per dragged pixel.
struct ScrubSettings final
{
    foundation::float64 Minimum = 0;
    foundation::float64 Maximum = 0;
    foundation::float64 Step = 1; ///< Change per pixel dragged; Shift divides it, Control multiplies it.
};

/// Value after dragging deltaPixels (positive to the right) from startValue. Shift gives fine steps and
/// Control coarse ones. Clamped to the range and rounded to a few decimals so 0.1 + 0.2 reads 0.3.
[[nodiscard]] foundation::float64 ComputeScrubbedValue(foundation::float64 startValue,
                                                       foundation::int32 deltaPixels,
                                                       const ScrubSettings& settings,
                                                       Qt::KeyboardModifiers modifiers);

/// A sensible step per pixel for a field: degrees move a whole degree, small ranges (colours, 0 to 1)
/// move fine, metres and scale factors ("×") move a hundredth; anything else moves a thousandth of its range, within
/// 0.001 to 1.
[[nodiscard]] foundation::float64
GetScrubStep(const QString& unit, foundation::float64 minimum, foundation::float64 maximum);

/// The shortest text that reads back as exactly value, so 0.003 shows as "0.003" rather than its 17-digit
/// expansion "0.0030000000000000001".
[[nodiscard]] QString FormatInspectorNumber(foundation::float64 value);

/// How a bounded slider spreads its positions over a number's range.
enum class SliderScale : foundation::uint8
{
    Linear,      ///< Every position changes the value by the same amount.
    Logarithmic, ///< Positions are spaced by ratio either side of zero, so tiny and huge values are both reachable.
};

/// Number of positions a bounded slider has across its range.
constexpr foundation::int32 SLIDER_POSITION_COUNT = 1000;

/// A slider's mapping between its positions (0 to SLIDER_POSITION_COUNT) and a field's inclusive range.
struct SliderMapping final
{
    foundation::float64 Minimum = 0;
    foundation::float64 Maximum = 0;
    /// The field's smallest meaningful change. A linear slider rounds to its decimals; a logarithmic slider is
    /// linear within one step of zero, so it can reach zero and cross between signs.
    foundation::float64 Step = 1;
    SliderScale Scale = SliderScale::Linear;
    bool IsInteger = false; ///< Values are rounded to whole numbers.
};

/// Whether minimum and maximum are finite and maximum is greater, so a slider can span them.
[[nodiscard]] bool HasSliderRange(foundation::float64 minimum, foundation::float64 maximum);

/// The mapping for a field: linear when the range holds at most ten thousand steps (a colour channel, an angle,
/// a few metres), logarithmic otherwise (0 to 10000 m), so a short drag still reaches small values.
/// @pre HasSliderRange(minimum, maximum) and step > 0.
[[nodiscard]] SliderMapping
MakeSliderMapping(foundation::float64 minimum, foundation::float64 maximum, foundation::float64 step, bool isInteger);

/// Value at a slider position, clamped to the range. The ends are exactly Minimum and Maximum; other positions
/// are rounded (to the step's decimals when linear, to three significant digits when logarithmic, to an integer
/// when IsInteger) so the field reads cleanly.
[[nodiscard]] foundation::float64 ConvertSliderPositionToValue(foundation::int32 position,
                                                               const SliderMapping& mapping);

/// Nearest slider position for value; values outside the range give the nearer end.
[[nodiscard]] foundation::int32 ConvertValueToSliderPosition(foundation::float64 value, const SliderMapping& mapping);

/// A horizontal slider bound to a number field with a minimum and maximum. Moving it writes the value into the
/// field and emits the field's textEdited, the same path as typing; typing a number moves the handle. It follows
/// the field's enabled state, and ignores the mouse wheel until focused so the panel still scrolls. GUI thread
/// only.
class BoundedSlider final : public QSlider
{
    Q_OBJECT
public:
    /// field must outlive the slider; both belong to the same row.
    BoundedSlider(QLineEdit& inoutField, const SliderMapping& mapping, QWidget* parentOrNull);
    /// The range and scale this slider uses.
    [[nodiscard]] const SliderMapping& GetMapping() const noexcept
    {
        return mMapping;
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void followField();
    void writeField(foundation::int32 position);

    QLineEdit& mField;
    SliderMapping mMapping;
    bool mIsWriting = false;
};

/// A row with a slider beside a number field, for a field with a minimum and maximum. The field keeps its object
/// name and signals and is reparented into the row.
[[nodiscard]] QWidget* MakeSliderRow(QLineEdit& inoutField, const SliderMapping& mapping, QWidget* parentOrNull);

/// Width, in device-independent pixels, field needs to show text whole: the text in the field's font plus its
/// text margins, cursor and style frame. GUI thread only.
[[nodiscard]] foundation::int32 ComputeLineEditWidth(const QLineEdit& field, const QString& text);

/// Keeps a number field at least as wide as its current value (and never narrower than a short value such as
/// "-0.000"), following edits, so a value such as 0.818579 never shows clipped as "818579" however narrow the panel
/// is; the surrounding row wraps or stacks instead.
void SetNumberFieldMinimumWidth(QLineEdit& inoutField);

/// Arranges an inspector form for a narrow panel: a field that does not fit beside its label wraps below it rather
/// than being clipped, every label moves above its field (in the caption colour) while the field column beside the
/// widest label would be too narrow for a number, and fields fill the row on every platform (macOS otherwise keeps
/// them at their size hint). Call it once the form is installed on its widget.
void ConfigureInspectorForm(QFormLayout& inoutForm);

/// Lays its items out in one row while their minimum widths fit, and otherwise stacks them, one per row at the full
/// width, so a narrow panel grows taller instead of clipping a field. In a row, items without stretch grow to their
/// size hint first and the rest of the width goes to stretched items. Hidden items take no space. GUI thread only.
class WrappingRowLayout final : public QLayout
{
public:
    /// spacingPixels separates neighbouring items in either arrangement.
    WrappingRowLayout(QWidget* parentOrNull, foundation::int32 spacingPixels);
    ~WrappingRowLayout() override;
    WrappingRowLayout(const WrappingRowLayout&) = delete;
    WrappingRowLayout& operator=(const WrappingRowLayout&) = delete;

    /// Adds widget with a share of the row's spare width; 0 keeps it at its size hint.
    void AddWidget(QWidget& inoutWidget, foundation::int32 stretch);
    /// Whether the last geometry stacked the items rather than placing them in one row.
    [[nodiscard]] bool IsStacked() const noexcept
    {
        return mIsStacked;
    }

    void addItem(QLayoutItem* item) override;
    [[nodiscard]] int count() const override;
    [[nodiscard]] QLayoutItem* itemAt(int index) const override;
    [[nodiscard]] QLayoutItem* takeAt(int index) override;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSize() const override;
    [[nodiscard]] Qt::Orientations expandingDirections() const override;
    [[nodiscard]] bool hasHeightForWidth() const override;
    [[nodiscard]] int heightForWidth(int width) const override;
    void setGeometry(const QRect& rectangle) override;

private:
    [[nodiscard]] bool fitsInOneRow(foundation::int32 width) const;
    /// Places the items in area (when shouldPlace) and returns the height they need.
    foundation::int32 arrange(const QRect& area, bool shouldPlace) const;

    QVector<QLayoutItem*> mItems;
    QVector<foundation::int32> mStretches;
    foundation::int32 mSpacingPixels = 0;
    bool mIsStacked = false;
};

/// A single-line label that shortens its text in the middle with an ellipsis when its width is too small, so a long
/// reference keeps its start and end visible. text() stays the whole text; the owner sets the tooltip to it.
class ElidedLabel final : public QLabel
{
    Q_OBJECT
public:
    ElidedLabel(const QString& text, QWidget* parentOrNull);
    /// Whether the label is currently too narrow for its whole text.
    [[nodiscard]] bool IsElided() const;
    /// The text as currently shown, shortened when IsElided().
    [[nodiscard]] QString GetShownText() const;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
};

/// A long identifier (a UUID or hash) as its first eight and last four characters joined by an ellipsis, so a
/// row stays narrow; identifiers that already fit are returned unchanged.
[[nodiscard]] QString ShortenIdentifier(const QString& identifier);

/// A field's axis or channel letter that changes the number in its field by dragging horizontally, as in the
/// transform fields of other editors. A click without a drag focuses the field. GUI thread only.
class ScrubLabel final : public QLabel
{
    Q_OBJECT
public:
    /// field must outlive this label; both belong to the same row.
    ScrubLabel(const QString& text, QLineEdit& inoutField, const ScrubSettings& settings, QWidget* parentOrNull);
    /// Whether a drag is in progress.
    [[nodiscard]] bool IsScrubbing() const noexcept
    {
        return mIsScrubbing;
    }

Q_SIGNALS:
    /// The field's text changed because of a drag; the owner marks its fields as edited.
    void Scrubbed();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QLineEdit& mField;
    ScrubSettings mSettings;
    foundation::float64 mStartValue = 0;
    foundation::int32 mStartX = 0;
    bool mIsScrubbing = false;
    bool mHasMoved = false;
};

/// One axis or channel of a vector row: its letter, its text field and its scrub range.
struct VectorComponent final
{
    QString Name;
    QLineEdit* FieldOrNull = nullptr; ///< Borrowed; reparented into the row.
    ScrubSettings Settings;
};

/// A compact row of letter-and-field pairs with an optional trailing unit, such as "X 0.00  Y 1.00  Z 0.00  m".
/// The pairs share one line while their number fields fit and stack one per line in a narrower panel
/// (WrappingRowLayout). The fields keep their object names and signals; only their arrangement changes.
/// onScrubbed is connected to every label's Scrubbed signal.
[[nodiscard]] QWidget* MakeVectorRow(const QVector<VectorComponent>& components,
                                     const QString& unit,
                                     QWidget* parentOrNull,
                                     const std::function<void()>& onScrubbed);

/// Linear RGB, clamped to 0 to 1, as the display (sRGB) colour a swatch shows, by the exact sRGB transfer.
[[nodiscard]] QColor
ConvertLinearToDisplayColour(foundation::float64 red, foundation::float64 green, foundation::float64 blue);
/// Display colour as linear RGB in 0 to 1, the inverse of ConvertLinearToDisplayColour. Black and white channels
/// convert exactly, so pure primaries keep zero channels.
void ConvertDisplayToLinearColour(const QColor& display,
                                  foundation::float64& outRed,
                                  foundation::float64& outGreen,
                                  foundation::float64& outBlue);

/// A colour swatch that follows three linear-RGB fields and opens the colour picker on click; picking writes
/// the fields. GUI thread only.
class ColourSwatch final : public QPushButton
{
    Q_OBJECT
public:
    /// The fields must outlive the swatch (they share a row).
    ColourSwatch(QLineEdit& inoutRed, QLineEdit& inoutGreen, QLineEdit& inoutBlue, QWidget* parentOrNull);
    /// The colour currently shown, from the fields; invalid when a field does not hold a number.
    [[nodiscard]] QColor GetDisplayColour() const;
    /// Write a picked display colour into the fields (as the picker does) and notify.
    void SetDisplayColour(const QColor& display);
    /// The swatch's fixed size; it has no text to fit.
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

Q_SIGNALS:
    /// The picker wrote new field text; the owner marks its fields as edited.
    void Picked();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void chooseColour();

    QLineEdit& mRed;
    QLineEdit& mGreen;
    QLineEdit& mBlue;
};

/// A read-only colour chip for a colour shown as text, with the text as its tooltip.
class ColourChip final : public QWidget
{
    Q_OBJECT
public:
    ColourChip(const QColor& displayColour, QWidget* parentOrNull);
    [[nodiscard]] QColor GetColour() const noexcept
    {
        return mColour;
    }
    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QColor mColour;
};

/// An identifier shown shortened, with a button that copies the full text. The full text is the tooltip. In a narrow
/// panel the shortened text is shortened further in the middle.
class IdentifierRow final : public QWidget
{
    Q_OBJECT
public:
    IdentifierRow(const QString& identifier, QWidget* parentOrNull);
    [[nodiscard]] const QString& GetIdentifier() const noexcept
    {
        return mIdentifier;
    }
    /// Copy the full identifier to the clipboard, as the button does.
    void Copy();

private:
    QString mIdentifier;
    ElidedLabel* mText;
    QToolButton* mCopy;
};
} // namespace ludus::editor
