#pragma once

#include <QString>
#include <QTabBar>
#include <QVariantMap>
#include <QWidget>

namespace ludus::editor
{
/// The tab data key SetTabModified stores its mark under; the rest of the tab's data map is left alone.
inline constexpr const char* TAB_MODIFIED_KEY = "ludusTabModified";

/// Whether SetTabModified marked this tab.
[[nodiscard]] inline bool IsTabModified(const QTabBar& tabBar, int index)
{
    return index >= 0 && index < tabBar.count() &&
           tabBar.tabData(index).toMap().value(QString::fromLatin1(TAB_MODIFIED_KEY)).toBool() == true;
}

/// Mark one tab of a tab bar as holding unsaved changes. The editor style then draws a dot where the tab's close
/// button sits, and the close cross only while the pointer is on that button, as the document tabs of VS and VS Code
/// do. The mark travels with the tab when it is dragged; a tab removed and inserted again starts unmarked.
inline void SetTabModified(QTabBar& inoutTabBar, int index, bool isModified)
{
    if (index < 0 || index >= inoutTabBar.count() || IsTabModified(inoutTabBar, index) == isModified)
    {
        return;
    }
    QVariantMap data = inoutTabBar.tabData(index).toMap();
    data.insert(QString::fromLatin1(TAB_MODIFIED_KEY), isModified);
    inoutTabBar.setTabData(index, data);
    inoutTabBar.update(inoutTabBar.tabRect(index));
    for (const QTabBar::ButtonPosition side : {QTabBar::LeftSide, QTabBar::RightSide})
    {
        if (QWidget* buttonOrNull = inoutTabBar.tabButton(index, side); buttonOrNull != nullptr)
        {
            buttonOrNull->update();
        }
    }
}
} // namespace ludus::editor
