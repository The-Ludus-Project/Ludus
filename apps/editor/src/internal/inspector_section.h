#pragma once

#include "internal/editor_icons.h"

#include <ludus/foundation/base/types.h>

#include <QLabel>
#include <QPointer>
#include <QString>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QAction;
class QFormLayout;
class QMenu;
class QToolButton;
QT_END_NAMESPACE

namespace ludus::editor
{
/// A row label of the Properties dock. In the shared label column it shortens at its end rather than wrap or clip,
/// and its tooltip always holds the whole label. When it names a check box, clicking it toggles that box, so the
/// label and the box act as one control.
class InspectorLabel final : public QLabel
{
    Q_OBJECT
public:
    InspectorLabel(const QString& text, QWidget* parentOrNull);

    /// True while the label column is too narrow for the whole text.
    [[nodiscard]] bool IsElided() const;
    /// The text as drawn: whole, or shortened with an ellipsis at its end.
    [[nodiscard]] QString GetShownText() const;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
};

/// Adds a labelled row to an inspector form: an InspectorLabel in the label column and field beside it. A check box
/// becomes the label's buddy, so clicking the label toggles it. Returns the label.
InspectorLabel& AddInspectorRow(QFormLayout& inoutForm, const QString& title, QWidget& inoutField);

/// One component (or other block) of the Properties dock: a header with a disclosure chevron, an icon, the title and
/// a trailing "⋯" menu for actions such as Remove Component, then an optional one-line help caption with "More",
/// then the fields. It draws a thin rule above itself instead of a card around it. Whether it is collapsed is kept
/// per state key in QSettings, so a component a person folded away stays folded on every entity.
class InspectorSection final : public QWidget
{
    Q_OBJECT
public:
    /// stateKey names the section in QSettings; an empty key keeps the collapsed state for this widget only.
    InspectorSection(const QString& title, const QString& stateKey, QWidget* parentOrNull);

    [[nodiscard]] QString GetTitle() const;
    void SetIcon(EditorIcon icon);
    /// Help shown under the header as a single caption line; "More" shows the rest.
    void SetHelp(const QString& text);
    /// Marks the section as one the project requires: a lock beside the title, with reason as its tooltip.
    void SetRequired(const QString& reason);
    /// Adds an action to the section's "⋯" menu, which appears once it has an action.
    QAction& AddMenuAction(const QString& text, const QString& objectName);

    /// The widget the caller lays its fields out on (usually with a QFormLayout).
    [[nodiscard]] QWidget& GetFields() noexcept
    {
        return *mFields;
    }
    [[nodiscard]] bool IsCollapsed() const noexcept
    {
        return mIsCollapsed;
    }
    void SetCollapsed(bool isCollapsed);
    [[nodiscard]] bool IsHelpExpanded() const noexcept
    {
        return mIsHelpExpanded;
    }
    void SetHelpExpanded(bool isExpanded);

Q_SIGNALS:
    void CollapsedChanged(bool isCollapsed);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateHelp();

    QString mStateKey;
    QWidget* mHeader;
    QToolButton* mDisclosure;
    QLabel* mIcon;
    QLabel* mTitle;
    QLabel* mRequired;
    QToolButton* mMenuButton;
    QMenu* mMenu;
    InspectorLabel* mHelp;
    QToolButton* mMore;
    QWidget* mFields;
    bool mIsCollapsed = false;
    bool mIsHelpExpanded = false;
};

/// Gives every inspector form under one root widget (the Properties dock's content) the same label column, so
/// Transform, Light and Material line up instead of each form choosing its own. The column takes a share of the
/// dock's width between a minimum and a maximum number of characters. When the dock is too narrow for a readable
/// field beside that column, every form puts its labels above their fields together; check boxes then carry their
/// label beside the box, so they never sit alone on a line.
class InspectorLabelColumn final : public QObject
{
public:
    explicit InspectorLabelColumn(QWidget& inoutRoot);

    /// The column controlling widget, or null when widget is not under one.
    [[nodiscard]] static const InspectorLabelColumn* FindOrNull(const QWidget& widget);

    [[nodiscard]] foundation::int32 GetLabelWidth() const noexcept
    {
        return mLabelWidth;
    }
    [[nodiscard]] bool IsStacked() const noexcept
    {
        return mIsStacked;
    }
    /// Recomputes the column from the root's width and applies it to every form under the root.
    void Update();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applyTo(QFormLayout& inoutForm) const;

    QWidget& mRoot;
    foundation::int32 mLabelWidth = 0;
    bool mIsStacked = false;
    bool mIsUpdating = false;
};
} // namespace ludus::editor
