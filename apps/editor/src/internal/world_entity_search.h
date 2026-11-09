#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

class QLabel;
class QLineEdit;
class QTreeWidget;

namespace ludus::editor
{
class WorldDocument;

/// Builds and filters the World entity tree. The workspace owns the borrowed tree and document.
class WorldEntitySearch final : public QWidget
{
    Q_OBJECT
public:
    WorldEntitySearch(QTreeWidget& inoutEntities, QWidget* parentOrNull);
    /// Rebuild rows from the validated draft, retaining expansion and selection by entity key.
    void Rebuild(const WorldDocument& document, const QStringList& selection, const QString& current);
    /// Update displayed names in place so applying inspector edits preserves focus.
    void UpdateLabels(const WorldDocument& document);
    /// Reveal an entity requested by viewport picking or diagnostic navigation.
    void RevealEntity(const QString& id);

Q_SIGNALS:
    /// Filtering or collapsing hid selected rows or the current row.
    void SelectionChanged();

private:
    void refresh();
    void deselectCollapsedRows();

    QTreeWidget& mEntities;
    QLineEdit* mSearch;
    QLabel* mCount;
};
} // namespace ludus::editor
