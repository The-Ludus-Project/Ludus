#include "internal/inspector_fields.h"
#include "internal/world_workspace.h"

#include <ludus/foundation/base/types.h>

#include <cmath>
#include <limits>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
using ludus::foundation::float64;
using ludus::foundation::int32;

namespace
{
constexpr float64 DEGREE_SCRUB_STEP = 1.0;
constexpr float64 COLOUR_SCRUB_STEP = 0.005;
constexpr float64 METRE_SCRUB_STEP = 0.01;
constexpr float64 WIDE_RANGE_MINIMUM = 0.0;
constexpr float64 WIDE_RANGE_MAXIMUM = 500.0;
constexpr float64 WIDE_RANGE_STEP = 0.5;
constexpr int32 SCRUB_DISTANCE = 10;
constexpr int32 SCRUB_PAST_LIMIT = 100000;
constexpr float64 SCRUBBED_TEN_PIXELS = 0.1;
constexpr float64 SCRUBBED_START = 1.0;
constexpr int GRAB_X = 4;
constexpr int GRAB_Y = 4;
constexpr int WORKSPACE_WIDTH = 1100;
constexpr int WORKSPACE_HEIGHT = 800;
constexpr int DISPLAY_GREY = 128;
constexpr float64 GREY_LINEAR = 0.2158605;
constexpr int SWATCH_TOLERANCE = 1;
constexpr int SETTLE_MILLISECONDS = 50;
const QString LONG_IDENTIFIER = QStringLiteral("0b2d8a4c-77f1-4a0e-9c35-6e1f2a3b4c5d");

QByteArray Read(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}
void Write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(bytes) == bytes.size());
}
QString MakeStarterProject(const QTemporaryDir& directory)
{
    REQUIRE(directory.isValid());
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    REQUIRE(root.mkdir(QStringLiteral("authoring")));
    Write(root.filePath(QStringLiteral("authoring/world.schema.json")),
          Read(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/examples/live-edit-game/authoring/world.schema.json")));
    const QString path = root.filePath(QStringLiteral("authoring/starter.world.json"));
    Write(path,
          Read(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/examples/live-edit-game/authoring/starter.world.json")));
    return path;
}
QRect GetRectangleIn(const QWidget& ancestor, const QWidget& widget)
{
    return {widget.mapTo(&ancestor, QPoint(0, 0)), widget.size()};
}
} // namespace

TEST_CASE("Scrubbing a label changes the number by a step per pixel", "[editor][inspector]")
{
    const ScrubSettings settings{ .Minimum = -10.0, .Maximum = 10.0, .Step = 0.01 };
    CHECK(ComputeScrubbedValue(SCRUBBED_START, SCRUB_DISTANCE, settings, Qt::NoModifier) ==
          Catch::Approx(SCRUBBED_START + SCRUBBED_TEN_PIXELS));
    CHECK(ComputeScrubbedValue(SCRUBBED_START, -SCRUB_DISTANCE, settings, Qt::NoModifier) ==
          Catch::Approx(SCRUBBED_START - SCRUBBED_TEN_PIXELS));
    // Shift is fine and Control is coarse, in the same direction as the plain drag.
    const float64 plain = ComputeScrubbedValue(0.0, SCRUB_DISTANCE, settings, Qt::NoModifier);
    const float64 fine = ComputeScrubbedValue(0.0, SCRUB_DISTANCE, settings, Qt::ShiftModifier);
    const float64 coarse = ComputeScrubbedValue(0.0, SCRUB_DISTANCE, settings, Qt::ControlModifier);
    CHECK(fine > 0.0);
    CHECK(fine < plain);
    CHECK(coarse > plain);
    // The range holds, and the result carries no floating-point residue.
    CHECK(ComputeScrubbedValue(0.0, SCRUB_PAST_LIMIT, settings, Qt::NoModifier) == Catch::Approx(settings.Maximum));
    CHECK(ComputeScrubbedValue(0.0, -SCRUB_PAST_LIMIT, settings, Qt::NoModifier) == Catch::Approx(settings.Minimum));
    CHECK(ComputeScrubbedValue(0.1, 20, settings, Qt::NoModifier) == 0.3);
}

TEST_CASE("Scrub steps follow the unit and the range of a field", "[editor][inspector]")
{
    CHECK(GetScrubStep(QStringLiteral("°"), -360.0, 360.0) == Catch::Approx(DEGREE_SCRUB_STEP));
    CHECK(GetScrubStep(QString(), 0.0, 1.0) == Catch::Approx(COLOUR_SCRUB_STEP));
    CHECK(GetScrubStep(QStringLiteral("m"), -1e6, 1e6) == Catch::Approx(METRE_SCRUB_STEP));
    CHECK(GetScrubStep(QStringLiteral("×"), 1e-6, 1e3) == Catch::Approx(METRE_SCRUB_STEP));
    CHECK(GetScrubStep(QString(), WIDE_RANGE_MINIMUM, WIDE_RANGE_MAXIMUM) == Catch::Approx(WIDE_RANGE_STEP));
    // Never so small a drag does nothing, nor so large a pixel jumps the whole range.
    CHECK(GetScrubStep(QString(), 0.0, 1e9) <= 1.0);
    CHECK(GetScrubStep(QString(), 0.0, 5.0) >= 0.001);
}

TEST_CASE("Long identifiers are shortened and short ones are left alone", "[editor][inspector]")
{
    CHECK(ShortenIdentifier(LONG_IDENTIFIER) == QStringLiteral("0b2d8a4c…4c5d"));
    CHECK(ShortenIdentifier(QStringLiteral("starter.cube")) == QStringLiteral("starter.cube"));
    CHECK(ShortenIdentifier(QString()).isEmpty() == true);
}

TEST_CASE("An identifier row shows the short form and copies the full text", "[editor][inspector]")
{
    IdentifierRow row(LONG_IDENTIFIER, nullptr);
    row.show();
    const QLabel* textOrNull = row.findChild<QLabel*>(QStringLiteral("inspectorIdentifierText"));
    REQUIRE(textOrNull != nullptr);
    CHECK(textOrNull->text() == ShortenIdentifier(LONG_IDENTIFIER));
    CHECK(textOrNull->toolTip().contains(LONG_IDENTIFIER) == true);
    QToolButton* copyOrNull = row.findChild<QToolButton*>(QStringLiteral("inspectorIdentifierCopy"));
    REQUIRE(copyOrNull != nullptr);
    QApplication::clipboard()->setText(QStringLiteral("before"));
    copyOrNull->click();
    CHECK(QApplication::clipboard()->text() == LONG_IDENTIFIER);
    CHECK(row.GetIdentifier() == LONG_IDENTIFIER);
}

TEST_CASE("Dragging an axis letter scrubs its field and reports the edit", "[editor][inspector]")
{
    QWidget host;
    QLineEdit field(QStringLiteral("1"), &host);
    QLineEdit other(QStringLiteral("5"), &host);
    int scrubbedCount = 0;
    const QVector<VectorComponent> components{
    {
        .Name = QStringLiteral("X"),
        .FieldOrNull = &field,
        .Settings = { .Minimum = -100.0, .Maximum = 100.0, .Step = 0.5 },
    },
                                              {
                                                  .Name = QStringLiteral("Y"),
                                                  .FieldOrNull = &other,
                                                  .Settings = { .Minimum = -100.0, .Maximum = 100.0, .Step = 0.5 },
                                              }};
    QWidget* row = MakeVectorRow(components, QStringLiteral("m"), &host, [&scrubbedCount]() { ++scrubbedCount; });
    host.resize(320, 40);
    row->resize(320, 40);
    host.show();
    QSignalSpy edited(&field, &QLineEdit::textEdited);
    ScrubLabel* labelOrNull = row->findChild<ScrubLabel*>(QStringLiteral("inspectorScrub:X"));
    REQUIRE(labelOrNull != nullptr);
    QTest::mousePress(labelOrNull, Qt::LeftButton, Qt::NoModifier, QPoint(GRAB_X, GRAB_Y));
    CHECK(labelOrNull->IsScrubbing() == true);
    QTest::mouseMove(labelOrNull, QPoint(GRAB_X + SCRUB_DISTANCE, GRAB_Y));
    QTest::mouseRelease(labelOrNull, Qt::LeftButton, Qt::NoModifier, QPoint(GRAB_X + SCRUB_DISTANCE, GRAB_Y));
    CHECK(labelOrNull->IsScrubbing() == false);
    CHECK(field.text().toDouble() == Catch::Approx(1.0 + SCRUB_DISTANCE * 0.5));
    CHECK(other.text() == QStringLiteral("5"));
    CHECK(scrubbedCount >= 1);
    CHECK(edited.count() >= 1);
    // The letters and fields sit on one line, with the unit after them.
    QLabel* unitOrNull = row->findChild<QLabel*>(QStringLiteral("inspectorUnit"));
    REQUIRE(unitOrNull != nullptr);
    CHECK(unitOrNull->text() == QStringLiteral("m"));
    CHECK(GetRectangleIn(host, field).top() < GetRectangleIn(host, other).bottom());
    CHECK(GetRectangleIn(host, other).top() < GetRectangleIn(host, field).bottom());
    CHECK(GetRectangleIn(host, field).right() < GetRectangleIn(host, other).left());
}

TEST_CASE("A click on an axis letter focuses the field without changing it", "[editor][inspector]")
{
    QWidget host;
    QLineEdit field(QStringLiteral("2.5"), &host);
    const QVector<VectorComponent> components{
    {
        .Name = QStringLiteral("Z"),
        .FieldOrNull = &field,
        .Settings = { .Minimum = 0.0, .Maximum = 10.0, .Step = 0.1 },
    }};
    QWidget* row = MakeVectorRow(components, QString(), &host, []() {});
    host.resize(200, 40);
    row->resize(200, 40);
    host.show();
    ScrubLabel* labelOrNull = row->findChild<ScrubLabel*>(QStringLiteral("inspectorScrub:Z"));
    REQUIRE(labelOrNull != nullptr);
    QTest::mouseClick(labelOrNull, Qt::LeftButton, Qt::NoModifier, QPoint(GRAB_X, GRAB_Y));
    CHECK(field.text() == QStringLiteral("2.5"));
    CHECK(host.focusWidget() == &field);
}

TEST_CASE("A colour swatch follows its fields and writes the picked colour back", "[editor][inspector]")
{
    QWidget host;
    QLineEdit red(QStringLiteral("1"), &host);
    QLineEdit green(QStringLiteral("0"), &host);
    QLineEdit blue(QStringLiteral("0"), &host);
    ColourSwatch swatch(red, green, blue, &host);
    CHECK(swatch.GetDisplayColour() == QColor(255, 0, 0));
    green.setText(QStringLiteral("1"));
    CHECK(swatch.GetDisplayColour() == QColor(255, 255, 0));
    // Half linear is brighter than half displayed: the swatch shows what the renderer outputs.
    red.setText(QStringLiteral("0.5"));
    CHECK(swatch.GetDisplayColour().red() > DISPLAY_GREY);
    // Text that is not a number gives no colour rather than a wrong one.
    blue.setText(QStringLiteral("-"));
    CHECK(swatch.GetDisplayColour().isValid() == false);

    QSignalSpy picked(&swatch, &ColourSwatch::Picked);
    swatch.SetDisplayColour(QColor(DISPLAY_GREY, DISPLAY_GREY, DISPLAY_GREY));
    CHECK(picked.count() == 1);
    CHECK(red.text().toDouble() == Catch::Approx(GREY_LINEAR).margin(0.001));
    CHECK(green.text().toDouble() == Catch::Approx(GREY_LINEAR).margin(0.001));
    CHECK(blue.text().toDouble() == Catch::Approx(GREY_LINEAR).margin(0.001));
    CHECK(std::abs(swatch.GetDisplayColour().red() - DISPLAY_GREY) <= SWATCH_TOLERANCE);
}

TEST_CASE("Linear and display colours convert both ways", "[editor][inspector]")
{
    CHECK(ConvertLinearToDisplayColour(0.0, 0.0, 0.0) == QColor(0, 0, 0));
    CHECK(ConvertLinearToDisplayColour(1.0, 1.0, 1.0) == QColor(255, 255, 255));
    float64 red = 0;
    float64 green = 0;
    float64 blue = 0;
    ConvertDisplayToLinearColour(QColor(255, 0, 128), red, green, blue);
    CHECK(red == Catch::Approx(1.0));
    CHECK(green == Catch::Approx(0.0));
    const QColor roundTrip = ConvertLinearToDisplayColour(red, green, blue);
    CHECK(roundTrip == QColor(255, 0, 128));
}

TEST_CASE("The world inspector shows compact transform rows and short identifiers", "[editor][inspector][world]")
{
    QTemporaryDir dir;
    const QString path = MakeStarterProject(dir);
    WorldWorkspace view;
    view.SetProject(dir.path(), 1);
    REQUIRE(view.Open(path));
    view.resize(WORKSPACE_WIDTH, WORKSPACE_HEIGHT);
    view.show();
    QTreeWidget* listOrNull = view.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(listOrNull != nullptr);
    listOrNull->setCurrentItem(listOrNull->topLevelItem(0), 0, QItemSelectionModel::ClearAndSelect);
    QTest::qWait(SETTLE_MILLISECONDS);
    QWidget* window = view.window();

    // Position, rotation and scale each take one line of three fields instead of nine stacked rows.
    const auto find = [window](const char* key) {
        QLineEdit* fieldOrNull =
            window->findChild<QLineEdit*>(QStringLiteral("worldField:") + QString::fromLatin1(key));
        REQUIRE(fieldOrNull != nullptr);
        return fieldOrNull;
    };
    const QRect x = GetRectangleIn(*window, *find("transform/x"));
    const QRect y = GetRectangleIn(*window, *find("transform/y"));
    const QRect z = GetRectangleIn(*window, *find("transform/z"));
    CHECK(x.top() < y.bottom());
    CHECK(y.top() < x.bottom());
    CHECK(y.top() < z.bottom());
    CHECK(x.right() < y.left());
    CHECK(y.right() < z.left());
    const QRect rotationX = GetRectangleIn(*window, *find("transform/rx"));
    CHECK(rotationX.top() >= x.bottom());
    CHECK(window->findChild<ScrubLabel*>(QStringLiteral("inspectorScrub:X")) != nullptr);

    // Dragging the X letter edits the draft through the same path as typing.
    ScrubLabel* scrubOrNull = window->findChild<ScrubLabel*>(QStringLiteral("inspectorScrub:X"));
    REQUIRE(scrubOrNull != nullptr);
    const QString before = find("transform/x")->text();
    QTest::mousePress(scrubOrNull, Qt::LeftButton, Qt::NoModifier, QPoint(GRAB_X, GRAB_Y));
    QTest::mouseMove(scrubOrNull, QPoint(GRAB_X + SCRUB_DISTANCE, GRAB_Y));
    QTest::mouseRelease(scrubOrNull, Qt::LeftButton, Qt::NoModifier, QPoint(GRAB_X + SCRUB_DISTANCE, GRAB_Y));
    CHECK(find("transform/x")->text() != before);
    CHECK(view.Dirty() == true);

    // The stable identifier is short with a copy button, and the full text is what is copied.
    IdentifierRow* identifierOrNull = window->findChild<IdentifierRow*>(QStringLiteral("worldEntityIdentifier"));
    REQUIRE(identifierOrNull != nullptr);
    CHECK(identifierOrNull->GetIdentifier().isEmpty() == false);
    identifierOrNull->Copy();
    CHECK(QApplication::clipboard()->text() == identifierOrNull->GetIdentifier());
}

TEST_CASE("The world inspector shows a swatch beside colour channels", "[editor][inspector][world]")
{
    QTemporaryDir dir;
    const QString path = MakeStarterProject(dir);
    WorldWorkspace view;
    view.SetProject(dir.path(), 1);
    REQUIRE(view.Open(path));
    view.resize(WORKSPACE_WIDTH, WORKSPACE_HEIGHT);
    view.show();
    QTreeWidget* listOrNull = view.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(listOrNull != nullptr);
    listOrNull->setCurrentItem(listOrNull->topLevelItem(0), 0, QItemSelectionModel::ClearAndSelect);
    QComboBox* choices = view.window()->findChild<QComboBox*>(QStringLiteral("worldComponentChoice"));
    REQUIRE(choices != nullptr);
    const int light = choices->findData(QStringLiteral("light"));
    REQUIRE(light >= 0);
    choices->setCurrentIndex(light);
    view.window()->findChild<QPushButton*>(QStringLiteral("worldAttach"))->click();
    QTest::qWait(SETTLE_MILLISECONDS);
    ColourSwatch* swatchOrNull = view.window()->findChild<ColourSwatch*>(QStringLiteral("inspectorColourSwatch"));
    REQUIRE(swatchOrNull != nullptr);
    QLineEdit* redOrNull = view.window()->findChild<QLineEdit*>(QStringLiteral("worldField:light/r"));
    REQUIRE(redOrNull != nullptr);
    const QString before = redOrNull->text();
    swatchOrNull->SetDisplayColour(QColor(0, 0, 0));
    CHECK(redOrNull->text() != before);
    CHECK(view.Dirty() == true);
}

namespace
{
constexpr float64 WIDE_METRES_MAXIMUM = 10000.0;
constexpr float64 OPENING_METRES = 0.003;
constexpr int32 MIDDLE_POSITION = SLIDER_POSITION_COUNT / 2;
constexpr int32 TWENTIETH_POSITION = SLIDER_POSITION_COUNT / 20;
constexpr float64 COLOUR_THIRD = 0.333;
constexpr int32 THIRD_POSITION = 333;
constexpr float64 AXIS_COUNT_MINIMUM = 2.0;
constexpr float64 AXIS_COUNT_MAXIMUM = 64.0;
constexpr float64 AXIS_COUNT_MIDDLE = 33.0;
constexpr float64 SYMMETRIC_LIMIT = 1e6;
/// Longest text a slider writes, such as "-0.000139": rounded values, never a 17-digit expansion.
constexpr qsizetype MAXIMUM_SLIDER_TEXT = 9;
} // namespace

TEST_CASE("Inspector numbers show their shortest exact text", "[editor][inspector]")
{
    CHECK(FormatInspectorNumber(OPENING_METRES) == QStringLiteral("0.003"));
    CHECK(FormatInspectorNumber(1.0) == QStringLiteral("1"));
    CHECK(FormatInspectorNumber(-2.5) == QStringLiteral("-2.5"));
    // The text reads back as exactly the same number.
    const float64 sum = 0.1 + 0.2;
    CHECK(FormatInspectorNumber(sum).toDouble() == sum);
}

TEST_CASE("A slider is linear over narrow ranges and logarithmic over wide ones", "[editor][inspector]")
{
    CHECK(HasSliderRange(0.0, 1.0) == true);
    CHECK(HasSliderRange(1.0, 1.0) == false);
    CHECK(HasSliderRange(0.0, std::numeric_limits<float64>::infinity()) == false);

    const SliderMapping colour = MakeSliderMapping(0.0, 1.0, GetScrubStep(QString(), 0.0, 1.0), false);
    CHECK(colour.Scale == SliderScale::Linear);
    CHECK(ConvertSliderPositionToValue(MIDDLE_POSITION, colour) == 0.5);
    CHECK(ConvertSliderPositionToValue(THIRD_POSITION, colour) == COLOUR_THIRD);
    CHECK(ConvertValueToSliderPosition(0.5, colour) == MIDDLE_POSITION);

    const SliderMapping degrees = MakeSliderMapping(-360.0, 360.0, GetScrubStep(QStringLiteral("°"), -360, 360), false);
    CHECK(degrees.Scale == SliderScale::Linear);
    CHECK(ConvertSliderPositionToValue(MIDDLE_POSITION, degrees) == 0.0);

    // 0 to 10000 m: half the travel lands near 10 m and a short drag still reaches millimetres.
    const SliderMapping metres =
        MakeSliderMapping(0.0, WIDE_METRES_MAXIMUM, GetScrubStep(QStringLiteral("m"), 0.0, WIDE_METRES_MAXIMUM), false);
    CHECK(metres.Scale == SliderScale::Logarithmic);
    CHECK(ConvertSliderPositionToValue(0, metres) == 0.0);
    CHECK(ConvertSliderPositionToValue(SLIDER_POSITION_COUNT, metres) == WIDE_METRES_MAXIMUM);
    const float64 middle = ConvertSliderPositionToValue(MIDDLE_POSITION, metres);
    CHECK(middle > 1.0);
    CHECK(middle < 100.0);
    CHECK(ConvertSliderPositionToValue(TWENTIETH_POSITION, metres) < 0.01);
    CHECK(ConvertValueToSliderPosition(OPENING_METRES, metres) > 0);
    CHECK(ConvertValueToSliderPosition(OPENING_METRES, metres) < TWENTIETH_POSITION);
    // Values outside the range give the nearer end.
    CHECK(ConvertValueToSliderPosition(2 * WIDE_METRES_MAXIMUM, metres) == SLIDER_POSITION_COUNT);
    CHECK(ConvertValueToSliderPosition(-1.0, metres) == 0);

    // A range around zero puts zero in the middle on both scales.
    const SliderMapping symmetric = MakeSliderMapping(-SYMMETRIC_LIMIT, SYMMETRIC_LIMIT, METRE_SCRUB_STEP, false);
    CHECK(symmetric.Scale == SliderScale::Logarithmic);
    CHECK(ConvertSliderPositionToValue(MIDDLE_POSITION, symmetric) == 0.0);
    CHECK(ConvertSliderPositionToValue(MIDDLE_POSITION - 1, symmetric) < 0.0);

    // Integer fields move in whole numbers.
    const SliderMapping axisCount = MakeSliderMapping(AXIS_COUNT_MINIMUM, AXIS_COUNT_MAXIMUM, 0.062, true);
    CHECK(axisCount.Scale == SliderScale::Linear);
    CHECK(ConvertSliderPositionToValue(MIDDLE_POSITION, axisCount) == AXIS_COUNT_MIDDLE);

    // Every mapping is monotonic, stays in range, writes short text and finds its own values again.
    for (const SliderMapping& mapping : {colour, degrees, metres, symmetric, axisCount})
    {
        float64 previous = mapping.Minimum;
        for (int32 position = 0; position <= SLIDER_POSITION_COUNT; ++position)
        {
            const float64 value = ConvertSliderPositionToValue(position, mapping);
            CHECK(value >= previous);
            CHECK(value <= mapping.Maximum);
            CHECK(FormatInspectorNumber(value).size() <= MAXIMUM_SLIDER_TEXT);
            if (mapping.IsInteger == false)
            {
                CHECK(std::abs(ConvertValueToSliderPosition(value, mapping) - position) <= 1);
            }
            previous = value;
        }
    }
}

TEST_CASE("A bounded slider and its field follow each other", "[editor][inspector]")
{
    QWidget host;
    QLineEdit field(QStringLiteral("0.003"), &host);
    const SliderMapping mapping = MakeSliderMapping(0.0, WIDE_METRES_MAXIMUM, METRE_SCRUB_STEP, false);
    QWidget* row = MakeSliderRow(field, mapping, &host);
    host.resize(320, 40);
    row->resize(320, 40);
    host.show();
    BoundedSlider* sliderOrNull = row->findChild<BoundedSlider*>(QStringLiteral("inspectorSlider"));
    REQUIRE(sliderOrNull != nullptr);
    CHECK(field.parentWidget() == row);
    CHECK(sliderOrNull->value() == ConvertValueToSliderPosition(OPENING_METRES, mapping));

    // Moving the slider writes the field through textEdited, like typing.
    QSignalSpy edited(&field, &QLineEdit::textEdited);
    sliderOrNull->setValue(MIDDLE_POSITION);
    CHECK(field.text() == FormatInspectorNumber(ConvertSliderPositionToValue(MIDDLE_POSITION, mapping)));
    CHECK(edited.count() == 1);

    // Text set or typed in the field moves the handle without editing the field again.
    field.setText(QStringLiteral("10000"));
    CHECK(sliderOrNull->value() == SLIDER_POSITION_COUNT);
    field.setText(QStringLiteral("not a number"));
    CHECK(sliderOrNull->value() == SLIDER_POSITION_COUNT);
    CHECK(edited.count() == 1);

    // The slider is unavailable whenever its field is.
    field.setEnabled(false);
    CHECK(sliderOrNull->isEnabled() == false);
    field.setEnabled(true);
    CHECK(sliderOrNull->isEnabled() == true);
    CHECK(GetRectangleIn(host, *sliderOrNull).right() < GetRectangleIn(host, field).left());
}

TEST_CASE("The world inspector gives bounded number fields a slider", "[editor][inspector][world]")
{
    QTemporaryDir dir;
    const QString path = MakeStarterProject(dir);
    WorldWorkspace view;
    view.SetProject(dir.path(), 1);
    REQUIRE(view.Open(path));
    view.resize(WORKSPACE_WIDTH, WORKSPACE_HEIGHT);
    view.show();
    QTreeWidget* listOrNull = view.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(listOrNull != nullptr);
    listOrNull->setCurrentItem(listOrNull->topLevelItem(0), 0, QItemSelectionModel::ClearAndSelect);
    QComboBox* choices = view.window()->findChild<QComboBox*>(QStringLiteral("worldComponentChoice"));
    REQUIRE(choices != nullptr);
    const int light = choices->findData(QStringLiteral("light"));
    REQUIRE(light >= 0);
    choices->setCurrentIndex(light);
    view.window()->findChild<QPushButton*>(QStringLiteral("worldAttach"))->click();
    QTest::qWait(SETTLE_MILLISECONDS);

    QLineEdit* intensityOrNull = view.window()->findChild<QLineEdit*>(QStringLiteral("worldField:light/intensity"));
    REQUIRE(intensityOrNull != nullptr);
    REQUIRE(intensityOrNull->parentWidget() != nullptr);
    BoundedSlider* sliderOrNull =
        intensityOrNull->parentWidget()->findChild<BoundedSlider*>(QStringLiteral("inspectorSlider"));
    REQUIRE(sliderOrNull != nullptr);
    CHECK(sliderOrNull->GetMapping().Scale == SliderScale::Logarithmic);
    CHECK(sliderOrNull->GetMapping().Maximum > sliderOrNull->GetMapping().Minimum);
    CHECK(sliderOrNull->toolTip().contains(QStringLiteral("logarithmic")) == true);
    // The slider fills the field column beside a number field sized to its value, not a stub beside a wide field.
    CHECK(intensityOrNull->minimumWidth() == intensityOrNull->maximumWidth());
    CHECK(sliderOrNull->width() > intensityOrNull->width());

    // The slider edits the draft through the same path as typing.
    const QString before = intensityOrNull->text();
    sliderOrNull->setValue(sliderOrNull->value() == MIDDLE_POSITION ? MIDDLE_POSITION + 1 : MIDDLE_POSITION);
    CHECK(intensityOrNull->text() != before);
    CHECK(view.Dirty() == true);

    // Colour channels keep their compact row and swatch instead of three sliders.
    QLineEdit* redOrNull = view.window()->findChild<QLineEdit*>(QStringLiteral("worldField:light/r"));
    REQUIRE(redOrNull != nullptr);
    CHECK(redOrNull->parentWidget()->findChild<BoundedSlider*>() == nullptr);

    // Angles and distances are typed or scrubbed: a slider over their whole range moves in jumps.
    qsizetype checkedCount = 0;
    for (const QJsonValue component : view.Document().Schema().value(QStringLiteral("components")).toArray())
    {
        if (component.toObject().value(QStringLiteral("id")).toString() != QStringLiteral("light"))
        {
            continue;
        }
        for (const QJsonValue property : component.toObject().value(QStringLiteral("properties")).toArray())
        {
            const QString unit = property.toObject().value(QStringLiteral("unit")).toString();
            const QLineEdit* fieldOrNull = view.window()->findChild<QLineEdit*>(
                QStringLiteral("worldField:light/") + property.toObject().value(QStringLiteral("id")).toString());
            if ((unit == QStringLiteral("°") || unit == QStringLiteral("m")) && fieldOrNull != nullptr)
            {
                INFO(fieldOrNull->objectName().toStdString());
                CHECK(fieldOrNull->parentWidget()->findChild<BoundedSlider*>(QString(), Qt::FindDirectChildrenOnly) ==
                      nullptr);
                ++checkedCount;
            }
        }
    }
    CHECK(checkedCount > 0);
    CHECK(ShouldUseSlider(0.0, 1.0, QString()) == true);
    CHECK(ShouldUseSlider(0.0, 89.0, QStringLiteral("°")) == false);
    CHECK(ShouldUseSlider(0.0, 1000.0, QStringLiteral("m")) == false);
}
