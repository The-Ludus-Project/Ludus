#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/content/content.h>

#include <QAbstractTableModel>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPushButton;
class QSortFilterProxyModel;
class QTableView;
QT_END_NAMESPACE

namespace ludus::editor
{
struct ContentRow final
{
    QString Id;
    QString Path;
    content::Kind Kind = content::Kind::AudioSource;
    [[nodiscard]] bool operator==(const ContentRow&) const = default;
};

// UI-thread projection of copied catalog rows. No widget/item allocation per
// row; persisted and selected identities are resource IDs, never row numbers.
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

private:
    QVector<ContentRow> Rows_;
    QHash<QString, int> Ids_;
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
};
struct ContentResult final
{
    ContentRequest Request;
    content::Status Status = content::Status::Invalid;
    QString Message;
    QString CandidatePath;
    QVector<ContentRow> Rows;
    content::Digest CatalogDigest;
};

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
Q_SIGNALS:
    void Completed(const ContentResult& result);
    void BusyChanged();

private:
    void Poll();
    struct Impl;
    Impl* Impl_ = nullptr;
    QTimer PollTimer_;
    bool Closing_ = false;
};

class StateView;

class ContentWorkspace final : public QWidget
{
    Q_OBJECT
public:
    explicit ContentWorkspace(QWidget* parent = nullptr);
    ~ContentWorkspace() override;
    void SetProject(const QString& root, foundation::uint64 epoch);
    void Refresh();
    [[nodiscard]] bool Import(const QString& file, const QString& id);
    void ChooseImport(bool reimport = false);
    void Cancel();
    void Shutdown();
    [[nodiscard]] bool Finished() const noexcept;
    [[nodiscard]] bool Busy() const noexcept;
    [[nodiscard]] bool Importing() const noexcept;
    [[nodiscard]] QString SelectedId() const;
Q_SIGNALS:
    void BusyChanged();
    void CatalogChanged();
    void OpenAudio(const QString& id);

private:
    void RenderActions();
    void showProblem(const ContentResult& result);
    void RestoreSelection();
    void Accept(const ContentResult& result);
    [[nodiscard]] ContentRequest Request() const;
    ContentJobs* Jobs_ = nullptr;
    ContentCatalogModel* Model_ = nullptr;
    QSortFilterProxyModel* Proxy_ = nullptr;
    QTableView* Table_ = nullptr;
    QLineEdit* Search_ = nullptr;
    QLabel* Status_ = nullptr;
    StateView* mProblemOrNull = nullptr;
    QPushButton* Refresh_ = nullptr;
    QPushButton* Import_ = nullptr;
    QPushButton* Reimport_ = nullptr;
    QPushButton* Cancel_ = nullptr;
    QString Root_;
    QString SelectedId_;
    QStringList mSelectedIds;
    foundation::uint64 Epoch_ = 0;
    foundation::uint64 NextOperation_ = 0;
    foundation::uint64 ActiveOperation_ = 0;
    content::Digest CatalogDigest_;
    bool HasCatalog_ = false;
    bool Applying_ = false;
    bool RefreshPending_ = false;
    bool Closing_ = false;
};
} // namespace ludus::editor
