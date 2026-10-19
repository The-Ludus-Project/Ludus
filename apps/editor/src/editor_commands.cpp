#include "internal/editor_commands.h"

#include <QAction>
#include <QKeySequence>
#include <QObject>
#include <QString>

namespace ludus::editor
{
QAction* CreateEditorCommand(const EditorCommand& command, QObject* owner)
{
    QAction* action = new QAction(QString::fromUtf8(command.Text), owner);
    action->setObjectName(QString::fromUtf8(command.Id));
    action->setCheckable(command.IsCheckable);
    const QString toolbarText = QString::fromUtf8(command.ToolbarText);
    if (toolbarText.isEmpty() == false)
    {
        // QToolButton shows iconText in text-only toolbars; keep labels short so a
        // 1280-pixel window shows every command without clipping.
        action->setIconText(toolbarText);
    }
    QString help = QString::fromUtf8(command.Help);
    const QKeySequence shortcut(QString::fromUtf8(command.Shortcut));
    if (shortcut.isEmpty() == false)
    {
        action->setShortcut(shortcut);
        help += QStringLiteral(" (%1)").arg(shortcut.toString(QKeySequence::NativeText));
    }
    action->setToolTip(help);
    action->setStatusTip(QString::fromUtf8(command.Help));
    return action;
}
} // namespace ludus::editor
