// Built-in creation and canvas settings use the real shared backend and document history.
#include "internal/controller.h"
#include "internal/render_graph_canvas.h"
#include "internal/render_graph_document.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QGraphicsScene>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
constexpr int32 TEMPLATE_JOB_TIMEOUT_MILLISECONDS = 60000;
constexpr int32 TEMPLATE_WORKSPACE_WIDTH = 1280;
constexpr int32 TEMPLATE_WORKSPACE_HEIGHT = 800;
const QString ROOT_GRAPH = QStringLiteral("main.lurender.json");
QString FindTemplatePython()
{
    const QString managed = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/out/host-tools/venv/bin/python");
    return QFileInfo(managed).isExecutable() == true ? managed
                                                     : QStandardPaths::findExecutable(QStringLiteral("python3"));
}
} // namespace

TEST_CASE("ReSTIR template settings are editable from the canvas with Properties hidden",
          "[editor][render-graph][restir-template]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    QWidget properties;
    RendererWorkspace workspace({FindTemplatePython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 1);
    workspace.resize(TEMPLATE_WORKSPACE_WIDTH, TEMPLATE_WORKSPACE_HEIGHT);
    QWidget* inspector = workspace.TakeInspectorPanel(&properties);
    inspector->hide();
    properties.hide();
    workspace.show();
    QSignalSpy finished(&workspace, &RendererWorkspace::JobFinished);
    QTimer::singleShot(0, &workspace, []() {
        QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        QComboBox* templateOrNull = dialogOrNull->findChild<QComboBox*>(QStringLiteral("newRenderGraphTemplate"));
        REQUIRE(templateOrNull != nullptr);
        templateOrNull->setCurrentIndex(templateOrNull->findData(QStringLiteral("restir-di")));
        const QString capture = qEnvironmentVariable("LUDUS_EDITOR_RESTIR_CAPTURE");
        if (capture.isEmpty() == false)
        {
            REQUIRE(dialogOrNull->grab().save(capture + QStringLiteral("-template.png")) == true);
        }
        dialogOrNull->accept();
    });
    REQUIRE(workspace.PromptNewGraph() == true);
    REQUIRE(finished.wait(TEMPLATE_JOB_TIMEOUT_MILLISECONDS) == true);
    INFO(workspace.GetLastResult().Message.toStdString());
    REQUIRE(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("bytes_per_pixel") == true);
    const QString manifest = QStringLiteral("main.lushader.json");
    const QByteArray original = workspace.GetDocument().GetDraft(manifest);
    REQUIRE(original.contains("RESTIR_INITIAL_CANDIDATE_COUNT") == true);
    REQUIRE(workspace.GetCanvas().GetNodeCount() > 0);
    CHECK(inspector->isVisible() == false);
    const auto getCount = [&workspace, &manifest]() {
        return QJsonDocument::fromJson(workspace.GetDocument().GetDraft(manifest))
            .object()
            .value(QStringLiteral("settings"))
            .toArray()[0]
            .toObject()
            .value(QStringLiteral("value"))
            .toInt();
    };
    CHECK(getCount() == 8);
    bool hasOpenedSettings = false;
    QTimer::singleShot(0, &workspace, [&workspace, &hasOpenedSettings]() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QAction* settingsOrNull = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphEditSettingsAction"));
        REQUIRE(settingsOrNull != nullptr);
        REQUIRE(settingsOrNull->isEnabled() == true);
        menuOrNull->close();
        QTimer::singleShot(0, &workspace, [&hasOpenedSettings]() {
            QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            REQUIRE(dialogOrNull != nullptr);
            QSpinBox* candidatesOrNull =
                dialogOrNull->findChild<QSpinBox*>(QStringLiteral("renderGraphSetting_RESTIR_INITIAL_CANDIDATE_COUNT"));
            REQUIRE(candidatesOrNull != nullptr);
            candidatesOrNull->setFocus();
            candidatesOrNull->selectAll();
            QTest::keyClicks(candidatesOrNull, QStringLiteral("12"));
            QSignalSpy committed(candidatesOrNull, &QSpinBox::editingFinished);
            QTest::keyClick(candidatesOrNull, Qt::Key_Return);
            CHECK(committed.count() > 0);
            hasOpenedSettings = true;
            const QString capture = qEnvironmentVariable("LUDUS_EDITOR_RESTIR_CAPTURE");
            if (capture.isEmpty() == false)
            {
                REQUIRE(dialogOrNull->grab().save(capture + QStringLiteral("-settings.png")) == true);
            }
            dialogOrNull->reject();
        });
        settingsOrNull->trigger();
    });
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const QPointF background = canvas.scene()->itemsBoundingRect().bottomRight() + QPointF(100, 100);
    canvas.centerOn(background);
    const QPoint position = canvas.mapFromScene(background);
    QContextMenuEvent context(QContextMenuEvent::Mouse, position, canvas.viewport()->mapToGlobal(position));
    QCoreApplication::sendEvent(canvas.viewport(), &context);
    REQUIRE(hasOpenedSettings == true);
    CHECK(getCount() == 12);
    CHECK(workspace.GetDocument().GetSaved(manifest) == original);
    REQUIRE(workspace.CanUndo() == true);
    workspace.Undo();
    CHECK(getCount() == 8);
    workspace.Redo();
    CHECK(getCount() == 12);
    REQUIRE(workspace.Save() == true);
    REQUIRE(workspace.Validate() == true);
    REQUIRE(finished.wait(TEMPLATE_JOB_TIMEOUT_MILLISECONDS) == true);
    INFO(workspace.GetLastResult().Message.toStdString());
    CHECK(workspace.GetLastResult().IsSuccessful == true);
    const QString path = QDir(project.path()).filePath(QStringLiteral("graphs/Main/main.lurender.json"));
    RenderGraphDocument reloaded;
    REQUIRE(reloaded.Load(QFileInfo(path).absolutePath(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(reloaded.GetDraft(manifest) == workspace.GetDocument().GetSaved(manifest));
}

TEST_CASE("Visibility templates are created from the editor picker with Properties hidden",
          "[editor][render-graph][visibility-template]")
{
    QString identity;
    SECTION("visibility buffer and depth")
    {
        identity = QStringLiteral("visibility-buffer");
    }
    SECTION("surface reconstruction and secondary rays")
    {
        identity = QStringLiteral("visibility-secondary-rays");
    }
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    QWidget properties;
    RendererWorkspace workspace({FindTemplatePython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 1);
    QWidget* inspector = workspace.TakeInspectorPanel(&properties);
    inspector->hide();
    properties.hide();
    workspace.show();
    QSignalSpy finished(&workspace, &RendererWorkspace::JobFinished);
    QTimer::singleShot(0, &workspace, [&identity]() {
        QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        QComboBox* templateOrNull = dialogOrNull->findChild<QComboBox*>(QStringLiteral("newRenderGraphTemplate"));
        REQUIRE(templateOrNull != nullptr);
        const int32 selected = templateOrNull->findData(identity);
        REQUIRE(selected >= 0);
        templateOrNull->setCurrentIndex(selected);
        CHECK(templateOrNull->currentData(Qt::ToolTipRole).toString().isEmpty() == false);
        dialogOrNull->accept();
    });
    REQUIRE(workspace.PromptNewGraph() == true);
    REQUIRE(finished.wait(TEMPLATE_JOB_TIMEOUT_MILLISECONDS) == true);
    INFO(workspace.GetLastResult().Message.toStdString());
    REQUIRE(workspace.GetCanvas().GetNodeCount() > 0);
    CHECK(inspector->isVisible() == false);
    const QByteArray manifest = workspace.GetDocument().GetDraft(QStringLiteral("main.lushader.json"));
    CHECK(manifest.contains("Flat") == true);
    const bool hasSecondaryRays = identity == QStringLiteral("visibility-secondary-rays");
    CHECK(manifest.contains("visibility_buffer") == hasSecondaryRays);
    const QByteArray graph = workspace.GetDocument().GetDraft(ROOT_GRAPH);
    CHECK(graph.contains("GraphicsPipeline") == true);
    CHECK(graph.contains("RayTracingPipeline") == hasSecondaryRays);
    CHECK(graph.contains("visibilityOutput") == true);
    CHECK(graph.contains("depthOutput") == true);
    REQUIRE(workspace.Validate() == true);
    REQUIRE(finished.wait(TEMPLATE_JOB_TIMEOUT_MILLISECONDS) == true);
    INFO(workspace.GetLastResult().Message.toStdString());
    CHECK(workspace.GetLastResult().IsSuccessful == true);
}
