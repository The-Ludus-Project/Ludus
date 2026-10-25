#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QStringList>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QTableWidget;
class QLabel;
QT_END_NAMESPACE

namespace ludus::editor
{
class RenderGraphDocument;
/// Finite option targets owned by a pipeline; excludes resource interfaces and identities.
[[nodiscard]] QStringList GetRenderGraphOptionTargets(const QJsonObject& node);
/// Enumerate allowed program alternatives on a pipeline for cleanup and stage-change protection.
[[nodiscard]] QStringList GetRenderGraphOptionPrograms(const QJsonObject& node);
/// True when a package option exposes this program identity; such identities cannot be renamed implicitly.
[[nodiscard]] bool HasRenderGraphOptionProgram(const QMap<QString, QByteArray>& files, const QString& program);
/// Option declarations available to this use; subgraphs read their selected configuration.
[[nodiscard]] QJsonArray GetRenderGraphNodeOptions(const RenderGraphDocument& document, const QJsonObject& node);
/// Validate finite typed declarations and overrides. Failure explains the repair; inputs stay unchanged.
[[nodiscard]] bool
ValidateRenderGraphOptions(const QJsonArray& declarations, const QJsonObject& overrides, QString& outError);
/// Copy a pipeline with this use's defaults/overrides applied to its declared scalar/program targets.
/// Failure preserves outPipeline; no library records or source bytes are modified.
[[nodiscard]] bool
ResolveRenderGraphPipelineOptions(const QJsonObject& node, QJsonObject& outPipeline, QString& outError);
/// Validate bindings and caller compatibility throughout an unpublished graph snapshot.
[[nodiscard]] bool ValidateRenderGraphOptionInterfaces(const QMap<QString, QByteArray>& files, QString& outError);

/// Shared option authoring and per-use choices for canvas details and Properties.
/// Completed choices emit overrides; declaration edits remain local until Apply Options.
/// No document mutation or saving occurs here. GUI thread only.
class RenderGraphOptionsEditor final : public QWidget
{
    Q_OBJECT
public:
    explicit RenderGraphOptionsEditor(const QJsonArray& declarations,
                                      const QJsonObject& overrides,
                                      const QStringList& targets,
                                      bool canAuthor,
                                      QWidget* parentOrNull = nullptr);
Q_SIGNALS:
    /// Propose the complete finite public interface after local validation.
    void DeclarationsEdited(const QJsonArray& declarations);
    /// Propose per-use overrides; an absent key means inherit the author default.
    void OverridesEdited(const QJsonObject& overrides);

private:
    void addDeclaration(const QJsonObject& declaration);
    void publishDeclarations();
    QTableWidget* mTableOrNull = nullptr;
    QLabel* mError;
    QStringList mTargets;
    QJsonObject mOverrides;
};
} // namespace ludus::editor
