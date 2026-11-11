#pragma once

#include "internal/content_import_gate.h"
#include "internal/content_thumbnails.h"
#include "internal/content_types.h"

#include <ludus/foundation/base/types.h>

#include <ludus/content/content.h>

#include <QAbstractTableModel>
#include <QHash>
#include <QObject>
#include <QSortFilterProxyModel>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QEvent;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QMenu;
class QProgressBar;
class QPushButton;
class QTableView;
class QToolButton;
QT_END_NAMESPACE

namespace ludus::editor
{
struct ContentRow final
{
    QString Id;
    /// Project-relative path of the authored or cooked file.
    QString Path;
    ContentType Type = ContentType::AudioSource;
    /// Artifact key of a scene asset; it changes whenever the cooked bytes or a dependency's bytes change, so
    /// thumbnails keyed by it are redrawn exactly when needed. Empty for audio catalog entries.
    QString Key;
    /// Human-readable name when the asset records one (a material's source name); empty otherwise.
    QString Label;
    /// Resource IDs this asset depends on (a material's textures).
    QStringList Dependencies;
    /// Whether the row stands for an import that has not reached the catalog yet.
    bool IsPending = false;
    [[nodiscard]] bool operator==(const ContentRow&) const = default;
};

/// Model columns, in display order.
enum class ContentColumn : foundation::uint8
{
    Name,
    Type,
    Status,
    Path,
};

/// Number of values in ContentColumn.
inline constexpr int CONTENT_COLUMN_COUNT = 4;

/// Role holding a row's ContentType as an integer.
inline constexpr int CONTENT_TYPE_ROLE = Qt::UserRole + 1;
/// Role holding a row's operation progress in [0, 1], or a negative value when it has none.
inline constexpr int CONTENT_PROGRESS_ROLE = Qt::UserRole + 2;
/// Role holding whether a row's last import failed.
inline constexpr int CONTENT_FAILED_ROLE = Qt::UserRole + 3;

/// Where an import of one asset stands.
enum class ContentOperationPhase : foundation::uint8
{
    Queued,
    Importing,
    Failed,
};

/// The import state the browser shows on one row.
struct ContentOperation final
{
    ContentOperationPhase Phase = ContentOperationPhase::Queued;
    /// Completed fraction in [0, 1] while importing.
    double Progress = 0.0;
    /// The current stage while importing, or the failure reason.
    QString Message;
    [[nodiscard]] bool operator==(const ContentOperation&) const = default;
};

// UI-thread projection of copied catalog rows. No widget/item allocation per
// row; persisted and selected identities are resource IDs, never row numbers.
// Thumbnails are requested only when a view asks a row for its decoration, so
// only painted rows cost anything.
class ContentCatalogModel final : public QAbstractTableModel
{
public:
    explicit ContentCatalogModel(QObject* parent = nullptr);
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void SetRows(QVector<ContentRow> rows);
    [[nodiscard]] QModelIndex FindId(const QString& id) const;
    [[nodiscard]] const ContentRow* Row(const QModelIndex& index) const;
    [[nodiscard]] const QVector<ContentRow>& GetRows() const noexcept;

    /// Project root that row paths are relative to; thumbnails read artifacts below it.
    void SetRoot(const QString& root);
    /// Attach the thumbnail cache that decorations come from; nullptr draws type icons only. Not owned.
    void SetThumbnailCache(ContentThumbnailCache* cacheOrNull);
    /// Device pixel ratio thumbnails are generated for.
    void SetDevicePixelRatio(qreal ratio);
    /// Show an import state on the row with this ID.
    void SetOperation(const QString& id, const ContentOperation& operation);
    /// Remove the import state of this ID.
    void ClearOperation(const QString& id);
    /// The import state of this ID, or nullptr when it has none.
    [[nodiscard]] const ContentOperation* FindOperation(const QString& id) const;

private:
    void emitRowChanged(const QString& id);
    [[nodiscard]] QVariant decoration(const ContentRow& row) const;

