#include "internal/controller.h"
#include "internal/inspector_fields.h"
#include "internal/workspace_style.h"
#include "internal/world_workspace.h"

#include <ludus/foundation/base/types.h>

#include <QAbstractButton>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::editor;
using namespace ludus::foundation;

constexpr int32 SETTLE_TIMEOUT_MILLISECONDS = 10000;
constexpr int32 TEXTURE_SIZE = 4;
/// Tall enough that every section is laid out on screen, so the screenshots show the whole inspector.
constexpr int32 DOCK_HEIGHT = 2000;
/// Dock widths from the narrowest the editor allows to a roomy one.
constexpr int32 NARROW_DOCK_WIDTH = 260;
constexpr int32 MEDIUM_DOCK_WIDTH = 320;
/// A light range typed in the round-trip test, in metres.
constexpr float64 EDITED_RANGE_METRES = 12;
constexpr int32 WIDE_DOCK_WIDTH = 480;

void WriteFixtureFile(const QString& path, const QByteArray& bytes)
{
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()) == true);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
}

/// A textured floor lit by one point light, so Properties shows a scene instance with a base colour map.
void WriteTexturedSource(const QString& path)
{
    WriteFixtureFile(path,
                     QByteArrayLiteral("LookAt 0 2 5  0 0 0  0 1 0\n"
                                       "Camera \"perspective\" \"float fov\" [45]\nWorldBegin\n"
                                       "LightSource \"point\" \"rgb I\" [3 3 3] \"point3 from\" [0 2 0]\n"
                                       "Texture \"checks\" \"spectrum\" \"imagemap\" \"string filename\" "
                                       "\"textures/checks.png\"\n"
                                       "Material \"diffuse\" \"texture reflectance\" \"checks\"\n"
                                       "Shape \"trianglemesh\" \"point3 P\" [-3 0 -3  3 0 -3  3 0 3  -3 0 3]\n"
                                       "\"point2 uv\" [0 0  1 0  1 1  0 1] \"integer indices\" [0 1 2  0 2 3]\n"));
    QImage texture(TEXTURE_SIZE, TEXTURE_SIZE, QImage::Format_RGB32);
    texture.fill(QColor(Qt::darkCyan));
    const QString texturePath = QDir(QFileInfo(path).absolutePath()).filePath(QStringLiteral("textures/checks.png"));
    REQUIRE(QDir().mkpath(QFileInfo(texturePath).absolutePath()) == true);
    REQUIRE(texture.save(texturePath) == true);
    WriteFixtureFile(QDir(QFileInfo(path).absolutePath()).filePath(QStringLiteral("LICENSE.txt")),
                     QByteArrayLiteral("CC0 synthetic fixture\n"));
}

void WaitForWorld(WorldWorkspace& inoutWorkspace)
{
    REQUIRE(QTest::qWaitFor([&inoutWorkspace]() { return inoutWorkspace.CanSave() == true; },
                            SETTLE_TIMEOUT_MILLISECONDS) == true);
}

ToolingPaths MakeSceneTools()
{
    ToolingPaths tools;
    tools.ToolingRoot = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    return tools;
}

void CreateImportedWorld(const QString& root, const QString& path, const QString& source)
{
    WorldWorkspace workspace(MakeSceneTools(), nullptr);
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(path) == true);
    REQUIRE(workspace.ImportScene(source) == true);
    WaitForWorld(workspace);
    REQUIRE(workspace.Save() == true);
    WaitForWorld(workspace);
    workspace.StopPreview();
}

void AttachComponent(QWidget& inoutProperties, const QString& component)
{
    INFO(component.toStdString());
    QPushButton* attachOrNull = inoutProperties.findChild<QPushButton*>(QStringLiteral("worldAttach"));
    REQUIRE(attachOrNull != nullptr);
    REQUIRE(QTest::qWaitFor([attachOrNull]() { return attachOrNull->isEnabled() == true; },
                            SETTLE_TIMEOUT_MILLISECONDS) == true);
    QComboBox* choicesOrNull = inoutProperties.findChild<QComboBox*>(QStringLiteral("worldComponentChoice"));
    REQUIRE(choicesOrNull != nullptr);
    const int32 index = choicesOrNull->findData(component);
    REQUIRE(index >= 0);
    choicesOrNull->setCurrentIndex(index);
    attachOrNull->click();
    REQUIRE(choicesOrNull->findData(component) < 0);
}

