#include "internal/content_browser.h"

#include <QVariant>

#include <utility>

namespace ludus::editor
{
// Thanks to Qt Group, "Model/View Programming", sections "Model/View Classes"
// and "Model Subclassing Reference": project data through a table model with
// role notifications, rather than storing it in per-row widgets.
// https://doc.qt.io/qt-6/model-view-programming.html
// See docs/architecture/editor-gui-systems.md, "Threads, jobs and debugging".
ContentCatalogModel::ContentCatalogModel(QObject* parent) : QAbstractTableModel(parent) {}
int ContentCatalogModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(Rows_.size());
}
int ContentCatalogModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : 3;
}
const ContentRow* ContentCatalogModel::Row(const QModelIndex& index) const
{
    return index.model() == this && index.row() >= 0 && index.row() < Rows_.size() ? &Rows_[index.row()] : nullptr;
}
QVariant ContentCatalogModel::data(const QModelIndex& index, int role) const
{
    const auto* row = Row(index);
    if (row == nullptr)
    {
        return {};
    }
    if (role == Qt::UserRole)
    {
        return row->Id;
    }
    if (role != Qt::DisplayRole && role != Qt::ToolTipRole)
    {
        return {};
    }
    if (index.column() == 0)
    {
        return row->Id;
    }
    if (index.column() == 1)
    {
        switch (row->Kind)
        {
            case content::Kind::AudioSource:
                return QStringLiteral("Audio source");
            case content::Kind::Sound:
                return QStringLiteral("Sound");
            case content::Kind::Music:
                return QStringLiteral("Music");
            case content::Kind::CookedScene:
                return QStringLiteral("Cooked scene");
            case content::Kind::Mesh:
                return QStringLiteral("Mesh");
            case content::Kind::CookedGeometry:
                return QStringLiteral("Cooked geometry");
        }
        return QStringLiteral("Unknown");
    }
    return index.column() == 2 ? QVariant(row->Path) : QVariant();
}
QVariant ContentCatalogModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
    {
        return {};
    }
    return section == 0   ? QStringLiteral("Resource ID")
           : section == 1 ? QStringLiteral("Kind")
           : section == 2 ? QStringLiteral("Content path")
                          : QString();
}
void ContentCatalogModel::SetRows(QVector<ContentRow> rows)
{
    bool sameIds = rows.size() == Rows_.size();
    for (qsizetype index = 0; sameIds && index < rows.size(); ++index)
    {
        sameIds = rows[index].Id == Rows_[index].Id;
    }
    if (sameIds)
    {
        for (qsizetype row = 0; row < rows.size(); ++row)
        {
            if (Rows_[row] != rows[row])
            {
                Rows_[row] = rows[row];
                Q_EMIT dataChanged(index(static_cast<int>(row), 0), index(static_cast<int>(row), 2));
            }
        }
        return;
    }
    beginResetModel();
    Rows_ = std::move(rows);
    Ids_.clear();
    Ids_.reserve(Rows_.size());
    for (qsizetype row = 0; row < Rows_.size(); ++row)
    {
        Ids_.insert(Rows_[row].Id, static_cast<int>(row));
    }
    endResetModel();
}
QModelIndex ContentCatalogModel::FindId(const QString& id) const
{
    const auto found = Ids_.constFind(id);
    return found == Ids_.constEnd() ? QModelIndex() : index(found.value(), 0);
}
} // namespace ludus::editor
