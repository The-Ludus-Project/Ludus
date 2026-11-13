#include "internal/controller.h"
#include "internal/inspector_fields.h"
#include "internal/inspector_section.h"
#include "internal/workspace_style.h"
#include "internal/world_workspace.h"

#include <ludus/foundation/base/types.h>

#include <QAbstractButton>
#include <QAction>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPoint>
#include <QPushButton>
#include <QRect>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
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
/// Where a Mac capture showed the Colour row keep R and drop G and B.
constexpr int32 COLOUR_WRAP_DOCK_WIDTH = 300;
/// A light range typed in the round-trip test, in metres.
constexpr float64 EDITED_RANGE_METRES = 12;
constexpr int32 WIDE_DOCK_WIDTH = 480;
/// Wide enough for a section's whole help on one line, so narrowing it brings "More" in during a resize.
constexpr int32 WIDE_SECTION_WIDTH = 2000;

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
    const QString name = QStringLiteral("worldAttach:") + component;
    QAction* choiceOrNull = inoutProperties.findChild<QAction*>(name);
    REQUIRE(choiceOrNull != nullptr);
    choiceOrNull->trigger();
    REQUIRE(inoutProperties.findChild<QAction*>(name) == nullptr);
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
        const InspectorLabel* inspectorLabelOrNull = qobject_cast<const InspectorLabel*>(widget);
        const QLabel* labelOrNull = qobject_cast<const QLabel*>(widget);
        if (inspectorLabelOrNull != nullptr && inspectorLabelOrNull->IsElided() == true)
        {
            // A label shortened by the shared column keeps its whole text in the tooltip.
            CHECK(inspectorLabelOrNull->toolTip() == inspectorLabelOrNull->text());
        }
        else if (elidedOrNull != nullptr)
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
        if (qobject_cast<const InspectorSection*>(widget) != nullptr)
        {
            CHECK(widget->width() >= widget->minimumSizeHint().width());
            if (widget->hasHeightForWidth() == true)
            {
                CHECK(widget->height() >= widget->heightForWidth(widget->width()));
            }
        }
    }
}
/// Where a widget sits inside one of its ancestors.
QRect GetRectangleIn(const QWidget& ancestor, const QWidget& widget)
{
    return {widget.mapTo(&ancestor, QPoint(0, 0)), widget.size()};
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
    REQUIRE(dock.findChild<InspectorSection*>(QStringLiteral("worldMaterial")) != nullptr);
    REQUIRE(dock.findChild<QLineEdit*>(QStringLiteral("worldField:scene/position2")) != nullptr);
    REQUIRE(dock.findChild<QLineEdit*>(QStringLiteral("worldField:transform/sz")) != nullptr);
    const QScrollArea* scrollOrNull = dock.findChild<QScrollArea*>(QStringLiteral("worldInspectorScroll"));
    REQUIRE(scrollOrNull != nullptr);
    REQUIRE(scrollOrNull->widget() != nullptr);

    for (const int32 width : {WORLD_PROPERTIES_MINIMUM_WIDTH,
                              NARROW_DOCK_WIDTH,
                              COLOUR_WRAP_DOCK_WIDTH,
                              MEDIUM_DOCK_WIDTH,
                              WIDE_DOCK_WIDTH})
    {
        INFO("Dock width " << width);
        dock.resize(width, DOCK_HEIGHT);
        // The forms wrap at the new width. Let the scroll area process the resulting layout requests
        // before checking the geometry, including the height that the wrapped rows need.
        // Thanks to the Qt contributors for QScrollArea::event's layout-request handling:
        // https://github.com/qt/qtbase/blob/v6.8.3/src/widgets/widgets/qscrollarea.cpp#L252-L267
        CHECK(QTest::qWaitFor(
                  [&dock, panelOrNull, scrollOrNull, width]() {
                      if (dock.width() != width || scrollOrNull->widget()->width() > scrollOrNull->viewport()->width())
                      {
                          return false;
                      }
                      for (const InspectorSection* group : dock.findChildren<InspectorSection*>())
                      {
                          if (group->isVisible() == true && group->hasHeightForWidth() == true &&
                              group->height() < group->heightForWidth(group->width()))
                          {
                              return false;
                          }
                      }
                      // A section decides on "More" after the layout pass that shortened its help, and the button's
                      // row lays it out on the next pass, so wait until every button has its room.
                      for (const QAbstractButton* button : panelOrNull->findChildren<QAbstractButton*>())
                      {
                          if (button->isVisible() == true && IsInternal(*button, *panelOrNull) == false &&
                              button->width() < button->minimumSizeHint().width())
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

    // The default World's Transform reads as one Position, Rotation and Scale line each, typed or scrubbed by its
    // axis letters rather than dragged on nine sliders.
    const auto findField = [&dock](const char* key) {
        QLineEdit* fieldOrNull =
            dock.findChild<QLineEdit*>(QStringLiteral("worldField:transform/") + QString::fromLatin1(key));
        REQUIRE(fieldOrNull != nullptr);
        return fieldOrNull;
    };
    const QRect x = GetRectangleIn(dock, *findField("x"));
    const QRect y = GetRectangleIn(dock, *findField("y"));
    const QRect z = GetRectangleIn(dock, *findField("z"));
    CHECK(y.top() < x.bottom());
    CHECK(z.top() < x.bottom());
    CHECK(x.right() < y.left());
    CHECK(y.right() < z.left());
    CHECK(GetRectangleIn(dock, *findField("rx")).top() >= x.bottom());
    CHECK(GetRectangleIn(dock, *findField("sx")).top() >= GetRectangleIn(dock, *findField("rx")).bottom());
    for (const char* key : {"x", "rx", "sx"})
    {
        CHECK(findField(key)->parentWidget()->findChild<BoundedSlider*>(QString(), Qt::FindDirectChildrenOnly) ==
              nullptr);
    }

    // Apply and Reset appear only with unapplied edits, and say how many fields changed.
    const QWidget* footerOrNull = dock.findChild<QWidget*>(QStringLiteral("worldCommitFooter"));
    const QLabel* summaryOrNull = dock.findChild<QLabel*>(QStringLiteral("worldPendingSummary"));
    REQUIRE(footerOrNull != nullptr);
    REQUIRE(summaryOrNull != nullptr);
    CHECK(footerOrNull->isVisible() == false);
    QLineEdit* scaleOrNull = dock.findChild<QLineEdit*>(QStringLiteral("worldField:transform/sz"));
    REQUIRE(scaleOrNull != nullptr);
    scaleOrNull->selectAll();
    QTest::keyClicks(scaleOrNull, QStringLiteral("2"));
    CHECK(footerOrNull->isVisible() == true);
    // Reset brings back the applied values (including the long red typed above) and hides the footer again.
    QPushButton* resetOrNull = dock.findChild<QPushButton*>(QStringLiteral("worldReset"));
    REQUIRE(resetOrNull != nullptr);
    resetOrNull->click();
    CHECK(footerOrNull->isVisible() == false);
    scaleOrNull = dock.findChild<QLineEdit*>(QStringLiteral("worldField:transform/sz"));
    REQUIRE(scaleOrNull != nullptr);
    scaleOrNull->selectAll();
    QTest::keyClicks(scaleOrNull, QStringLiteral("2"));
    CHECK(summaryOrNull->text() == QStringLiteral("1 unapplied change"));
    QLineEdit* greenOrNull = dock.findChild<QLineEdit*>(QStringLiteral("worldField:renderable/g"));
    REQUIRE(greenOrNull != nullptr);
    greenOrNull->selectAll();
    QTest::keyClicks(greenOrNull, QStringLiteral("0.5"));
    CHECK(summaryOrNull->text() == QStringLiteral("2 unapplied changes"));
    // The edits preview in the viewport first; Reset is available again once that preview has settled.
    REQUIRE(QTest::qWaitFor([resetOrNull]() { return resetOrNull->isEnabled() == true; },
                            SETTLE_TIMEOUT_MILLISECONDS) == true);
    resetOrNull->click();
    CHECK(footerOrNull->isVisible() == false);
    workspace.StopPreview();
}

namespace
{
/// The text of the label beside inoutField in its inspector form, or empty when it has none.
QString GetFormLabelText(QWidget& inoutField)
{
    for (QWidget* rowOrNull = &inoutField; rowOrNull != nullptr; rowOrNull = rowOrNull->parentWidget())
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
    QTest::qWait(0);
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
    // A widget added to a shown layout appears once queued events run.
    REQUIRE(QTest::qWaitFor([kindOrNull, &dock]() { return kindOrNull->isVisibleTo(&dock) == true; },
                            SETTLE_TIMEOUT_MILLISECONDS) == true);
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
    intensityOrNull->selectAll();
    QTest::keyClicks(intensityOrNull, QStringLiteral("43.46527099609375"));
    ApplyChanges(dock, workspace);
    const QJsonValue stored = GetDraftLight(workspace, floor).value(QStringLiteral("intensity"));
    REQUIRE(stored.isDouble() == true);
    intensityOrNull = qobject_cast<QLineEdit*>(find("intensity"));
    REQUIRE(intensityOrNull != nullptr);
    CHECK(intensityOrNull->text() == QStringLiteral("43.46527"));
    CHECK(intensityOrNull->toolTip().contains(FormatStoredNumber(stored.toDouble())) == true);
    QLineEdit* rangeOrNull = qobject_cast<QLineEdit*>(find("range"));
    REQUIRE(rangeOrNull != nullptr);
    rangeOrNull->selectAll();
    QTest::keyClicks(rangeOrNull, FormatInspectorNumber(EDITED_RANGE_METRES));
    ApplyChanges(dock, workspace);
    CHECK(GetDraftLight(workspace, floor).value(QStringLiteral("intensity")) == stored);
    CHECK(GetDraftLight(workspace, floor).value(QStringLiteral("range")).toDouble() == EDITED_RANGE_METRES);
    workspace.StopPreview();
}

TEST_CASE("Properties sections share one label column and stack together when the dock is narrow",
          "[editor][world][inspector][layout]")
{
    QWidget root;
    QVBoxLayout* rootLayout = new QVBoxLayout(&root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    // A dock can be narrower than its content asks for; the column must cope rather than the window grow.
    rootLayout->setSizeConstraint(QLayout::SetNoConstraint);
    InspectorLabelColumn* column = new InspectorLabelColumn(root);
    InspectorSection* transform = new InspectorSection(QStringLiteral("Transform"), QString(), &root);
    QFormLayout* transformRows = new QFormLayout(&transform->GetFields());
    ConfigureInspectorForm(*transformRows);
    QLineEdit* x = new QLineEdit(QStringLiteral("0.25"), &transform->GetFields());
    InspectorLabel& xLabel = AddInspectorRow(*transformRows, QStringLiteral("X"), *x);
    InspectorSection* light = new InspectorSection(QStringLiteral("Light"), QString(), &root);
    QFormLayout* lightRows = new QFormLayout(&light->GetFields());
    ConfigureInspectorForm(*lightRows);
    QLineEdit* angle = new QLineEdit(QStringLiteral("25"), &light->GetFields());
    InspectorLabel& angleLabel =
        AddInspectorRow(*lightRows, QStringLiteral("Outer half angle of the spot cone in degrees"), *angle);
    QCheckBox* enabled = new QCheckBox(&light->GetFields());
    InspectorLabel& enabledLabel = AddInspectorRow(*lightRows, QStringLiteral("Enabled"), *enabled);
    rootLayout->addWidget(transform);
    rootLayout->addWidget(light);
    rootLayout->addStretch();
    root.resize(WIDE_DOCK_WIDTH, DOCK_HEIGHT);
    root.show();
    column->Update();
    QTest::qWait(0);

    // One column: a short label in one section and a long one in another start and end at the same place.
    CHECK(column->IsStacked() == false);
    CHECK(xLabel.width() == column->GetLabelWidth());
    CHECK(angleLabel.width() == column->GetLabelWidth());
    CHECK(xLabel.mapTo(&root, QPoint(0, 0)).x() == angleLabel.mapTo(&root, QPoint(0, 0)).x());
    CHECK(x->mapTo(&root, QPoint(0, 0)).x() == angle->mapTo(&root, QPoint(0, 0)).x());
    // The long label shortens at its end and keeps the whole text in its tooltip.
    CHECK(angleLabel.IsElided() == true);
    CHECK(angleLabel.toolTip() == angleLabel.text());
    // The check box sits in the field column on its label's row, and clicking the label toggles it.
    CHECK(enabledLabel.isVisible() == true);
    CHECK(enabled->text().isEmpty() == true);
    CHECK(enabled->mapTo(&root, QPoint(0, 0)).x() == angle->mapTo(&root, QPoint(0, 0)).x());
    QTest::mouseClick(&enabledLabel, Qt::LeftButton);
    CHECK(enabled->isChecked() == true);

    // Too narrow for a number beside the column: every section puts its labels above its fields at once.
    root.resize(column->GetLabelWidth(), DOCK_HEIGHT);
    column->Update();
    QTest::qWait(0);
    CHECK(column->IsStacked() == true);
    CHECK(transformRows->rowWrapPolicy() == QFormLayout::WrapAllRows);
    CHECK(lightRows->rowWrapPolicy() == QFormLayout::WrapAllRows);
    // A check box never sits alone under its label: it carries the label beside the box instead.
    CHECK(enabledLabel.isHidden() == true);
    CHECK(enabled->text() == QStringLiteral("Enabled"));

    root.resize(WIDE_DOCK_WIDTH, DOCK_HEIGHT);
    column->Update();
    CHECK(column->IsStacked() == false);
    CHECK(enabledLabel.isHidden() == false);
    CHECK(enabled->text().isEmpty() == true);
}

TEST_CASE("A Properties section folds away, keeps that state and offers its actions in one menu",
          "[editor][world][inspector][layout]")
{
    const QString key = QStringLiteral("r9-layout-test-section");
    QSettings().remove(QStringLiteral("inspector/collapsedSections/") + key);
    {
        InspectorSection section(QStringLiteral("Light"), key, nullptr);
        section.SetIcon(EditorIcon::Lighting);
        section.SetHelp(QStringLiteral("Authored photometric light. Point and spot lights use candela; directional "
                                       "lights use lux at the surface they face."));
        QAction& remove = section.AddMenuAction(QStringLiteral("Remove Component"), QStringLiteral("removeIt"));
        QFormLayout* rows = new QFormLayout(&section.GetFields());
        rows->addRow(QStringLiteral("Range"), new QLineEdit(QStringLiteral("10"), &section.GetFields()));
        // Start wide enough for the whole help, then narrow it: "More" appears from a resize, inside a layout pass.
        section.resize(WIDE_SECTION_WIDTH, DOCK_HEIGHT);
        section.show();
        QTest::qWait(0);
        section.resize(NARROW_DOCK_WIDTH, DOCK_HEIGHT);
        QTest::qWait(0);
        CHECK(section.findChild<QAction*>(QStringLiteral("removeIt")) == &remove);
        const QWidget* menuOrNull = section.findChild<QWidget*>(QStringLiteral("inspectorSectionMenu"));
        REQUIRE(menuOrNull != nullptr);
        CHECK(menuOrNull->isVisible() == true);
        // The help is one caption line; "More" shows all of it.
        const InspectorLabel* helpOrNull = section.findChild<InspectorLabel*>(QStringLiteral("inspectorSectionHelp"));
        REQUIRE(helpOrNull != nullptr);
        CHECK(helpOrNull->IsElided() == true);
        QAbstractButton* moreOrNull = section.findChild<QAbstractButton*>(QStringLiteral("inspectorSectionMore"));
        REQUIRE(moreOrNull != nullptr);
        CHECK(moreOrNull->isVisible() == true);
        // The help row lays "More" out in full rather than leaving it zero pixels wide at the row's end.
        CHECK(QTest::qWaitFor([moreOrNull]() { return moreOrNull->width() >= moreOrNull->minimumSizeHint().width(); },
                              SETTLE_TIMEOUT_MILLISECONDS) == true);
        CHECK(moreOrNull->geometry().right() <= moreOrNull->parentWidget()->width() - 1);
        moreOrNull->click();
        QTest::qWait(0);
        CHECK(section.IsHelpExpanded() == true);
        CHECK(helpOrNull->wordWrap() == true);
        // Folding hides the fields and is remembered for the next section with the same key.
        QAbstractButton* toggleOrNull = section.findChild<QAbstractButton*>(QStringLiteral("inspectorSectionToggle"));
        REQUIRE(toggleOrNull != nullptr);
        toggleOrNull->click();
        CHECK(section.IsCollapsed() == true);
        CHECK(section.GetFields().isVisible() == false);
    }
    {
        InspectorSection again(QStringLiteral("Light"), key, nullptr);
        CHECK(again.IsCollapsed() == true);
        again.SetCollapsed(false);
    }
    CHECK(QSettings().contains(QStringLiteral("inspector/collapsedSections/") + key) == false);
}
