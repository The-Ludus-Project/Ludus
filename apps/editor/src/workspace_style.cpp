#include "internal/workspace_style.h"

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QObject>
#include <QPalette>
#include <QString>
#include <QWidget>

namespace ludus::editor
{
namespace
{
void UpdateWorkspaceStyle(QWidget* widget)
{
    // Thanks to David Lightbown, Designing the User Experience of Game
    // Development Tools (CRC Press, 2015), ch. 5, pp. 80-86: expose hierarchy
    // and preserve familiar controls. Brand roles remain confined to content;
    // native menus and window decoration retain their platform conventions.
    // See docs/architecture/editor-art-direction.md and
    // https://www.uxofgametools.com/ (2015 edition consulted).
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    const QColor workspace(dark ? "#18232c" : "#edf3f2");
    const QColor panel(dark ? "#222f39" : "#ffffff");
    const QColor raised(dark ? "#2c3c47" : "#e1ece9");
    const QColor text(dark ? "#e9f2f1" : "#18232c");
    const QColor secondary(dark ? "#b5c5c9" : "#4c6267");
    const QColor border(dark ? "#80969e" : "#71878b");
    const QColor accent(dark ? "#57d2c1" : "#14786f");
    const QColor accentText(dark ? "#18232c" : "#ffffff");
    const QColor selected(dark ? "#294b4b" : "#d5eee8");
    auto palette = QApplication::palette();
    palette.setColor(QPalette::Window, workspace);
    palette.setColor(QPalette::Base, panel);
    palette.setColor(QPalette::AlternateBase, raised);
    palette.setColor(QPalette::Button, raised);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::PlaceholderText, secondary);
    palette.setColor(QPalette::Mid, border);
    palette.setColor(QPalette::Highlight, selected);
    palette.setColor(QPalette::HighlightedText, text);
    palette.setColor(QPalette::Link, accent);
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
    {
        palette.setColor(QPalette::Disabled, role, secondary);
    }
    auto style =
        QStringLiteral(
            "QLabel, QCheckBox, QRadioButton, QGroupBox, QToolButton, QTabBar, QStatusBar, QDockWidget { color: "
            "palette(text); } QCheckBox:disabled, QRadioButton:disabled, QToolButton:disabled { color: "
            "palette(placeholder-text); } "
            "QMainWindow, QDialog, QWidget#projectWelcome { background: palette(window); } "
            "QDialog { border: 1px solid palette(mid); } "
            "QDockWidget::title { background: palette(button); padding: 6px; border-bottom: 1px solid palette(mid); } "
            "QTabWidget::pane { border: 1px solid palette(mid); background: palette(base); } "
            "QTabBar::tab { background: palette(window); padding: 7px 12px; border-bottom: 2px solid transparent; } "
            "QTabBar::tab:selected { background: palette(base); border-bottom-color: %1; } "
            "QTabBar::tab:hover { background: palette(button); } "
            "QMainWindow::separator { background: palette(mid); width: 4px; height: 4px; } "
            "QToolBar { spacing: 4px; padding: 4px; border-bottom: 1px solid palette(mid); } "
            "QToolButton { padding: 4px; border-radius: 4px; } "
            "QToolButton:hover, QToolButton:checked { background: palette(button); } "
            "QPushButton { background: palette(button); color: palette(button-text); border: 1px solid palette(mid); "
            "border-radius: 4px; padding: 5px 12px; } "
            "QPushButton[density=compact] { padding: 2px 8px; } "
            "QPushButton:hover:enabled { border-color: %1; } "
            "QPushButton:pressed, QPushButton:checked { background: palette(highlight); } "
            "QPushButton[visualRole=primary]:enabled, QPushButton:default:enabled { background: %1; color: %2; "
            "border-color: %1; } "
            "QPushButton:disabled { color: palette(placeholder-text); background: palette(window); } "
            "QLineEdit, QAbstractSpinBox, QComboBox { background: palette(base); color: palette(text); "
            "border: 1px solid palette(mid); border-radius: 4px; padding: 4px; } "
            "QAbstractSpinBox QLineEdit { border: none; padding: 0px; background: transparent; } "
            "QComboBox { padding-right: 20px; } "
            "QAbstractItemView, QPlainTextEdit, QTextEdit { background: palette(base); border: 1px solid palette(mid); "
            "border-radius: 4px; selection-background-color: palette(highlight); selection-color: "
            "palette(highlighted-text); } "
            "QListWidget#recentProjectsList::item { padding: 8px; } "
            "QAbstractItemView::item:selected { background: palette(highlight); color: palette(highlighted-text); } "
            "QHeaderView::section { background: palette(button); color: palette(text); padding: 4px; "
            "border: none; border-bottom: 1px solid palette(mid); } "
            "QPushButton:focus, QLineEdit:focus, QAbstractSpinBox:focus, QComboBox:focus, QAbstractItemView:focus, "
            "QPlainTextEdit:focus, QTextEdit:focus { border: 2px solid %1; } "
            "QToolButton:focus, QTabBar::tab:focus { outline: 2px solid %1; } "
            "QLabel[visualRole=secondary] { color: palette(placeholder-text); } "
            "QStatusBar { border-top: 1px solid palette(mid); }")
            .arg(accent.name(), accentText.name());
    style.replace(QStringLiteral("palette(window)"), workspace.name());
    style.replace(QStringLiteral("palette(base)"), panel.name());
    style.replace(QStringLiteral("palette(button)"), raised.name());
    style.replace(QStringLiteral("palette(mid)"), border.name());
    style.replace(QStringLiteral("palette(button-text)"), text.name());
    style.replace(QStringLiteral("palette(text)"), text.name());
    style.replace(QStringLiteral("palette(highlight)"), selected.name());
    style.replace(QStringLiteral("palette(highlighted-text)"), text.name());
    style.replace(QStringLiteral("palette(placeholder-text)"), secondary.name());
    widget->setStyleSheet(style);
    widget->setPalette(palette);
}

class WorkspaceStyle final : public QObject
{
public:
    explicit WorkspaceStyle(QWidget* widget) : QObject(widget), Widget_(widget) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == qApp && event->type() == QEvent::ApplicationPaletteChange)
        {
            UpdateWorkspaceStyle(Widget_);
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QWidget* Widget_;
};
} // namespace

void ApplyWorkspaceStyle(QWidget* widget)
{
    UpdateWorkspaceStyle(widget);
    qApp->installEventFilter(new WorkspaceStyle(widget));
}
} // namespace ludus::editor
