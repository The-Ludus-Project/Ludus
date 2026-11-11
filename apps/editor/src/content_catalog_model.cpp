#include "internal/content_browser.h"
#include "internal/editor_icons.h"

#include <QDir>
#include <QVariant>

#include <utility>

namespace ludus::editor
{
namespace
{
/// Whether a catalog path stays inside its project root once joined to it.
[[nodiscard]] bool IsInsideRoot(const QString& root, const QString& relative)
{
    if (root.isEmpty() == true || relative.isEmpty() == true || QDir::isAbsolutePath(relative) == true)
    {
        return false;
    }
    const QString cleaned = QDir::cleanPath(relative);
    return cleaned != QStringLiteral("..") && cleaned.startsWith(QStringLiteral("../")) == false;
}

[[nodiscard]] QString GetStatusText(const ContentOperation& operation)
{
    constexpr double PERCENT = 100.0;
    switch (operation.Phase)
    {
        case ContentOperationPhase::Queued:
            return QStringLiteral("Queued");
        case ContentOperationPhase::Importing:
            return QStringLiteral("%1 · %2%").arg(operation.Message).arg(qRound(operation.Progress * PERCENT));
        case ContentOperationPhase::Failed:
            return QStringLiteral("Failed: ") + operation.Message;
    }
    return {};
}
} // namespace

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
    return parent.isValid() ? 0 : CONTENT_COLUMN_COUNT;
}
const ContentRow* ContentCatalogModel::Row(const QModelIndex& index) const
{
    return index.model() == this && index.row() >= 0 && index.row() < Rows_.size() ? &Rows_[index.row()] : nullptr;
}
const QVector<ContentRow>& ContentCatalogModel::GetRows() const noexcept
{
    return Rows_;
}
QVariant ContentCatalogModel::data(const QModelIndex& index, int role) const
{
    const ContentRow* row = Row(index);
    if (row == nullptr)
    {
        return {};
    }
    const ContentOperation* operationOrNull = FindOperation(row->Id);
    switch (role)
    {
        case Qt::UserRole:
            return row->Id;
        case CONTENT_TYPE_ROLE:
            return static_cast<int>(row->Type);
        case CONTENT_PROGRESS_ROLE:
            return operationOrNull != nullptr && operationOrNull->Phase == ContentOperationPhase::Importing
                       ? operationOrNull->Progress
                       : -1.0;
        case CONTENT_FAILED_ROLE:
            return operationOrNull != nullptr && operationOrNull->Phase == ContentOperationPhase::Failed;
        default:
            break;
    }
    const ContentColumn column = static_cast<ContentColumn>(index.column());
    if (role == Qt::DecorationRole)
    {
        if (column == ContentColumn::Name)
        {
            return decoration(*row);
        }
        if (column == ContentColumn::Status && operationOrNull != nullptr &&
            operationOrNull->Phase == ContentOperationPhase::Failed)
        {
            return GetEditorIcon(EditorIcon::Error);
        }
        return {};
    }
    if (role != Qt::DisplayRole && role != Qt::ToolTipRole)
    {
        return {};
    }
    switch (column)
    {
        case ContentColumn::Name:
            if (role == Qt::ToolTipRole)
            {
                return row->Label.isEmpty() == true ? row->Id : row->Label + QStringLiteral("\n") + row->Id;
            }
            return row->Label.isEmpty() == true ? row->Id : row->Label;
        case ContentColumn::Type:
            return GetContentTypeName(row->Type);
        case ContentColumn::Status:
            if (operationOrNull != nullptr)
            {
                return GetStatusText(*operationOrNull);
            }
            return row->IsPending == true ? QStringLiteral("Queued") : QString();
        case ContentColumn::Path:
            return row->Path;
    }
    return {};
}
QVariant ContentCatalogModel::decoration(const ContentRow& row) const
{
    const QIcon typeIcon = GetEditorIcon(GetContentTypeIcon(row.Type));
    if (mThumbnailsOrNull == nullptr || HasContentThumbnail(row.Type) == false || row.Key.isEmpty() == true ||
        IsInsideRoot(mRoot, row.Path) == false)
    {
        return typeIcon;
    }
    const QPixmap thumbnail = mThumbnailsOrNull->Find(row.Id, row.Key);
    if (thumbnail.isNull() == false)
    {
        return thumbnail;
    }
    if (mThumbnailsOrNull->HasFailed(row.Id, row.Key) == true)
    {
        return typeIcon;
    }
    // Only rows a view paints ask for a decoration, so only visible rows queue work.
    ContentThumbnailRequest request;
    request.Id = row.Id;
    request.Key = row.Key;
    request.Type = row.Type;
    const QDir root(mRoot);
    request.Path = root.filePath(row.Path);
    request.Side = qRound(CONTENT_THUMBNAIL_SIZE * mDevicePixelRatio);
    request.DevicePixelRatio = mDevicePixelRatio;
    for (const QString& dependency : row.Dependencies)
    {
        const QHash<QString, int>::const_iterator found = Ids_.constFind(dependency);
        if (found != Ids_.constEnd() && Rows_[found.value()].Type == ContentType::Texture &&
            IsInsideRoot(mRoot, Rows_[found.value()].Path) == true)
        {
            request.TexturePaths.insert(dependency, root.filePath(Rows_[found.value()].Path));
        }
    }
    mThumbnailsOrNull->Request(request);
    return typeIcon;
}
QVariant ContentCatalogModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
    {
        return {};
    }
    switch (static_cast<ContentColumn>(section))
    {
        case ContentColumn::Name:
            return QStringLiteral("Name");
        case ContentColumn::Type:
            return QStringLiteral("Type");
        case ContentColumn::Status:
            return QStringLiteral("Status");
        case ContentColumn::Path:
            return QStringLiteral("Content path");
    }
    return {};
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
                Q_EMIT dataChanged(index(static_cast<int>(row), 0),
                                   index(static_cast<int>(row), CONTENT_COLUMN_COUNT - 1));
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
void ContentCatalogModel::SetRoot(const QString& root)
{
    if (mRoot == root)
    {
        return;
    }
    mRoot = root;
    if (mThumbnailsOrNull != nullptr)
    {
        mThumbnailsOrNull->Clear();
    }
}
void ContentCatalogModel::SetThumbnailCache(ContentThumbnailCache* cacheOrNull)
{
    if (mThumbnailsOrNull != nullptr)
    {
        disconnect(mThumbnailsOrNull, nullptr, this, nullptr);
    }
    mThumbnailsOrNull = cacheOrNull;
    if (mThumbnailsOrNull != nullptr)
    {
        connect(mThumbnailsOrNull, &ContentThumbnailCache::ThumbnailReady, this, [this](const QString& id) {
            const QModelIndex found = FindId(id);
            if (found.isValid() == true)
            {
                Q_EMIT dataChanged(found, found, {Qt::DecorationRole});
            }
        });
    }
}
void ContentCatalogModel::SetDevicePixelRatio(qreal ratio)
{
    const qreal clamped = ratio < 1.0 ? 1.0 : ratio;
    if (qFuzzyCompare(clamped, mDevicePixelRatio) == true)
    {
        return;
    }
    mDevicePixelRatio = clamped;
    if (mThumbnailsOrNull != nullptr)
    {
        mThumbnailsOrNull->Clear();
    }
    if (Rows_.isEmpty() == false)
    {
        Q_EMIT dataChanged(index(0, 0), index(static_cast<int>(Rows_.size()) - 1, 0), {Qt::DecorationRole});
    }
}
void ContentCatalogModel::SetOperation(const QString& id, const ContentOperation& operation)
{
    const QHash<QString, ContentOperation>::const_iterator found = mOperations.constFind(id);
    if (found != mOperations.constEnd() && found.value() == operation)
    {
        return;
    }
    mOperations.insert(id, operation);
    emitRowChanged(id);
}
void ContentCatalogModel::ClearOperation(const QString& id)
{
    if (mOperations.remove(id) > 0)
    {
        emitRowChanged(id);
    }
}
const ContentOperation* ContentCatalogModel::FindOperation(const QString& id) const
{
    const QHash<QString, ContentOperation>::const_iterator found = mOperations.constFind(id);
    return found == mOperations.constEnd() ? nullptr : &found.value();
}
void ContentCatalogModel::emitRowChanged(const QString& id)
{
    const QModelIndex found = FindId(id);
    if (found.isValid() == true)
    {
        Q_EMIT dataChanged(index(found.row(), 0), index(found.row(), CONTENT_COLUMN_COUNT - 1));
    }
}
} // namespace ludus::editor
