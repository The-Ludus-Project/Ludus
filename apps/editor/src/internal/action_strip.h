#pragma once

#include <ludus/foundation/base/types.h>

#include <QList>
#include <QWidget>

class QAbstractButton;
class QMenu;
class QToolButton;

namespace ludus::editor
{
/// One row of action buttons that never wraps and never clips a label. Buttons keep their order and size;
/// those that do not fit the width move into a trailing "More" menu, where each stays an entry with its own
/// label, enabled state and tooltip. The strip asks only for the More button's width as its minimum, so a
/// short or narrow dock can shrink it and leave height for the list above. GUI thread only.
class ActionStrip final : public QWidget
{
    Q_OBJECT

public:
    explicit ActionStrip(QWidget* parentOrNull = nullptr);

    /// Append action; the strip takes it as its child.
    void AddAction(QAbstractButton& inoutAction);
    /// Offer or withdraw action without deleting it (a command that only applies in some states).
    void SetActionVisible(QAbstractButton& inoutAction, bool isVisible);
    /// The trailing button that opens the actions that do not fit.
    [[nodiscard]] QToolButton& GetOverflowButton() const noexcept
    {
        return *mOverflow;
    }
    /// How many offered actions the current width moved into the More menu.
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
    struct Item final
    {
        QAbstractButton* ButtonOrNull = nullptr;
        bool IsVisible = true;
    };

    /// Show the offered actions that fit the width in order, the rest in the menu.
    void arrange();
    /// Mirror the offered actions the width hid as menu entries.
    void populateMenu();
    [[nodiscard]] Item* findItemOrNull(QAbstractButton& inoutAction) noexcept;

    QList<Item> mItems;
    QToolButton* mOverflow;
    QMenu* mMenu;
    foundation::int32 mOverflowCount = 0;
};
} // namespace ludus::editor
