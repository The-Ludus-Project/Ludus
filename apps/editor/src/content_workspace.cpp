#include "internal/content_browser.h"
#include "internal/item_selection.h"

#include <QAbstractItemView>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>

namespace ludus::editor
{
ContentWorkspace::ContentWorkspace(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("contentWorkspace"));
    auto* layout = new QVBoxLayout(this);
    Search_ = new QLineEdit(this);
    Search_->setObjectName(QStringLiteral("contentSearch"));
    Search_->setAccessibleName(QStringLiteral("Search content by resource ID, kind or path"));
    Search_->setPlaceholderText(QStringLiteral("Search resource ID, kind or path"));
    layout->addWidget(Search_);
    Model_ = new ContentCatalogModel(this);
    Proxy_ = new QSortFilterProxyModel(this);
    Proxy_->setSourceModel(Model_);
    Proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    Proxy_->setFilterKeyColumn(-1);
    Table_ = new QTableView(this);
    Table_->setObjectName(QStringLiteral("contentTable"));
    Table_->setAccessibleName(QStringLiteral("Project content resources"));
    Table_->setModel(Proxy_);
    Table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    ConfigureItemSelection(*Table_);
    Table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    Table_->setSortingEnabled(true);
    Table_->sortByColumn(0, Qt::AscendingOrder);
    Table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    Table_->horizontalHeader()->setStretchLastSection(true);
    Table_->setColumnWidth(0, 230);
    Table_->setColumnWidth(1, 100);
    layout->addWidget(Table_, 1);
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
    Cancel_ = button(QStringLiteral("Cancel operation"), "contentCancel");
    Status_ = new QLabel(QStringLiteral("Open a project to browse content/catalog.json."), this);
    Status_->setObjectName(QStringLiteral("contentStatus"));
    Status_->setWordWrap(true);
    Status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(Status_);
    Jobs_ = new ContentJobs(this);
    connect(Jobs_, &ContentJobs::Completed, this, &ContentWorkspace::Accept);
    connect(Jobs_, &ContentJobs::BusyChanged, this, [this]() {
        RenderActions();
        Q_EMIT BusyChanged();
        if (RefreshPending_ && Busy() == false && Closing_ == false)
        {
            RefreshPending_ = false;
            Refresh();
        }
    });
    connect(Search_, &QLineEdit::textChanged, this, [this](const QString& query) {
        Applying_ = true;
        Proxy_->setFilterFixedString(query);
        Applying_ = false;
        RestoreSelection();
    });
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
        const auto* row = Model_->Row(Proxy_->mapToSource(index));
        if (row != nullptr && (row->Kind == content::Kind::Sound || row->Kind == content::Kind::Music))
        {
            Q_EMIT OpenAudio(row->Id);
        }
        else if (row != nullptr && row->Kind == content::Kind::Mesh)
        {
            Status_->setText(QStringLiteral("Mesh editing is not available yet."));
        }
    });
    connect(Refresh_, &QPushButton::clicked, this, &ContentWorkspace::Refresh);
    connect(Import_, &QPushButton::clicked, this, [this]() { ChooseImport(false); });
    connect(Reimport_, &QPushButton::clicked, this, [this]() { ChooseImport(true); });
    connect(Cancel_, &QPushButton::clicked, this, &ContentWorkspace::Cancel);
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
void ContentWorkspace::SetProject(const QString& root, foundation::uint64 epoch)
{
    if (Root_ == root && Epoch_ == epoch)
    {
        return;
    }
    (void)Jobs_->Cancel();
    Root_ = root;
    Epoch_ = epoch;
    ActiveOperation_ = 0;
    HasCatalog_ = false;
    SelectedId_.clear();
    mSelectedIds.clear();
    Applying_ = true;
    Model_->SetRows({});
    Applying_ = false;
    RefreshPending_ = Busy() && Root_.isEmpty() == false;
    if (Root_.isEmpty() == false && Busy() == false)
    {
        Refresh();
    }
    else if (Root_.isEmpty())
    {
        Status_->setText(QStringLiteral("Open a project to browse content/catalog.json."));
    }
    RenderActions();
}
ContentRequest ContentWorkspace::Request() const
{
    ContentRequest request;
    request.ProjectEpoch = Epoch_;
    request.Root = Root_;
    request.CheckCatalog = HasCatalog_;
    request.CatalogDigest = CatalogDigest_;
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
    Status_->setText(QStringLiteral("Reading content catalog…"));
    if (Jobs_->Start(request) == false)
    {
        Status_->setText(QStringLiteral("Content worker unavailable; retry Refresh."));
    }
}
bool ContentWorkspace::Import(const QString& file, const QString& id)
{
    if (Root_.isEmpty() || Busy() || Closing_ || id.isEmpty() || NextOperation_ == ~foundation::uint64{0})
    {
        return false;
    }
    auto request = Request();
    request.Operation = ++NextOperation_;
    request.File = file;
    request.Id = id;
    ActiveOperation_ = request.Operation;
    Status_->setText(QStringLiteral("Reading and validating source and saved dependencies…"));
    return Jobs_->Start(request);
}
void ContentWorkspace::ChooseImport(bool reimport)
{
    if (Busy() || Root_.isEmpty() || Closing_)
    {
        Status_->setText(QStringLiteral("Finish or cancel the active content operation, then retry Import."));
        return;
    }
    const auto identity = Epoch_;
    const auto selected = SelectedId();
    const auto file =
        QFileDialog::getOpenFileName(this,
                                     reimport ? QStringLiteral("Reimport source") : QStringLiteral("Import source"),
                                     {},
                                     QStringLiteral("Audio (*.wav *.flac)"));
    if (file.isEmpty() || identity != Epoch_)
    {
        return;
    }
    QString id = selected;
    if (reimport == false)
    {
        bool accepted = false;
        id = QInputDialog::getText(this,
                                   QStringLiteral("Stable resource ID"),
                                   QStringLiteral("Lowercase resource ID (not a file path)"),
                                   QLineEdit::Normal,
                                   QStringLiteral("source/") + QFileInfo(file).completeBaseName().toLower(),
                                   &accepted);
        if (accepted == false || identity != Epoch_)
        {
            return;
        }
    }
    (void)Import(file, id);
}
void ContentWorkspace::Accept(const ContentResult& result)
{
    if (Closing_ || result.Request.ProjectEpoch != Epoch_ || result.Request.Root != Root_ ||
        result.Request.Operation != ActiveOperation_)
    {
        return;
    }
    Status_->setText(result.Message);
    if (result.Status != content::Status::Ok)
    {
        return;
    }
    if (result.Request.Id.isEmpty() == false)
    {
        SelectedId_ = result.Request.Id;
        mSelectedIds = {SelectedId_};
        // Retain the explicit import result while refreshing its new catalog.
        RefreshPending_ = true;
        HasCatalog_ = false;
        Q_EMIT CatalogChanged();
        return;
    }
    CatalogDigest_ = result.CatalogDigest;
    HasCatalog_ = true;
    Applying_ = true;
    Model_->SetRows(result.Rows);
    Applying_ = false;
    RestoreSelection();
}
void ContentWorkspace::RestoreSelection()
{
    const auto index = Proxy_->mapFromSource(Model_->FindId(SelectedId_));
    Applying_ = true;
    Table_->selectionModel()->clearSelection();
    for (const QString& id : mSelectedIds)
    {
        const QModelIndex selected = Proxy_->mapFromSource(Model_->FindId(id));
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
void ContentWorkspace::RenderActions()
{
    const bool available = Root_.isEmpty() == false && Busy() == false && Closing_ == false;
    Refresh_->setEnabled(available);
    Import_->setEnabled(available);
    const auto* row = Model_->Row(Proxy_->mapToSource(Table_->currentIndex()));
    Reimport_->setEnabled(available && row != nullptr && row->Kind == content::Kind::AudioSource);
    Cancel_->setEnabled(Busy() && Closing_ == false);
#if defined(Q_OS_WASM)
    Refresh_->setEnabled(false);
    Import_->setEnabled(false);
    Reimport_->setEnabled(false);
    Status_->setText(QStringLiteral("Content browsing and audio import require the desktop editor."));
#endif
}
void ContentWorkspace::Cancel()
{
    Status_->setText(Jobs_->Cancel() ? QStringLiteral("Cancellation requested; waiting for acknowledgement…")
                                     : QStringLiteral("Publication already began; waiting for its result…"));
}
void ContentWorkspace::Shutdown()
{
    Closing_ = true;
    Jobs_->Shutdown();
    RenderActions();
}
bool ContentWorkspace::Finished() const noexcept
{
    return Jobs_->Finished();
}
bool ContentWorkspace::Busy() const noexcept
{
    return Jobs_->Busy();
}
bool ContentWorkspace::Importing() const noexcept
{
    return Jobs_->Importing();
}
} // namespace ludus::editor
