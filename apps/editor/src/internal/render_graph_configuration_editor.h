#pragma once

#include <ludus/foundation/base/types.h>

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
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
    [[nodiscard]] QJsonArray GetOutputs() const;
    /// Keep the dialog open and display a rejected edit's reason beside its controls.
    void ShowError(const QString& message);

Q_SIGNALS:
    void ApplyRequested();

private:
    [[nodiscard]] QTableWidget* createTable(bool isOutput);
    void addPort(QTableWidget& inoutTable, const QJsonObject& port, bool isOutput);
    void refreshSources();
    const RenderGraphDocument& mDocument;
    QJsonObject mConfiguration;
    foundation::int32 mVersion;
    QTableWidget* mInputs;
    QTableWidget* mOutputs;
    QLabel* mError;
};
} // namespace ludus::editor
