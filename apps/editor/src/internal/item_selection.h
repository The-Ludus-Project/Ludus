#pragma once

#include <ludus/foundation/base/compiler.h>

#include <QAbstractItemView>
#include <QKeySequence>
#include <QObject>
#include <QShortcut>

namespace ludus::editor
{
/// Use Qt's native toggle/range/Select All bindings; Escape clears only the focused view.
LUDUS_INLINE void ConfigureItemSelection(QAbstractItemView& inoutView)
{
    inoutView.setSelectionMode(QAbstractItemView::ExtendedSelection);
    QShortcut* clear = new QShortcut(QKeySequence(Qt::Key_Escape), &inoutView);
    clear->setContext(Qt::WidgetShortcut);
    QObject::connect(clear, &QShortcut::activated, &inoutView, [&inoutView]() { inoutView.clearSelection(); });
}
} // namespace ludus::editor
