#include "design_tokens.h"
#include "internal/content_browser.h"
#include "internal/editor_files.h"
#include "internal/editor_icons.h"
#include "internal/item_selection.h"
#include "internal/state_view.h"

#include <QAbstractItemView>
#include <QContextMenuEvent>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

#if !defined(Q_OS_WASM)
#    include <QFileSystemWatcher>
#endif

namespace ludus::editor
{
namespace
{
/// Initial widths of the Name, Type and Status columns in logical pixels; Content path stretches.
constexpr int NAME_COLUMN_WIDTH = 260;
constexpr int TYPE_COLUMN_WIDTH = 110;
constexpr int STATUS_COLUMN_WIDTH = 180;
/// Height of the per-row import progress bar drawn under the status text.
constexpr int ROW_PROGRESS_HEIGHT = 3;
/// Steps of the overall progress bar.
constexpr int PROGRESS_STEPS = 1000;
/// Quiet period after a catalog folder changes before the list refreshes, so one import's several writes refresh once.
constexpr int WATCH_DELAY_MILLISECONDS = 250;

[[nodiscard]] QString GetStageText(ContentImportStage stage)
{
    switch (stage)
    {
        case ContentImportStage::Waiting:
            return QStringLiteral("Waiting");
        case ContentImportStage::Reading:
            return QStringLiteral("Reading source");
        case ContentImportStage::Validating:
            return QStringLiteral("Validating dependencies");
        case ContentImportStage::Checking:
            return QStringLiteral("Checking for changes");
        case ContentImportStage::Publishing:
            return QStringLiteral("Publishing");
    }
    return {};
}

/// Draws the status text and, while a row imports, a thin progress bar along the bottom of its cell.
class ContentStatusDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        // A status reads from its start ("Failed: Source decoding…"); the tooltip carries the whole reason.
        option->textElideMode = Qt::ElideRight;
    }

public:
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        const double progress = index.data(CONTENT_PROGRESS_ROLE).toDouble();
        if (progress < 0.0)
        {
            return;
        }
        const QRect track(option.rect.left() + design::SPACING_EXTRA_SMALL,
                          option.rect.bottom() - ROW_PROGRESS_HEIGHT - design::SPACING_EXTRA_SMALL / 2,
                          option.rect.width() - 2 * design::SPACING_EXTRA_SMALL,
                          ROW_PROGRESS_HEIGHT);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(Qt::NoPen);
        painter->setBrush(option.palette.color(QPalette::Mid));
        painter->drawRoundedRect(track, ROW_PROGRESS_HEIGHT * 0.5, ROW_PROGRESS_HEIGHT * 0.5);
        QRect filled = track;
        filled.setWidth(qRound(track.width() * qBound(0.0, progress, 1.0)));
        painter->setBrush(option.palette.color(QPalette::Link));
        painter->drawRoundedRect(filled, ROW_PROGRESS_HEIGHT * 0.5, ROW_PROGRESS_HEIGHT * 0.5);
        painter->restore();
    }
};
} // namespace