QString Describe(const QWidget& widget)
{
    QString name = QString::fromLatin1(widget.metaObject()->className()) + QStringLiteral(" ") + widget.objectName();
    const QLabel* labelOrNull = qobject_cast<const QLabel*>(&widget);
    if (labelOrNull != nullptr)
    {
        name += QStringLiteral(" \"") + labelOrNull->text() + QStringLiteral("\"");
    }
    const QAbstractButton* buttonOrNull = qobject_cast<const QAbstractButton*>(&widget);
    if (buttonOrNull != nullptr)
    {
        name += QStringLiteral(" \"") + buttonOrNull->text() + QStringLiteral("\"");
    }
    const QLineEdit* fieldOrNull = qobject_cast<const QLineEdit*>(&widget);
    if (fieldOrNull != nullptr)
    {
        name += QStringLiteral(" \"") + fieldOrNull->text() + QStringLiteral("\"");
    }
    return name;
}

/// Whether widget sits inside a scroll bar or a combo box's own popup machinery, which lay themselves out.
bool IsInternal(const QWidget& widget, const QWidget& root)
{
    for (const QWidget* ancestorOrNull = &widget; ancestorOrNull != nullptr && ancestorOrNull != &root;
         ancestorOrNull = ancestorOrNull->parentWidget())
    {
        if (qobject_cast<const QScrollBar*>(ancestorOrNull) != nullptr ||
            (ancestorOrNull != &widget && qobject_cast<const QComboBox*>(ancestorOrNull) != nullptr))
        {
            return true;
        }
    }
    return false;
}

/// Every visible widget in the Properties panel lies within its parent horizontally and has the room its text
/// needs: labels show their whole text (or wrap to their full height), number fields show their whole value,
/// buttons and choices have their natural width. Only ElidedLabel may shorten its text, and then it carries the
/// full text as its tooltip.
void CheckNothingClipped(const QWidget& panel)
{
    const QScrollArea* scrollOrNull = panel.findChild<QScrollArea*>(QStringLiteral("worldInspectorScroll"));
    REQUIRE(scrollOrNull != nullptr);
    REQUIRE(scrollOrNull->widget() != nullptr);
    // The inspector never needs a horizontal scroll bar: its content follows the dock's width.
    CHECK(scrollOrNull->widget()->width() <= scrollOrNull->viewport()->width());
    for (const QWidget* widget : panel.findChildren<QWidget*>())
    {
        if (widget->isVisible() == false || IsInternal(*widget, panel) == true || widget->isWindow() == true)
        {
            continue;
        }
        INFO(Describe(*widget).toStdString());
        const QWidget* parentOrNull = widget->parentWidget();
        if (parentOrNull != nullptr && parentOrNull != scrollOrNull->viewport())
        {
            CHECK(widget->geometry().left() >= 0);
            CHECK(widget->geometry().right() <= parentOrNull->width() - 1);
        }
        const QRect onPanel(widget->mapTo(&panel, QPoint(0, 0)), widget->size());
        CHECK(onPanel.left() >= 0);
        CHECK(onPanel.right() <= panel.width() - 1);
        const ElidedLabel* elidedOrNull = qobject_cast<const ElidedLabel*>(widget);
        const QLabel* labelOrNull = qobject_cast<const QLabel*>(widget);
        if (elidedOrNull != nullptr)
        {
            if (elidedOrNull->IsElided() == true)
            {
                // The tooltip holds the whole reference; the label may already show a shortened form of it.
                const QString head = elidedOrNull->text().section(QStringLiteral("…"), 0, 0);
                CHECK(elidedOrNull->toolTip().isEmpty() == false);
                CHECK(elidedOrNull->toolTip().startsWith(head) == true);
            }
        }
        else if (labelOrNull != nullptr && labelOrNull->text().isEmpty() == false)
        {
            if (labelOrNull->wordWrap() == true)
            {
                CHECK(labelOrNull->heightForWidth(labelOrNull->width()) <= labelOrNull->height());
            }
            else
            {
                CHECK(labelOrNull->sizeHint().width() <= labelOrNull->width());
            }
        }
        const QLineEdit* fieldOrNull = qobject_cast<const QLineEdit*>(widget);
        bool isNumber = false;
        if (fieldOrNull != nullptr)
        {
            static_cast<void>(fieldOrNull->text().toDouble(&isNumber));
        }
        if (isNumber == true)
        {
            CHECK(fieldOrNull->width() >= ComputeLineEditWidth(*fieldOrNull, fieldOrNull->text()));
        }
        if (qobject_cast<const QAbstractButton*>(widget) != nullptr ||
            qobject_cast<const QComboBox*>(widget) != nullptr)
        {
            CHECK(widget->width() >= widget->minimumSizeHint().width());
        }
        if (qobject_cast<const QPushButton*>(widget) != nullptr)
        {
            CHECK(widget->width() >= widget->sizeHint().width());
        }
        if (qobject_cast<const QGroupBox*>(widget) != nullptr)
        {
            CHECK(widget->width() >= widget->minimumSizeHint().width());
            if (widget->hasHeightForWidth() == true)
            {
                CHECK(widget->height() >= widget->heightForWidth(widget->width()));
            }
        }
    }
}
} // namespace

