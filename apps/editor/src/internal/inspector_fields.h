#pragma once

#include <ludus/foundation/base/types.h>

#include <QColor>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

QT_BEGIN_NAMESPACE
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
/// The fields keep their object names and signals; only their arrangement changes. onScrubbed is connected to
/// every label's Scrubbed signal.
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

/// An identifier shown shortened, with a button that copies the full text. The full text is the tooltip.
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
    QLabel* mText;
    QToolButton* mCopy;
};
} // namespace ludus::editor
