#pragma once

#include <ludus/foundation/base/types.h>

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QStringList>

QT_BEGIN_NAMESPACE
class QLabel;
class QTableWidget;
QT_END_NAMESPACE

namespace ludus::editor
{
class RenderGraphDocument;
/// Configuration interface admission bound, matching the graph source contract.
inline constexpr foundation::int32 MAX_CONFIGURATION_PORTS = 8;
/// Allowed interface types for the authored graph version and direction.
[[nodiscard]] QStringList GetConfigurationPortTypes(foundation::int32 version, bool isOutput);

/// Modal editor for one configuration interface. Controls are local until ApplyRequested;
/// the workspace accepts a typed document edit before closing. GUI thread only.
class RenderGraphConfigurationEditor final : public QDialog
{
    Q_OBJECT
public:
    explicit RenderGraphConfigurationEditor(const RenderGraphDocument& document,
                                            const QString& file,
                                            const QJsonObject& configuration,
                                            QWidget* parentOrNull = nullptr);
    /// Return candidate input records; does not change the document.
    [[nodiscard]] QJsonArray GetInputs() const;
    /// Return candidate output records, including sources and required-consumer flags.
    /// Input sources retain original row identities for the document's rename transaction.
    [[nodiscard]] QJsonArray GetOutputs() const;
    /// Explicit old-to-new identities retained by rows; removal/addition is never guessed as a rename.
    [[nodiscard]] QMap<QString, QString> GetInputRenames() const;
    /// Return old-to-new output identities; new rows and removed rows contribute no rename.
    [[nodiscard]] QMap<QString, QString> GetOutputRenames() const;
    /// Keep the dialog open and display a rejected edit's reason beside its controls.
    void ShowError(const QString& message);

Q_SIGNALS:
    void ApplyRequested();

private:
    [[nodiscard]] QTableWidget* createTable(bool isOutput);
    void
    addPort(QTableWidget& inoutTable, const QJsonObject& port, bool isOutput, const QString& originalIdentity = {});
    void refreshSources();
    const RenderGraphDocument& mDocument;
    QString mFile;
    QJsonObject mConfiguration;
    foundation::int32 mVersion;
    QTableWidget* mInputs;
    QTableWidget* mOutputs;
    QLabel* mError;
    QLabel* mRepairs;
};
} // namespace ludus::editor
