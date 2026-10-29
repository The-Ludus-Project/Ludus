#include "internal/workspace_style.h"
#include "internal/editor_style.h"
#include "internal/theme_preferences.h"

#include <QApplication>
#include <QEvent>
#include <QMainWindow>
#include <QObject>
#include <QPalette>
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
    // The palette carries the colours and EditorStyle (editor_style.cpp) draws
    // the controls; no stylesheet is involved, so palette changes propagate to
    // every child widget normally.
    // See docs/architecture/editor-art-direction.md and
    // https://www.uxofgametools.com/ (2015 edition consulted).
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    const ThemePreferences preferences = GetThemePreferences();
    const bool isHighContrast = preferences.IsHighContrast == true;
    ApplyMotionPreference(preferences, qobject_cast<QMainWindow*>(widget));
    const QPalette palette = MakeEditorPalette(QApplication::palette(), dark, isHighContrast);
    InstallEditorStyle();
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
