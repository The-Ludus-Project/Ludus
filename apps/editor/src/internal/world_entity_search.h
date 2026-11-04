#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;

namespace ludus::editor
{
/// Private hierarchy search controls. The owner keeps the borrowed list alive during use.
/// Filtering only changes view state and deselects hidden rows to keep group edits visible.
class WorldEntitySearch final : public QWidget
{
    Q_OBJECT
public:
    WorldEntitySearch(QListWidget& inoutEntities, QWidget* parentOrNull);
    /// Reapply the current query after the owner rebuilds the entity rows.
    void Refresh();
    /// Clear a query hiding an entity requested by viewport picking or diagnostic navigation.
    void RevealEntity(const QString& id);

Q_SIGNALS:
    /// Filtering removed selected rows or the current row; read the list once after the update.
    void SelectionChanged();

private:
    QListWidget& mEntities;
    QLineEdit* mSearch;
    QLabel* mCount;
};
} // namespace ludus::editor
