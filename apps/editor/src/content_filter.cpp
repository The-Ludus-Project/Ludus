#include "internal/content_browser.h"

#include <QRegularExpression>

namespace ludus::editor
{
QStringList SplitContentSearch(const QString& text)
{
    static const QRegularExpression WHITESPACE(QStringLiteral("\\s+"));
    return text.split(WHITESPACE, Qt::SkipEmptyParts);
}

bool DoesContentRowMatch(const ContentRow& row, const QStringList& terms, ContentCategoryMask mask)
{
    if (mask != 0 && (mask & GetContentCategoryBit(GetContentCategory(row.Type))) == 0)
    {
        return false;
    }
    if (terms.isEmpty() == true)
    {
        return true;
    }
    const QString typeName = GetContentTypeName(row.Type);
    for (const QString& term : terms)
    {
        if (row.Id.contains(term, Qt::CaseInsensitive) == false &&
            row.Label.contains(term, Qt::CaseInsensitive) == false &&
            typeName.contains(term, Qt::CaseInsensitive) == false &&
            row.Path.contains(term, Qt::CaseInsensitive) == false)
        {
            return false;
        }
    }
    return true;
}

ContentFilterProxyModel::ContentFilterProxyModel(QObject* parentOrNull) : QSortFilterProxyModel(parentOrNull)
{
    setSortCaseSensitivity(Qt::CaseInsensitive);
}

void ContentFilterProxyModel::SetCatalogModel(ContentCatalogModel* modelOrNull)
{
    mCatalogOrNull = modelOrNull;
    setSourceModel(modelOrNull);
}

void ContentFilterProxyModel::SetSearchText(const QString& text)
{
    QStringList terms = SplitContentSearch(text);
    if (terms == mTerms)
    {
        return;
    }
    mTerms = terms;
    refilter();
}

void ContentFilterProxyModel::SetCategoryMask(ContentCategoryMask mask)
{
    if (mask == mCategoryMask)
    {
        return;
    }
    mCategoryMask = mask;
    refilter();
}

ContentCategoryMask ContentFilterProxyModel::GetCategoryMask() const noexcept
{
    return mCategoryMask;
}

QVector<int> ContentFilterProxyModel::CountCategories() const
{
    QVector<int> counts(static_cast<qsizetype>(CONTENT_CATEGORY_COUNT), 0);
    if (mCatalogOrNull == nullptr)
    {
        return counts;
    }
    for (const ContentRow& row : mCatalogOrNull->GetRows())
    {
        if (DoesContentRowMatch(row, mTerms, 0) == true)
        {
            ++counts[static_cast<qsizetype>(GetContentCategory(row.Type))];
        }
    }
    return counts;
}

bool ContentFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    if (mCatalogOrNull == nullptr)
    {
        return QSortFilterProxyModel::filterAcceptsRow(sourceRow, sourceParent);
    }
    const ContentRow* rowOrNull = mCatalogOrNull->Row(mCatalogOrNull->index(sourceRow, 0, sourceParent));
    return rowOrNull != nullptr && DoesContentRowMatch(*rowOrNull, mTerms, mCategoryMask) == true;
}

void ContentFilterProxyModel::refilter()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    beginFilterChange();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    invalidateRowsFilter();
#endif
}
} // namespace ludus::editor
