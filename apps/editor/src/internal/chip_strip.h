#pragma once

#include <ludus/foundation/base/types.h>

#include <QList>
#include <QWidget>

class QMenu;
class QToolButton;

namespace ludus::editor
{
/// One row of filter chips that never clips one. Chips keep their order and size; those that do not fit the width
/// move into a trailing "More" menu button, where each stays a checkable entry with its own label and count. The
/// More button reads as on (the toggle accent) while a chip it holds is on, so an active filter never hides. The
/// strip asks only for the More button's width as its minimum, so a narrow dock can shrink it. GUI thread only.
class ChipStrip final : public QWidget
{
public:
    explicit ChipStrip(QWidget* parentOrNull = nullptr);

    /// Append chip, a checkable tool button; the strip takes it as its child.
    void AddChip(QToolButton& inoutChip);
    /// The trailing button that opens the chips that do not fit.
    [[nodiscard]] QToolButton& GetOverflowButton() const noexcept
    {
        return *mOverflow;
    }
    /// How many chips the current width moved into the More menu.
    [[nodiscard]] foundation::int32 GetOverflowCount() const noexcept
    {
        return mOverflowCount;
    }

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    bool event(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    /// Show the chips that fit the width in order, the rest in the menu.
    void arrange();
    /// Mirror the chips the width hid as menu entries.
    void populateMenu();
    /// The More button is on while a chip it holds is on.
    void renderOverflowState();

    QList<QToolButton*> mChips;
    QToolButton* mOverflow;
    QMenu* mMenu;
    foundation::int32 mOverflowCount = 0;
};
} // namespace ludus::editor