    QVector<ContentRow> Rows_;
    QHash<QString, int> Ids_;
    QHash<QString, ContentOperation> mOperations;
    QString mRoot;
    ContentThumbnailCache* mThumbnailsOrNull = nullptr;
    qreal mDevicePixelRatio = 1.0;
};

/// Whether a row matches every search term (case-insensitive, in its ID, name, type or path) and the category mask
/// (zero accepts every category). Pending imports always match their own ID.
[[nodiscard]] bool DoesContentRowMatch(const ContentRow& row, const QStringList& terms, ContentCategoryMask mask);

/// Split search text into terms; whitespace separates them.
[[nodiscard]] QStringList SplitContentSearch(const QString& text);

/// Live search and type filter over a ContentCatalogModel.
class ContentFilterProxyModel final : public QSortFilterProxyModel
{
public:
    explicit ContentFilterProxyModel(QObject* parentOrNull = nullptr);
    /// Filter this catalog model (also its source model); nullptr detaches. Not owned.
    void SetCatalogModel(ContentCatalogModel* modelOrNull);
    void SetSearchText(const QString& text);
    void SetCategoryMask(ContentCategoryMask mask);
    [[nodiscard]] ContentCategoryMask GetCategoryMask() const noexcept;
    /// Rows of each category that match the search text, ignoring the category mask (the counts the chips show).
    [[nodiscard]] QVector<int> CountCategories() const;

protected:
    [[nodiscard]] bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    void refilter();

    const ContentCatalogModel* mCatalogOrNull = nullptr;
    QStringList mTerms;
    ContentCategoryMask mCategoryMask = 0;
};

struct ContentRequest final
{
    foundation::uint64 Operation = 0;
    foundation::uint64 ProjectEpoch = 0;
    QString Root;
    QString File;
    QString Id; // empty for catalog refresh
    bool CheckCatalog = false;
    content::Digest CatalogDigest;
    /// Project root holding assets/catalog.json; empty skips the scene asset catalog.
    QString AssetRoot;
};
struct ContentResult final
{
    ContentRequest Request;
    content::Status Status = content::Status::Invalid;
    QString Message;
    QString CandidatePath;
    QVector<ContentRow> Rows;
    content::Digest CatalogDigest;
    /// Whether content/catalog.json exists; imports compare-and-swap against CatalogDigest only then.
    bool HasAudioCatalog = false;
    /// Why the scene asset catalog could not be listed; empty when it was listed or is absent.
    QString AssetProblem;
};

/// Read the scene asset catalog below root (assets/catalog.json) into browser rows: every record's ID, kind,
/// artifact path, key and dependencies, and a material's source name. Returns Ok with no rows when the catalog does
/// not exist, and the failing status with outProblem set when it is unreadable or malformed. Call on a worker.
[[nodiscard]] content::Status
ReadSceneAssetRows(const QString& root, QVector<ContentRow>& outRows, QString& outProblem);

// One bounded request at a time. Each worker owns its copied state. Polling
// runs only during an operation; destruction cancels without joining a thread.
// Finished acknowledges owned work before normal window shutdown completes.
// Browser requests yield once before synchronous bounded work on the UI owner;
// cancellation is available before that work begins, not during decode/publication.
class ContentJobs final : public QObject
{
    Q_OBJECT
public:
    explicit ContentJobs(QObject* parent = nullptr);
    ~ContentJobs() override;
    [[nodiscard]] bool Start(const ContentRequest& request);
    [[nodiscard]] bool Cancel();
    void Shutdown();
    [[nodiscard]] bool Busy() const noexcept;
    [[nodiscard]] bool Importing() const noexcept;
    [[nodiscard]] bool Finished() const noexcept;
    /// Progress of the active import, as last reported by its worker.
    [[nodiscard]] ContentImportProgress GetProgress() const noexcept;
Q_SIGNALS:
    void Completed(const ContentResult& result);
    void BusyChanged();
    /// The active import reported a new stage or step.
    void ProgressChanged(const ContentImportProgress& progress);

private:
    void Poll();
    struct Impl;
    Impl* Impl_ = nullptr;
    QTimer PollTimer_;
    ContentImportProgress mLastProgress;
    bool Closing_ = false;
};

class StateView;

class ContentWorkspace final : public QWidget
{
    Q_OBJECT
public:
    explicit ContentWorkspace(QWidget* parent = nullptr);
    ~ContentWorkspace() override;
    /// Browse root (a project's content folder, holding the audio catalog) and assetRoot (the project root, holding
    /// assets/catalog.json); an empty assetRoot lists audio content only.
    void SetProject(const QString& root, foundation::uint64 epoch, const QString& assetRoot = {});
    void Refresh();
    /// Same as QueueImport.
    [[nodiscard]] bool Import(const QString& file, const QString& id);
    /// Import file as id: now when no operation runs, otherwise after the imports already queued. Returns false when
    /// no project is open, the ID is empty or already queued, or the browser is closing.
    [[nodiscard]] bool QueueImport(const QString& file, const QString& id);
    /// Queue the failed import of id again with the same file. Returns false when id has no failed import.
    [[nodiscard]] bool RetryImport(const QString& id);
    /// Forget the failed import of id and its reason.
    void DismissFailure(const QString& id);
    void ChooseImport(bool reimport = false);
    /// Cancel the active operation and drop queued imports.
    void Cancel();
    void Shutdown();
    [[nodiscard]] bool Finished() const noexcept;
    /// Whether an operation runs or imports are queued.
    [[nodiscard]] bool Busy() const noexcept;
    /// Whether an import runs or is queued.
    [[nodiscard]] bool Importing() const noexcept;
    [[nodiscard]] QString SelectedId() const;
    /// The thumbnail cache behind the list's decorations.
    [[nodiscard]] ContentThumbnailCache& GetThumbnails() noexcept;
Q_SIGNALS:
    void BusyChanged();
    void CatalogChanged();
    void OpenAudio(const QString& id);

protected:
    bool event(QEvent* event) override;

private:
    struct QueuedImport final
    {
        QString File;
        QString Id;
    };