ContentWorkspace::ContentWorkspace(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("contentWorkspace"));
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(design::SPACING_SMALL);
    Search_ = new QLineEdit(this);
    Search_->setObjectName(QStringLiteral("contentSearch"));
    Search_->setAccessibleName(QStringLiteral("Search content by name, type or path"));
    Search_->setPlaceholderText(QStringLiteral("Search name, type or path"));
    Search_->setClearButtonEnabled(true);
    layout->addWidget(Search_);
    // Type filter chips: none checked shows every type; each checked chip adds its category.
    QHBoxLayout* chips = new QHBoxLayout;
    chips->setSpacing(design::SPACING_EXTRA_SMALL);
    for (foundation::usize index = 0; index < CONTENT_CATEGORY_COUNT; ++index)
    {
        const ContentCategory category = static_cast<ContentCategory>(index);
        QToolButton* chip = new QToolButton(this);
        chip->setObjectName(QStringLiteral("contentFilter") + GetContentCategoryName(category));
        chip->setCheckable(true);
        chip->setAutoRaise(true);
        chip->setProperty("visualRole", QStringLiteral("toggle"));
        chip->setAccessibleName(QStringLiteral("Show only ") + GetContentCategoryName(category).toLower());
        connect(chip, &QToolButton::toggled, this, [this]() { applyFilter(); });
        chips->addWidget(chip);
        mChips.append(chip);
    }
    chips->addStretch();
    layout->addLayout(chips);
    Model_ = new ContentCatalogModel(this);
    mThumbnailsOrNull = new ContentThumbnailCache(this);
    Model_->SetThumbnailCache(mThumbnailsOrNull);
    mFilterOrNull = new ContentFilterProxyModel(this);
    mFilterOrNull->SetCatalogModel(Model_);
    Table_ = new QTableView(this);
    Table_->setObjectName(QStringLiteral("contentTable"));
    Table_->setAccessibleName(QStringLiteral("Project content resources"));
    Table_->setModel(mFilterOrNull);
    Table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    ConfigureItemSelection(*Table_);
    Table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    Table_->setSortingEnabled(true);
    // One line per row keeps the list scannable and every row the same height; long IDs and paths lose their
    // middle, where catalog keys differ least, and the tooltip carries the full text.
    Table_->setWordWrap(false);
    Table_->setTextElideMode(Qt::ElideMiddle);
    Table_->sortByColumn(0, Qt::AscendingOrder);
    Table_->setIconSize(QSize(CONTENT_THUMBNAIL_SIZE, CONTENT_THUMBNAIL_SIZE));
    Table_->setContextMenuPolicy(Qt::CustomContextMenu);
    Table_->setItemDelegateForColumn(static_cast<int>(ContentColumn::Status), new ContentStatusDelegate(Table_));
    // Fixed row heights let the view lay out thousands of rows without measuring them.
    Table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    Table_->verticalHeader()->setDefaultSectionSize(CONTENT_THUMBNAIL_SIZE + design::SPACING_EXTRA_SMALL);
    Table_->verticalHeader()->hide();
    Table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    Table_->horizontalHeader()->setStretchLastSection(true);
    Table_->setColumnWidth(static_cast<int>(ContentColumn::Name), NAME_COLUMN_WIDTH);
    Table_->setColumnWidth(static_cast<int>(ContentColumn::Type), TYPE_COLUMN_WIDTH);
    Table_->setColumnWidth(static_cast<int>(ContentColumn::Status), STATUS_COLUMN_WIDTH);
    layout->addWidget(Table_, 1);
    mNoMatchesOrNull = new QLabel(this);
    mNoMatchesOrNull->setObjectName(QStringLiteral("contentNoMatches"));
    mNoMatchesOrNull->setWordWrap(true);
    mNoMatchesOrNull->setProperty("visualRole", QStringLiteral("secondary"));
    mNoMatchesOrNull->hide();
    layout->addWidget(mNoMatchesOrNull);
    // Overall import progress: which import of how many, its stage, and the bar for the whole batch.
    mProgressRowOrNull = new QWidget(this);
    mProgressRowOrNull->setObjectName(QStringLiteral("contentProgress"));
    QHBoxLayout* progressLayout = new QHBoxLayout(mProgressRowOrNull);
    progressLayout->setContentsMargins(0, 0, 0, 0);
    progressLayout->setSpacing(design::SPACING_SMALL);
    mProgressLabelOrNull = new QLabel(mProgressRowOrNull);
    mProgressLabelOrNull->setObjectName(QStringLiteral("contentProgressLabel"));
    mProgressBarOrNull = new QProgressBar(mProgressRowOrNull);
    mProgressBarOrNull->setObjectName(QStringLiteral("contentProgressBar"));
    mProgressBarOrNull->setRange(0, PROGRESS_STEPS);
    mProgressBarOrNull->setTextVisible(false);
    mProgressBarOrNull->setAccessibleName(QStringLiteral("Import progress"));
    progressLayout->addWidget(mProgressLabelOrNull, 1);
    progressLayout->addWidget(mProgressBarOrNull, 1);
    mProgressRowOrNull->hide();
    layout->addWidget(mProgressRowOrNull);
    auto* actions = new QHBoxLayout;
    layout->addLayout(actions);
    const auto button = [this, actions](const QString& label, const char* name) {
        auto* value = new QPushButton(label, this);
        value->setObjectName(QString::fromLatin1(name));
        actions->addWidget(value);
        return value;
    };
    Refresh_ = button(QStringLiteral("Refresh"), "contentRefresh");
    Import_ = button(QStringLiteral("Import WAV / FLAC…"), "contentImport");
    Reimport_ = button(QStringLiteral("Reimport selected…"), "contentReimport");
    mRetryOrNull = button(QStringLiteral("Retry import"), "contentRetry");
    Cancel_ = button(QStringLiteral("Cancel operation"), "contentCancel");
    Status_ = new QLabel(QStringLiteral("Open a project to browse content/catalog.json."), this);
    Status_->setObjectName(QStringLiteral("contentStatus"));
    Status_->setWordWrap(true);
    Status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mProblemOrNull = new StateView(this);
    mProblemOrNull->setObjectName(QStringLiteral("contentProblem"));
    mProblemOrNull->hide();
    connect(mProblemOrNull, &StateView::ActionTriggered, this, [this]() {
        if (mProblemImportId.isEmpty() == false)
        {
            (void)RetryImport(mProblemImportId);
            return;
        }
        Refresh();
    });
    // Centre the problem with stretches, not an alignment flag: an aligned item loses its height-for-width, which
    // clipped the wrapped sentence, cause and fix.
    QHBoxLayout* problemRow = new QHBoxLayout;
    problemRow->addStretch();
    problemRow->addWidget(mProblemOrNull, 1);
    problemRow->addStretch();
    layout->addLayout(problemRow);
    layout->addWidget(Status_);
    Jobs_ = new ContentJobs(this);
    connect(Jobs_, &ContentJobs::Completed, this, &ContentWorkspace::Accept);
    connect(Jobs_, &ContentJobs::ProgressChanged, this, [this]() { renderProgress(); });
    connect(Jobs_, &ContentJobs::BusyChanged, this, [this]() {
        if (Jobs_->Busy() == false && Closing_ == false && mImportQueue.isEmpty() == false)
        {
            startNextImport();
        }
        RenderActions();
        renderProgress();
        Q_EMIT BusyChanged();
        if (RefreshPending_ && Busy() == false && Closing_ == false)
        {
            RefreshPending_ = false;
            Refresh();
        }
    });
    connect(Search_, &QLineEdit::textChanged, this, [this]() { applyFilter(); });
    connect(Table_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& index) {
        if (Applying_ == false && index.isValid())
        {
            SelectedId_ = index.data(Qt::UserRole).toString();
        }
        RenderActions();
    });
    connect(Table_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() {
        if (Applying_ == true)
        {
            return;
        }
        mSelectedIds.clear();
        for (const QModelIndex& index : Table_->selectionModel()->selectedRows())
        {
            mSelectedIds.append(index.data(Qt::UserRole).toString());
        }
    });
    connect(Table_, &QTableView::activated, this, [this](const QModelIndex& index) {
        const ContentRow* row = Model_->Row(mFilterOrNull->mapToSource(index));
        if (row != nullptr && (row->Type == ContentType::Sound || row->Type == ContentType::Music))
        {
            Q_EMIT OpenAudio(row->Id);
        }
        else if (row != nullptr && row->Type == ContentType::Mesh)
        {
            Status_->setText(QStringLiteral("Mesh editing is not available yet."));
        }
    });
    connect(Table_, &QTableView::customContextMenuRequested, this, [this](const QPoint& position) {
        const QModelIndex index = Table_->indexAt(position);
        const QString id = index.data(Qt::UserRole).toString();
        if (id.isEmpty() == true || mFailedImports.contains(id) == false)
        {
            return;
        }
        QMenu menu(this);
        menu.addAction(QStringLiteral("Retry import"), this, [this, id]() { (void)RetryImport(id); });
        menu.addAction(QStringLiteral("Dismiss"), this, [this, id]() { DismissFailure(id); });
        menu.exec(Table_->viewport()->mapToGlobal(position));
    });
    connect(Refresh_, &QPushButton::clicked, this, &ContentWorkspace::Refresh);
    connect(Import_, &QPushButton::clicked, this, [this]() { ChooseImport(false); });
    connect(Reimport_, &QPushButton::clicked, this, [this]() { ChooseImport(true); });
    connect(mRetryOrNull, &QPushButton::clicked, this, [this]() { (void)RetryImport(SelectedId()); });
    connect(Cancel_, &QPushButton::clicked, this, &ContentWorkspace::Cancel);
    mWatchDelay.setSingleShot(true);
    mWatchDelay.setInterval(WATCH_DELAY_MILLISECONDS);
    connect(&mWatchDelay, &QTimer::timeout, this, [this]() {
        if (Busy() == true)
        {
            RefreshPending_ = true;
            return;
        }
        Refresh();
    });