TEST_CASE("Properties keeps every field, label and value whole from the narrowest dock width",
          "[editor][world][inspector][layout]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString projectPath = QDir(directory.path()).filePath(QStringLiteral("project"));
    REQUIRE(QDir().mkpath(projectPath) == true);
    const QString root = QFileInfo(projectPath).canonicalFilePath();
    const QString source = directory.filePath(QStringLiteral("source/textured.pbrt"));
    WriteTexturedSource(source);
    const QString path = QDir(root).filePath(QStringLiteral("textured.world.json"));
    CreateImportedWorld(root, path, source);

    WorldWorkspace workspace(MakeSceneTools(), nullptr);
    workspace.SetProject(root, 1);
    // The shell hosts the inspector in its Properties tool window; this widget stands in for that dock.
    QWidget dock;
    dock.setObjectName(QStringLiteral("propertiesDockStandIn"));
    QVBoxLayout* dockLayout = new QVBoxLayout(&dock);
    dockLayout->setContentsMargins(0, 0, 0, 0);
    QWidget* panelOrNull = workspace.TakePropertiesPanel(&dock);
    REQUIRE(panelOrNull != nullptr);
    dockLayout->addWidget(panelOrNull);
    ApplyWorkspaceStyle(&dock);
    REQUIRE(workspace.Open(path) == true);
    WaitForWorld(workspace);

    QString floor;
    for (const QJsonValue value : workspace.Document().Draft().value(QStringLiteral("entities")).toArray())
    {
        const QJsonObject entity = value.toObject();
        if (entity.value(QStringLiteral("scene")).isObject() == true && floor.isEmpty() == true)
        {
            floor = workspace.Document().GetEntityKey(entity);
        }
    }
    REQUIRE(floor.isEmpty() == false);
    REQUIRE(workspace.SelectEntity(floor) == true);
    dock.resize(WIDE_DOCK_WIDTH, DOCK_HEIGHT);
    dock.show();
    // Every section the Mac captures showed clipping: the scene instance, its material, Renderable and Transform.
    AttachComponent(dock, QStringLiteral("renderable"));
    AttachComponent(dock, QStringLiteral("transform"));
    WaitForWorld(workspace);
    REQUIRE(QTest::qWaitFor(
                [&dock]() {
                    const QWidget* thumbnailOrNull =
                        dock.findChild<QWidget*>(QStringLiteral("worldMaterialThumbnail:Base colour map"));
                    return thumbnailOrNull != nullptr && thumbnailOrNull->isVisible() == true;
                },
                SETTLE_TIMEOUT_MILLISECONDS) == true);
    // A long colour value, as the staircase's Renderable showed it ("0.818579" read as "818579").
    QLineEdit* redOrNull = dock.findChild<QLineEdit*>(QStringLiteral("worldField:renderable/r"));
    REQUIRE(redOrNull != nullptr);
    redOrNull->setText(QStringLiteral("0.818579"));
    REQUIRE(dock.findChild<QGroupBox*>(QStringLiteral("worldMaterial")) != nullptr);
    REQUIRE(dock.findChild<QLineEdit*>(QStringLiteral("worldField:scene/position2")) != nullptr);
    REQUIRE(dock.findChild<QLineEdit*>(QStringLiteral("worldField:transform/sz")) != nullptr);
    const QScrollArea* scrollOrNull = dock.findChild<QScrollArea*>(QStringLiteral("worldInspectorScroll"));
    REQUIRE(scrollOrNull != nullptr);
    REQUIRE(scrollOrNull->widget() != nullptr);

    for (const int32 width : {WORLD_PROPERTIES_MINIMUM_WIDTH, NARROW_DOCK_WIDTH, MEDIUM_DOCK_WIDTH, WIDE_DOCK_WIDTH})
    {
        INFO("Dock width " << width);
        dock.resize(width, DOCK_HEIGHT);
        // The forms wrap at the new width. Let the scroll area process the resulting layout requests
        // before checking the geometry, including the height that the wrapped rows need.
        // Thanks to the Qt contributors for QScrollArea::event's layout-request handling:
        // https://github.com/qt/qtbase/blob/v6.8.3/src/widgets/widgets/qscrollarea.cpp#L252-L267
        CHECK(QTest::qWaitFor(
                  [&dock, scrollOrNull, width]() {
                      if (dock.width() != width || scrollOrNull->widget()->width() > scrollOrNull->viewport()->width())
                      {
                          return false;
                      }
                      for (const QGroupBox* group : dock.findChildren<QGroupBox*>())
                      {
                          if (group->isVisible() == true && group->hasHeightForWidth() == true &&
                              group->height() < group->heightForWidth(group->width()))
                          {
                              return false;
                          }
                      }
                      return true;
                  },
                  SETTLE_TIMEOUT_MILLISECONDS) == true);
        CHECK(dock.width() == width);
        CheckNothingClipped(*panelOrNull);
        if (qEnvironmentVariableIsSet("LUDUS_PROPERTIES_SCREENSHOT_DIRECTORY") == true)
        {
            const QDir shots(qEnvironmentVariable("LUDUS_PROPERTIES_SCREENSHOT_DIRECTORY"));
            // The inspector's whole content at this width, then the dock as it shows.
            REQUIRE(scrollOrNull->widget()->grab().save(
                        shots.filePath(QStringLiteral("properties-%1.png").arg(width))) == true);
        }
    }
    workspace.StopPreview();
}