    void RenderActions();
    void showProblem(const ContentResult& result);
    void hideProblem();
    void RestoreSelection();
    void Accept(const ContentResult& result);
    void finishImport(const ContentResult& result);
    void startNextImport();
    void applyRows();
    void applyFilter();
    void renderChips();
    void renderProgress();
    void clearImports();
    void watchCatalogs();
    [[nodiscard]] ContentRequest Request() const;
    ContentJobs* Jobs_ = nullptr;
    ContentCatalogModel* Model_ = nullptr;
    ContentFilterProxyModel* mFilterOrNull = nullptr;
    ContentThumbnailCache* mThumbnailsOrNull = nullptr;
    QTableView* Table_ = nullptr;
    QLineEdit* Search_ = nullptr;
    QVector<QToolButton*> mChips;
    QLabel* mNoMatchesOrNull = nullptr;
    QWidget* mProgressRowOrNull = nullptr;
    QLabel* mProgressLabelOrNull = nullptr;
    QProgressBar* mProgressBarOrNull = nullptr;
    QLabel* Status_ = nullptr;
    StateView* mProblemOrNull = nullptr;
    QPushButton* Refresh_ = nullptr;
    QPushButton* Import_ = nullptr;
    QPushButton* Reimport_ = nullptr;
    QPushButton* mRetryOrNull = nullptr;
    QPushButton* Cancel_ = nullptr;
    QFileSystemWatcher* mWatcherOrNull = nullptr;
    QTimer mWatchDelay;
    QString Root_;
    QString mAssetRoot;
    QString SelectedId_;
    QStringList mSelectedIds;
    QVector<ContentRow> mCatalogRows;
    QVector<QueuedImport> mImportQueue;
    QueuedImport mActiveImport;
    QHash<QString, QueuedImport> mFailedImports;
    /// Import whose failure the problem state shows; its action retries it. Empty for a catalog problem.
    QString mProblemImportId;
    int mBatchTotal = 0;
    int mBatchDone = 0;
    foundation::uint64 Epoch_ = 0;
    foundation::uint64 NextOperation_ = 0;
    foundation::uint64 ActiveOperation_ = 0;
    content::Digest CatalogDigest_;
    bool HasCatalog_ = false;
    bool Applying_ = false;
    bool RefreshPending_ = false;
    bool mHasActiveImport = false;
    bool Closing_ = false;
};
} // namespace ludus::editor