#if !defined(Q_OS_WASM)
    // A scene import from the World or an outside cook replaces assets/catalog.json; refresh so the list and its
    // thumbnails follow the new artifact keys without a manual Refresh.
    mWatcherOrNull = new QFileSystemWatcher(this);
    connect(mWatcherOrNull, &QFileSystemWatcher::directoryChanged, this, [this]() { mWatchDelay.start(); });
#endif
    applyFilter();
    RenderActions();
}
ContentWorkspace::~ContentWorkspace()
{
    // Disconnect before QWidget destroys models and focused child controls.
    for (auto* child : findChildren<QObject*>())
    {
        disconnect(child, nullptr, this, nullptr);
    }
    Shutdown();
}
void ContentWorkspace::SetProject(const QString& root, foundation::uint64 epoch, const QString& assetRoot)
{
    if (Root_ == root && Epoch_ == epoch && mAssetRoot == assetRoot)
    {
        return;
    }
    (void)Jobs_->Cancel();
    clearImports();
    Root_ = root;
    mAssetRoot = assetRoot;
    Epoch_ = epoch;
    ActiveOperation_ = 0;
    HasCatalog_ = false;
    SelectedId_.clear();
    mSelectedIds.clear();
    mCatalogRows.clear();
    Applying_ = true;
    Model_->SetRoot(assetRoot);
    Model_->SetRows({});
    mProblemOrNull->hide();
    Applying_ = false;
    watchCatalogs();
    RefreshPending_ = Busy() && Root_.isEmpty() == false;
    if (Root_.isEmpty() == false && Busy() == false)
    {
        Refresh();
    }
    else if (Root_.isEmpty())
    {
        Status_->setText(QStringLiteral("Open a project to browse content/catalog.json."));
    }
    applyFilter();
    RenderActions();
}
ContentRequest ContentWorkspace::Request() const
{
    ContentRequest request;
    request.ProjectEpoch = Epoch_;
    request.Root = Root_;
    request.CheckCatalog = HasCatalog_;
    request.CatalogDigest = CatalogDigest_;
    request.AssetRoot = mAssetRoot;
    return request;
}
void ContentWorkspace::Refresh()
{
    if (Root_.isEmpty() || Busy() || Closing_ || NextOperation_ == ~foundation::uint64{0})
    {
        return;
    }
    auto request = Request();
    request.Operation = ++NextOperation_;
    ActiveOperation_ = request.Operation;
    // A refresh retries a catalog problem; an import failure stays until it is retried or dismissed.
    if (mProblemImportId.isEmpty() == true)
    {
        hideProblem();
    }
    Status_->setText(QStringLiteral("Reading content catalog…"));
    if (Jobs_->Start(request) == false)
    {
        Status_->setText(QStringLiteral("Content worker unavailable; retry Refresh."));
    }
}
bool ContentWorkspace::Import(const QString& file, const QString& id)
{
    // A direct import starts now or not at all; QueueImport waits behind the running work instead.
    if (Busy() == true)
    {
        return false;
    }
    return QueueImport(file, id);
}
bool ContentWorkspace::QueueImport(const QString& file, const QString& id)
{
    if (Root_.isEmpty() || Closing_ || id.isEmpty() || NextOperation_ == ~foundation::uint64{0})
    {
        return false;
    }
    for (const QueuedImport& queued : mImportQueue)
    {
        if (queued.Id == id)
        {
            return false;
        }
    }
    if (mHasActiveImport == true && mActiveImport.Id == id)
    {
        return false;
    }
    mFailedImports.remove(id);
    mImportQueue.append({ .File = file, .Id = id });
    ++mBatchTotal;
    Model_->SetOperation(id, { .Phase = ContentOperationPhase::Queued, .Progress = 0.0, .Message = {} });
    applyRows();
    if (mProblemImportId == id)
    {
        hideProblem();
    }
    if (Jobs_->Busy() == false)
    {
        startNextImport();
    }
    RenderActions();
    renderProgress();
    Q_EMIT BusyChanged();
    return mHasActiveImport == true || mImportQueue.isEmpty() == false;
}
bool ContentWorkspace::RetryImport(const QString& id)
{
    if (mFailedImports.contains(id) == false)
    {
        return false;
    }
    // Take the failure out first: QueueImport clears it anyway, and the file and ID must outlive that.
    const QueuedImport retry = mFailedImports.take(id);
    if (QueueImport(retry.File, retry.Id) == false)
    {
        mFailedImports.insert(id, retry);
        return false;
    }
    return true;
}
void ContentWorkspace::DismissFailure(const QString& id)
{
    if (mFailedImports.remove(id) == false)
    {
        return;
    }
    Model_->ClearOperation(id);
    if (mProblemImportId == id)
    {
        hideProblem();
    }
    applyRows();
    RenderActions();
}
void ContentWorkspace::startNextImport()
{
    if (mImportQueue.isEmpty() == true || Jobs_->Busy() == true || Closing_ == true)
    {
        return;
    }
    mActiveImport = mImportQueue.takeFirst();
    mHasActiveImport = true;
    auto request = Request();
    request.Operation = ++NextOperation_;
    request.File = mActiveImport.File;
    request.Id = mActiveImport.Id;
    ActiveOperation_ = request.Operation;
    Model_->SetOperation(mActiveImport.Id,
                         {
                             .Phase = ContentOperationPhase::Importing,
                             .Progress = 0.0,
                             .Message = GetStageText(ContentImportStage::Waiting),
                         });
    Status_->setText(QStringLiteral("Reading and validating source and saved dependencies…"));
    if (Jobs_->Start(request) == false)
    {
        ContentResult failed;
        failed.Request = std::move(request);
        failed.Status = content::Status::IoError;
        failed.Message = QStringLiteral("Content worker unavailable; retry the import.");
        Accept(failed);
        // No worker ran, so no completion will start the next import; continue the batch here.
        startNextImport();
    }
}
void ContentWorkspace::ChooseImport(bool reimport)
{
    if (Root_.isEmpty() || Closing_)
    {
        Status_->setText(QStringLiteral("Open a project, then retry Import."));
        return;
    }
    const auto identity = Epoch_;
    const QString root = Root_;
    const auto selected = SelectedId();
    OpenEditorDocument(this,
                       {
                           .Title = reimport ? QStringLiteral("Reimport source") : QStringLiteral("Import source"),
                           .Directory = {},
                           .Filter = QStringLiteral("Audio (*.wav *.flac)"),
                           .ProjectFolder = false,
                           .IsAudioSource = true,
                       },
                       [this, identity, root, selected, reimport](const QString& file) {
                           if (identity != Epoch_ || root != Root_ || Closing_ == true)
                           {
                               return;
                           }
                           QString id = selected;
                           if (reimport == false)
                           {
                               bool isAccepted = false;
                               id = QInputDialog::getText(this,
                                                          QStringLiteral("Stable resource ID"),
                                                          QStringLiteral("Lowercase resource ID (not a file path)"),
                                                          QLineEdit::Normal,
                                                          QStringLiteral("source/") +
                                                              QFileInfo(file).completeBaseName().toLower(),
                                                          &isAccepted);
                               if (isAccepted == false || identity != Epoch_ || root != Root_)
                               {
                                   return;
                               }
                           }
                           (void)QueueImport(file, id);
                       });
}
void ContentWorkspace::Accept(const ContentResult& result)
{
    if (Closing_ || result.Request.ProjectEpoch != Epoch_ || result.Request.Root != Root_ ||
        result.Request.Operation != ActiveOperation_)
    {
        return;
    }
    Status_->setText(result.Message);
    if (result.Request.Id.isEmpty() == false)
    {
        finishImport(result);
    }
    showProblem(result);
    if (result.Status != content::Status::Ok)
    {
        return;
    }
    if (result.Request.Id.isEmpty() == false)
    {
#if defined(Q_OS_WASM)
        MarkBrowserEdited();
#endif
        SelectedId_ = result.Request.Id;
        mSelectedIds = {SelectedId_};
        // Retain the explicit import result while refreshing its new catalog.
        RefreshPending_ = true;
        HasCatalog_ = false;
        Q_EMIT CatalogChanged();
        return;
    }
    CatalogDigest_ = result.CatalogDigest;
    HasCatalog_ = result.HasAudioCatalog;
    mCatalogRows = result.Rows;
    applyRows();
}
void ContentWorkspace::finishImport(const ContentResult& result)
{
    const QString& id = result.Request.Id;
    if (mHasActiveImport == false || mActiveImport.Id != id)
    {
        return;
    }
    const QueuedImport finished = mActiveImport;
    mHasActiveImport = false;
    ++mBatchDone;
    if (result.Status == content::Status::Ok || result.Status == content::Status::Cancelled)
    {
        // The refreshed catalog row replaces the pending one; a cancelled import simply leaves the list.
        Model_->ClearOperation(id);
        mThumbnailsOrNull->Invalidate(id);
    }
    else
    {
        mFailedImports.insert(id, finished);
        Model_->SetOperation(id,
                             {
                                 .Phase = ContentOperationPhase::Failed,
                                 .Progress = 0.0,
                                 .Message = result.Message,
                             });
    }
    applyRows();
    renderProgress();
}
void ContentWorkspace::showProblem(const ContentResult& result)
{
    // A failure says what failed, why (the worker's message), and what to do. Cancelling is the user's choice,
    // not a problem.
    if (result.Status == content::Status::Ok || result.Status == content::Status::Cancelled ||
        result.Status == content::Status::Pending)
    {
        // A successful catalog read clears a catalog problem, and a successful import clears its own failure.
        if (result.Request.Id == mProblemImportId)
        {
            hideProblem();
        }
        return;
    }
    const bool isImport = result.Request.Id.isEmpty() == false;
    StateViewContent state;
    state.Icon = EditorIcon::Error;
    state.Message = isImport ? QStringLiteral("The import of %1 did not finish.").arg(result.Request.Id)
                             : QStringLiteral("The content catalog could not be read.");
    state.Cause = result.Message;
    state.Details = result.Message;
    switch (result.Status)
    {
        case content::Status::Limit:
            state.Fix = QStringLiteral("Use a smaller file or fewer entries, then try again.");
            break;
        case content::Status::OutOfMemory:
            state.Fix = QStringLiteral("Close other applications or use a smaller file, then try again.");
            break;
        case content::Status::IoError:
            state.Fix =
                QStringLiteral("Check that the file exists and the project folder can be written, then try again.");
            break;
        case content::Status::Conflict:
            state.Fix = QStringLiteral("The catalog changed underneath this action. Refresh, then repeat it.");
            break;
        case content::Status::NotFound:
            state.Fix = QStringLiteral("Check the file path and the resource ID, then try again.");
            break;
        case content::Status::Unsupported:
            state.Fix = QStringLiteral("Use a WAV or FLAC file.");
            break;
        default:
            state.Fix = QStringLiteral("Fix the source file or catalog entry the message names, then try again.");
            break;
    }
    if (isImport == false)
    {
        state.ActionLabel = QStringLiteral("Refresh");
        mProblemImportId.clear();
    }
    else
    {
        // The failed row keeps its reason, so retrying needs no new file choice or ID.
        state.ActionLabel = QStringLiteral("Retry import");
        mProblemImportId = result.Request.Id;
    }
    mProblemOrNull->SetContent(state);
    mProblemOrNull->show();
}
void ContentWorkspace::hideProblem()
{
    mProblemImportId.clear();
    mProblemOrNull->hide();
}
void ContentWorkspace::applyRows()
{
    // Imports of IDs the catalog does not list yet appear as pending rows, so their progress and failures show
    // where the asset will be.
    QVector<ContentRow> rows = mCatalogRows;
    QSet<QString> listed;
    listed.reserve(rows.size());
    for (const ContentRow& row : rows)
    {
        listed.insert(row.Id);
    }
    const auto addPending = [&rows, &listed](const QueuedImport& item) {
        if (listed.contains(item.Id) == true)
        {
            return;
        }
        listed.insert(item.Id);
        ContentRow row;
        row.Id = item.Id;
        row.Path = QFileInfo(item.File).fileName();
        row.Type = ContentType::AudioSource;
        row.IsPending = true;
        rows.append(row);
    };
    if (mHasActiveImport == true)
    {
        addPending(mActiveImport);
    }
    for (const QueuedImport& item : mImportQueue)
    {
        addPending(item);
    }
    for (const QueuedImport& item : mFailedImports)
    {
        addPending(item);
    }
    Applying_ = true;
    Model_->SetRows(rows);
    Applying_ = false;
    RestoreSelection();
    renderChips();
}
void ContentWorkspace::applyFilter()
{
    ContentCategoryMask mask = 0;
    for (qsizetype index = 0; index < mChips.size(); ++index)
    {
        if (mChips[index]->isChecked() == true)
        {
            mask |= GetContentCategoryBit(static_cast<ContentCategory>(index));
        }
    }
    Applying_ = true;
    mFilterOrNull->SetSearchText(Search_->text());
    mFilterOrNull->SetCategoryMask(mask);
    Applying_ = false;
    RestoreSelection();
    renderChips();
}
void ContentWorkspace::renderChips()
{
    const QVector<int> counts = mFilterOrNull->CountCategories();
    for (qsizetype index = 0; index < mChips.size(); ++index)
    {
        const ContentCategory category = static_cast<ContentCategory>(index);
        QToolButton* chip = mChips[index];
        chip->setText(QStringLiteral("%1 %2").arg(GetContentCategoryName(category)).arg(counts[index]));
        // An empty category stays visible, so the row of chips never shifts, but cannot be chosen unless it is on.
        chip->setEnabled(counts[index] > 0 || chip->isChecked() == true);
    }
    const bool hasHiddenEverything = Model_->rowCount() > 0 && mFilterOrNull->rowCount() == 0;
    if (hasHiddenEverything == true)
    {
        mNoMatchesOrNull->setText(QStringLiteral("No assets match this search and filter. Clear the search or turn "
                                                 "off a filter to see all %1.")
                                      .arg(Model_->rowCount()));
    }
    mNoMatchesOrNull->setVisible(hasHiddenEverything);
}
void ContentWorkspace::renderProgress()
{
    const bool isActive = mHasActiveImport == true || mImportQueue.isEmpty() == false;
    if (isActive == false)
    {
        mBatchTotal = 0;
        mBatchDone = 0;
        mProgressRowOrNull->hide();
        return;
    }
    double current = 0.0;
    QString stage = GetStageText(ContentImportStage::Waiting);
    if (mHasActiveImport == true)
    {
        const ContentImportProgress progress = Jobs_->GetProgress();
        current = GetContentImportFraction(progress);
        stage = GetStageText(progress.Stage);
        if (progress.Stage == ContentImportStage::Validating && progress.TotalSteps > 1)
        {
            stage += QStringLiteral(" (%1 of %2)").arg(progress.CompletedSteps).arg(progress.TotalSteps);
        }
        Model_->SetOperation(mActiveImport.Id,
                             {
                                 .Phase = ContentOperationPhase::Importing,
                                 .Progress = current,
                                 .Message = GetStageText(progress.Stage),
                             });
    }
    const int total = qMax(1, mBatchTotal);
    const int position = qMin(total, mBatchDone + 1);
    mProgressLabelOrNull->setText(QStringLiteral("Importing %1 of %2 · %3 · %4")
                                      .arg(position)
                                      .arg(total)
                                      .arg(mHasActiveImport == true ? mActiveImport.Id : QString())
                                      .arg(stage));
    mProgressBarOrNull->setValue(qRound(PROGRESS_STEPS * (mBatchDone + current) / total));
    mProgressRowOrNull->show();
}
void ContentWorkspace::clearImports()
{
    for (const QueuedImport& item : mImportQueue)
    {
        Model_->ClearOperation(item.Id);
    }
    for (const QueuedImport& item : mFailedImports)
    {
        Model_->ClearOperation(item.Id);
    }
    if (mHasActiveImport == true)
    {
        Model_->ClearOperation(mActiveImport.Id);
    }
    mImportQueue.clear();
    mFailedImports.clear();
    mHasActiveImport = false;
    mBatchTotal = 0;
    mBatchDone = 0;
    hideProblem();
    renderProgress();
}
void ContentWorkspace::watchCatalogs()
{
#if !defined(Q_OS_WASM)
    const QStringList watched = mWatcherOrNull->directories();
    if (watched.isEmpty() == false)
    {
        (void)mWatcherOrNull->removePaths(watched);
    }
    QStringList folders;
    if (Root_.isEmpty() == false && QFileInfo(Root_).isDir() == true)
    {
        folders.append(Root_);
    }
    const QString assets =
        mAssetRoot.isEmpty() == true ? QString() : QDir(mAssetRoot).filePath(QStringLiteral("assets"));
    if (assets.isEmpty() == false && QFileInfo(assets).isDir() == true)
    {
        folders.append(assets);
    }
    if (folders.isEmpty() == false)
    {
        (void)mWatcherOrNull->addPaths(folders);
    }
#endif
}
bool ContentWorkspace::event(QEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    constexpr QEvent::Type RATIO_CHANGE = QEvent::DevicePixelRatioChange;
#else
    // Before Qt 6.6 a widget hears of a new device pixel ratio only through the screen change it comes with.
    constexpr QEvent::Type RATIO_CHANGE = QEvent::ScreenChangeInternal;
#endif
    if (event->type() == RATIO_CHANGE || event->type() == QEvent::Show)
    {
        Model_->SetDevicePixelRatio(devicePixelRatioF());
    }
    return QWidget::event(event);
}
void ContentWorkspace::RestoreSelection()
{
    const QModelIndex index = mFilterOrNull->mapFromSource(Model_->FindId(SelectedId_));
    Applying_ = true;
    Table_->selectionModel()->clearSelection();
    for (const QString& id : mSelectedIds)
    {
        const QModelIndex selected = mFilterOrNull->mapFromSource(Model_->FindId(id));
        if (selected.isValid() == true)
        {
            Table_->selectionModel()->select(selected, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
    }
    Table_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
    Applying_ = false;
    RenderActions();
}
QString ContentWorkspace::SelectedId() const
{
    return Table_->currentIndex().isValid() ? Table_->currentIndex().data(Qt::UserRole).toString() : QString();
}
ContentThumbnailCache& ContentWorkspace::GetThumbnails() noexcept
{
    return *mThumbnailsOrNull;
}
void ContentWorkspace::RenderActions()
{
    const bool isAvailable = Root_.isEmpty() == false && Busy() == false && Closing_ == false;
    const bool canImport = Root_.isEmpty() == false && Closing_ == false;
    Refresh_->setEnabled(isAvailable);
    // Imports queue behind the active one, so Import stays available while another runs.
    Import_->setEnabled(canImport);
    const ContentRow* row = Model_->Row(mFilterOrNull->mapToSource(Table_->currentIndex()));
    Reimport_->setEnabled(canImport && row != nullptr && row->Type == ContentType::AudioSource &&
                          row->IsPending == false);
    mRetryOrNull->setEnabled(canImport && row != nullptr && mFailedImports.contains(row->Id) == true);
    mRetryOrNull->setVisible(mFailedImports.isEmpty() == false);
    Cancel_->setEnabled(Busy() && Closing_ == false);
}
void ContentWorkspace::Cancel()
{
    // Cancelling stops the whole batch: queued imports leave the list, the active one stops before publication.
    for (const QueuedImport& item : mImportQueue)
    {
        Model_->ClearOperation(item.Id);
    }
    mImportQueue.clear();
    applyRows();
    Status_->setText(Jobs_->Cancel() ? QStringLiteral("Cancellation requested; waiting for acknowledgement…")
                                     : QStringLiteral("Publication already began; waiting for its result…"));
    renderProgress();
    RenderActions();
}
void ContentWorkspace::Shutdown()
{
    Closing_ = true;
    mImportQueue.clear();
    mWatchDelay.stop();
    Jobs_->Shutdown();
    RenderActions();
}
bool ContentWorkspace::Finished() const noexcept
{
    return Jobs_->Finished();
}
bool ContentWorkspace::Busy() const noexcept
{
    return Jobs_->Busy() || mImportQueue.isEmpty() == false;
}
bool ContentWorkspace::Importing() const noexcept
{
    return Jobs_->Importing() || mImportQueue.isEmpty() == false;
}
} // namespace ludus::editor