namespace
{
/// The text of the label beside field in its inspector form, or empty when it has none.
QString GetFormLabelText(QWidget& field)
{
    for (QWidget* rowOrNull = &field; rowOrNull != nullptr; rowOrNull = rowOrNull->parentWidget())
    {
        QWidget* parentOrNull = rowOrNull->parentWidget();
        QFormLayout* formOrNull =
            parentOrNull == nullptr ? nullptr : qobject_cast<QFormLayout*>(parentOrNull->layout());
        if (formOrNull != nullptr && formOrNull->indexOf(rowOrNull) >= 0)
        {
            const QLabel* labelOrNull = qobject_cast<QLabel*>(formOrNull->labelForField(rowOrNull));
            return labelOrNull == nullptr ? QString() : labelOrNull->text();
        }
    }
    return {};
}

/// The light component of entity as the World draft holds it.
QJsonObject GetDraftLight(const WorldWorkspace& workspace, const QString& entity)
{
    return workspace.Document()
        .Entity(entity)
        .value(QStringLiteral("components"))
        .toObject()
        .value(QStringLiteral("light"))
        .toObject();
}

/// Clicks Apply Changes once the World is ready for it.
void ApplyChanges(QWidget& inoutProperties, WorldWorkspace& inoutWorkspace)
{
    QPushButton* applyOrNull = inoutProperties.findChild<QPushButton*>(QStringLiteral("worldApply"));
    REQUIRE(applyOrNull != nullptr);
    REQUIRE(QTest::qWaitFor([applyOrNull]() { return applyOrNull->isEnabled() == true; },
                            SETTLE_TIMEOUT_MILLISECONDS) == true);
    applyOrNull->click();
    WaitForWorld(inoutWorkspace);
}
} // namespace

