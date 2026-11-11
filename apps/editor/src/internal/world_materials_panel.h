#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QEvent;
class QJsonObject;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
QT_END_NAMESPACE

namespace ludus::editor
{
class WorldDocument;
struct RenderScene;

struct WorldMaterialUser final
{
    QString EntityId;
    QString Name;

    [[nodiscard]] bool operator==(const WorldMaterialUser&) const = default;
};

struct WorldMaterialRow final
{
    QString ResourceId;
    QString Name;
    QVector<WorldMaterialUser> Users;

    [[nodiscard]] bool operator==(const WorldMaterialRow&) const = default;
};

/// Group imported renderables in the applied World by effective resource ID. Names are borrowed
/// from the decoded viewport only when its level associates the renderable with that same resource.
[[nodiscard]] QVector<WorldMaterialRow> CollectWorldMaterialRows(const WorldDocument& world,
                                                                 const QJsonObject& presentedLevel,
                                                                 const RenderScene& presentedScene);

/// The World sidebar's resource list. Expanding a resource reveals its renderables.
class WorldMaterialsPanel final : public QWidget
{
    Q_OBJECT
public:
    explicit WorldMaterialsPanel(QWidget* parent);
    void SetRows(const QVector<WorldMaterialRow>& rows);
    void RevealResource(const QString& id);
    void ShowMissingRenderable();

Q_SIGNALS:
    void RenderableActivated(const QString& entityId);

private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    [[nodiscard]] bool tryActivateRenderable(const QTreeWidgetItem* itemOrNull);
    void updateActions();

    QTreeWidget* mTree;
    QPushButton* mCopy;
    QLabel* mStatus;
    QVector<WorldMaterialRow> mRows;
};
} // namespace ludus::editor