TEST_CASE("Light fields follow the light's kind, and an untouched number keeps every stored digit",
          "[editor][world][inspector]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString projectPath = QDir(directory.path()).filePath(QStringLiteral("project"));
    REQUIRE(QDir().mkpath(projectPath) == true);
    const QString root = QFileInfo(projectPath).canonicalFilePath();
    const QString source = directory.filePath(QStringLiteral("source/textured.pbrt"));
    WriteTexturedSource(source);
    const QString path = QDir(root).filePath(QStringLiteral("textured.world.json"));
    CreateImportedWorld(root, path, source);

    WorldWorkspace workspace(MakeSceneTools(), nullptr);
    workspace.SetProject(root, 1);
    QWidget dock;
    QVBoxLayout* dockLayout = new QVBoxLayout(&dock);
    QWidget* panelOrNull = workspace.TakePropertiesPanel(&dock);
    REQUIRE(panelOrNull != nullptr);
    dockLayout->addWidget(panelOrNull);
    REQUIRE(workspace.Open(path) == true);
    WaitForWorld(workspace);
    QString floor;
    for (const QJsonValue value : workspace.Document().Draft().value(QStringLiteral("entities")).toArray())
    {
        if (value.toObject().value(QStringLiteral("scene")).isObject() == true && floor.isEmpty() == true)
        {
            floor = workspace.Document().GetEntityKey(value.toObject());
        }
    }
    REQUIRE(floor.isEmpty() == false);
    REQUIRE(workspace.SelectEntity(floor) == true);
    dock.resize(WIDE_DOCK_WIDTH, DOCK_HEIGHT);
    dock.show();
    AttachComponent(dock, QStringLiteral("light"));
    WaitForWorld(workspace);

    // A directional light has no range or cone, and its intensity reads in lux; a spot light shows both in candela.
    const auto find = [&dock](const char* key) {
        QWidget* fieldOrNull = dock.findChild<QWidget*>(QStringLiteral("worldField:light/") + QString::fromLatin1(key));
        REQUIRE(fieldOrNull != nullptr);
        return fieldOrNull;
    };
    QComboBox* kindOrNull = qobject_cast<QComboBox*>(find("kind"));
    REQUIRE(kindOrNull != nullptr);
    kindOrNull->setCurrentIndex(kindOrNull->findData(QStringLiteral("directional")));
    CHECK(find("range")->isVisibleTo(&dock) == false);
    CHECK(find("innerAngle")->isVisibleTo(&dock) == false);
    CHECK(find("outerAngle")->isVisibleTo(&dock) == false);
    CHECK(GetFormLabelText(*find("intensity")) == QStringLiteral("Intensity (lx)"));
    kindOrNull->setCurrentIndex(kindOrNull->findData(QStringLiteral("spot")));
    CHECK(find("range")->isVisibleTo(&dock) == true);
    CHECK(find("outerAngle")->isVisibleTo(&dock) == true);
    CHECK(GetFormLabelText(*find("intensity")) == QStringLiteral("Intensity (cd)"));
    kindOrNull->setCurrentIndex(kindOrNull->findData(QStringLiteral("point")));
    CHECK(find("range")->isVisibleTo(&dock) == true);
    CHECK(find("outerAngle")->isVisibleTo(&dock) == false);

    // A float32 intensity shows its short text, keeps every digit in the tooltip, and an unrelated Apply leaves
    // the stored number exactly as it was.
    QLineEdit* intensityOrNull = qobject_cast<QLineEdit*>(find("intensity"));
    REQUIRE(intensityOrNull != nullptr);
    intensityOrNull->setText(QStringLiteral("43.46527099609375"));
    ApplyChanges(dock, workspace);
    const QJsonValue stored = GetDraftLight(workspace, floor).value(QStringLiteral("intensity"));
    REQUIRE(stored.isDouble() == true);
    intensityOrNull = qobject_cast<QLineEdit*>(find("intensity"));
    REQUIRE(intensityOrNull != nullptr);
    CHECK(intensityOrNull->text() == QStringLiteral("43.46527"));
    CHECK(intensityOrNull->toolTip().contains(FormatStoredNumber(stored.toDouble())) == true);
    QLineEdit* rangeOrNull = qobject_cast<QLineEdit*>(find("range"));
    REQUIRE(rangeOrNull != nullptr);
    rangeOrNull->setText(FormatInspectorNumber(EDITED_RANGE_METRES));
    ApplyChanges(dock, workspace);
    CHECK(GetDraftLight(workspace, floor).value(QStringLiteral("intensity")) == stored);
    CHECK(GetDraftLight(workspace, floor).value(QStringLiteral("range")).toDouble() == EDITED_RANGE_METRES);
    workspace.StopPreview();
}
