#include "internal/reference_power_view.h"

#include "design_tokens.h"
#include "internal/captured_image_view.h"
#include "internal/viewport_reference_capture.h"

#include <ludus/content/render_graph_format/capture.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <QtEndian>

#include <bit>
#include <cmath>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace format = content::render_graph_format;

// RT1's LUHDR01 layout is documented under Reference workflow and frozen profile in renderer-systems.md.
// Width, height, target and the pixel record have the same layout as LURGCAP.
constexpr char REFERENCE_MAGIC[] = "LUHDR01";
constexpr usize REFERENCE_HEADER_SIZE_OFFSET = 20;
constexpr usize REFERENCE_FLAGS_OFFSET = 44;
constexpr uint32 REFERENCE_PARTIAL_FLAG = 32;
constexpr usize REFERENCE_MATRIX_OFFSET = 96;
constexpr usize REFERENCE_CAMERA_OFFSET = 160;
constexpr usize CLIP_COMPONENTS = 4;
constexpr usize RADIANCE_CHANNELS = 3;
constexpr usize SAMPLE_OFFSET = RADIANCE_CHANNELS * sizeof(float32);
constexpr usize MAXIMUM_FILE_BYTES = format::CAPTURE_HEADER_BYTES + usize{MAXIMUM_REFERENCE_LONG_SIDE} *
                                                                        MAXIMUM_REFERENCE_LONG_SIDE *
                                                                        format::CAPTURE_PIXEL_BYTES;
constexpr float64 MAXIMUM_RECEIVER_DISTANCE_METRES = 2'000'000;
constexpr float64 PERCENT_SCALE = 100;
constexpr int32 PERCENT_DECIMALS = 3;
constexpr int32 RECEIVER_DECIMALS = 6;
constexpr float64 DEFAULT_EXPECTED_PERCENT = 50;
constexpr int32 MAXIMUM_DISPLAY_CHANNEL = 255;
constexpr int32 TINT_SECONDARY_DIVISOR = 3;

uint32 ReadWord(const QByteArray& bytes, usize offset) noexcept
{
    return qFromLittleEndian<uint32>(bytes.constData() + offset);
}

float32 ReadFloat(const QByteArray& bytes, usize offset) noexcept
{
    return std::bit_cast<float32>(ReadWord(bytes, offset));
}

struct RawPowerCapture final
{
    QByteArray Bytes;
    StaticArray<float64, format::CAPTURE_MATRIX_VALUES> WorldToClip{};
    StaticArray<float32, format::CAPTURE_POSITION_VALUES> CameraWorld{};
    uint32 Width = 0;
    uint32 Height = 0;
    uint32 TargetSamples = 0;

    [[nodiscard]] bool ReadChannel(usize pixel, usize channel, float64& outValue) const noexcept
    {
        const usize offset = format::CAPTURE_HEADER_BYTES + pixel * format::CAPTURE_PIXEL_BYTES;
        const float32 value = ReadFloat(Bytes, offset + channel * sizeof(float32));
        if (std::isfinite(value) == false || value < 0 || ReadWord(Bytes, offset + SAMPLE_OFFSET) < TargetSamples)
        {
            return false;
        }
        outValue = static_cast<float64>(value);
        return true;
    }
};

bool ReadCapture(const QString& path, RawPowerCapture& outCapture, QString& outError)
{
    QFile file(path);
    if (file.open(QIODevice::ReadOnly) == false)
    {
        outError = QStringLiteral("Could not open %1. Choose a readable raw capture.").arg(path);
        return false;
    }
    const qint64 size = file.size();
    if (size < format::CAPTURE_HEADER_BYTES || size > static_cast<qint64>(MAXIMUM_FILE_BYTES))
    {
        outError = QStringLiteral("%1 is outside the capture size limits. Export a completed capture at up to %2 px.")
                       .arg(path)
                       .arg(MAXIMUM_REFERENCE_LONG_SIDE);
        return false;
    }
    outCapture.Bytes = file.read(size);
    const QByteArray& bytes = outCapture.Bytes;
    const bool isGraph = bytes.startsWith(QByteArrayView(format::CAPTURE_MAGIC, sizeof(format::CAPTURE_MAGIC)));
    const bool isReference = bytes.startsWith(QByteArrayView(REFERENCE_MAGIC, sizeof(REFERENCE_MAGIC)));
    if (bytes.size() != size || (isGraph == false && isReference == false) ||
        ReadWord(bytes, format::CAPTURE_VERSION_OFFSET) != format::CAPTURE_VERSION ||
        ReadWord(bytes, isGraph ? format::CAPTURE_HEADER_SIZE_OFFSET : REFERENCE_HEADER_SIZE_OFFSET) !=
            format::CAPTURE_HEADER_BYTES ||
        (isGraph == true && ReadWord(bytes, format::CAPTURE_PIXEL_SIZE_OFFSET) != format::CAPTURE_PIXEL_BYTES))
    {
        outError =
            QStringLiteral("Could not read %1. Choose a raw .luhdr or .lurgcap file saved yFromY Reference Capture.")
                .arg(path);
        return false;
    }

    const uint32 flags = ReadWord(bytes, isGraph ? format::CAPTURE_FLAGS_OFFSET : REFERENCE_FLAGS_OFFSET);
    constexpr uint32 GRAPH_INCOMPLETE_FLAGS =
        static_cast<uint32>(format::CaptureFlag::Partial) | static_cast<uint32>(format::CaptureFlag::Failed);
    const bool isIncomplete = (flags & (isGraph ? GRAPH_INCOMPLETE_FLAGS : REFERENCE_PARTIAL_FLAG)) != 0 ||
                              (isGraph == true && ReadWord(bytes, format::CAPTURE_FAILED_PIXELS_OFFSET) != 0);
    outCapture.TargetSamples = ReadWord(bytes, format::CAPTURE_TARGET_SAMPLES_OFFSET);
    outCapture.Width = ReadWord(bytes, format::CAPTURE_WIDTH_OFFSET);
    outCapture.Height = ReadWord(bytes, format::CAPTURE_HEIGHT_OFFSET);
    if (isIncomplete == true || outCapture.TargetSamples == 0 || outCapture.Width == 0 ||
        outCapture.Width > MAXIMUM_REFERENCE_LONG_SIDE || outCapture.Height == 0 ||
        outCapture.Height > MAXIMUM_REFERENCE_LONG_SIDE ||
        static_cast<usize>(size) !=
            format::CAPTURE_HEADER_BYTES + usize{outCapture.Width} * outCapture.Height * format::CAPTURE_PIXEL_BYTES)
    {
        outError =
            QStringLiteral("%1 is incomplete or invalid. Run the capture to completion and save it again.").arg(path);
        return false;
    }

    const usize matrixOffset = isGraph ? format::CAPTURE_WORLD_TO_CLIP_OFFSET : REFERENCE_MATRIX_OFFSET;
    const usize cameraOffset = isGraph ? format::CAPTURE_CAMERA_POSITION_OFFSET : REFERENCE_CAMERA_OFFSET;
    for (usize i = 0; i < outCapture.WorldToClip.GetSize(); ++i)
    {
        outCapture.WorldToClip[i] = static_cast<float64>(ReadFloat(bytes, matrixOffset + i * sizeof(float32)));
    }
    for (usize i = 0; i < outCapture.CameraWorld.GetSize(); ++i)
    {
        outCapture.CameraWorld[i] = ReadFloat(bytes, cameraOffset + i * sizeof(float32));
    }
    return true;
}

bool AreViewsEqual(const RawPowerCapture& first, const RawPowerCapture& second) noexcept
{
    if (first.Width != second.Width || first.Height != second.Height)
    {
        return false;
    }
    for (usize i = 0; i < first.WorldToClip.GetSize(); ++i)
    {
        if (first.WorldToClip[i] != second.WorldToClip[i] || std::isfinite(first.WorldToClip[i]) == false)
        {
            return false;
        }
    }
    for (usize i = 0; i < first.CameraWorld.GetSize(); ++i)
    {
        if (first.CameraWorld[i] != second.CameraWorld[i] || std::isfinite(first.CameraWorld[i]) == false)
        {
            return false;
        }
    }
    return true;
}

struct ReceiverPoint final
{
    float64 X = 0;
    float64 Y = 0;
};

class ReceiverProjection final
{
public:
    ReceiverProjection(const RawPowerCapture& capture, float64 receiverZWorld) noexcept
    {
        // The capture is column-major. Restrict the transform to one world XY plane.
        for (usize row = 0; row < CLIP_COMPONENTS; ++row)
        {
            mX[row] = capture.WorldToClip[row];
            mY[row] = capture.WorldToClip[row + CLIP_COMPONENTS];
            mConstant[row] = capture.WorldToClip[row + 2 * CLIP_COMPONENTS] * receiverZWorld +
                             capture.WorldToClip[row + (CLIP_COMPONENTS - 1) * CLIP_COMPONENTS];
        }
    }

    [[nodiscard]] bool ProjectCorner(float64 xNdc, float64 yNdc, ReceiverPoint& outPoint) const noexcept
    {
        constexpr usize X = 0;
        constexpr usize Y = 1;
        constexpr usize Z = 2;
        constexpr usize W = CLIP_COMPONENTS - 1;
        const float64 xFromX = mX[X] - xNdc * mX[W];
        const float64 xFromY = mY[X] - xNdc * mY[W];
        const float64 xConstant = mConstant[X] - xNdc * mConstant[W];
        const float64 yFromX = mX[Y] - yNdc * mX[W];
        const float64 yFromY = mY[Y] - yNdc * mY[W];
        const float64 yConstant = mConstant[Y] - yNdc * mConstant[W];
        const float64 determinant = xFromX * yFromY - yFromX * xFromY;
        if (determinant == 0 || std::isfinite(determinant) == false)
        {
            return false;
        }
        outPoint.X = (xFromY * yConstant - yFromY * xConstant) / determinant;
        outPoint.Y = (yFromX * xConstant - xFromX * yConstant) / determinant;
        const float64 depth = mX[Z] * outPoint.X + mY[Z] * outPoint.Y + mConstant[Z];
        const float64 weight = mX[W] * outPoint.X + mY[W] * outPoint.Y + mConstant[W];
        return std::isfinite(outPoint.X) && std::isfinite(outPoint.Y) && std::isfinite(weight) && weight > 0 &&
               depth >= 0 && depth <= weight;
    }

private:
    StaticArray<float64, CLIP_COMPONENTS> mX{};
    StaticArray<float64, CLIP_COMPONENTS> mY{};
    StaticArray<float64, CLIP_COMPONENTS> mConstant{};
};

float64
ComputeArea(ReceiverPoint topLeft, ReceiverPoint topRight, ReceiverPoint bottomRight, ReceiverPoint bottomLeft) noexcept
{
    return 0.5 * std::abs((bottomRight.X - topLeft.X) * (bottomLeft.Y - topRight.Y) -
                          (bottomRight.Y - topLeft.Y) * (bottomLeft.X - topRight.X));
}

struct PowerRequest final
{
    QString Candidate;
    QString Open;
    QString Expected;
    float64 ReceiverZWorld = 0;
    float64 ExpectedFraction = 0;
    float64 Tolerance = 0;
    usize Channel = 0;
};

struct PowerResult final
{
    QString Message;
    QImage Heatmap;
};

struct PowerPixel final
{
    float64 Candidate = 0;
    float64 Expected = 0;
};

// Thanks to Steinberg et al., A Free-Space Diffraction BSDF, supplemental Eq. S1.4,
// for the aperture/receiver balance. See scripts/check-render-power for the offline report.
PowerResult ComputePower(const PowerRequest& request)
{
    RawPowerCapture open;
    RawPowerCapture candidate;
    RawPowerCapture expected;
    QString error;
    if (ReadCapture(request.Open, open, error) == false || ReadCapture(request.Candidate, candidate, error) == false ||
        ReadCapture(request.Expected, expected, error) == false)
    {
        return {error, {}};
    }
    if (AreViewsEqual(open, candidate) == false || AreViewsEqual(open, expected) == false)
    {
        return {
            QStringLiteral("The captures have different views. Save all three from the same camera and resolution."),
            {}};
    }

    const ReceiverProjection projection(open, request.ReceiverZWorld);
    Array<ReceiverPoint> top;
    Array<ReceiverPoint> bottom;
    Array<PowerPixel> powers;
    if (top.TryResize(open.Width + 1) == false || bottom.TryResize(open.Width + 1) == false ||
        powers.TryResize(usize{open.Width} * open.Height) == false)
    {
        return {
            QStringLiteral("Could not allocate the power report. Capture all three images at a smaller resolution."),
            {}};
    }
    const auto projectRow = [&projection, &open](uint32 row, Array<ReceiverPoint>& outRow) {
        for (uint32 column = 0; column <= open.Width; ++column)
        {
            const float64 x = 2 * static_cast<float64>(column) / open.Width - 1;
            const float64 y = 1 - 2 * static_cast<float64>(row) / open.Height;
            if (projection.ProjectCorner(x, y, outRow[column]) == false)
            {
                return false;
            }
        }
        return true;
    };
    const QString projectionError =
        QStringLiteral("The receiver plane does not cover the view. Check Receiver Z and the camera framing.");
    if (projectRow(0, top) == false)
    {
        return {projectionError, {}};
    }

    float64 openPower = 0;
    float64 measuredPower = 0;
    float64 expectedPower = 0;
    for (uint32 row = 0; row < open.Height; ++row)
    {
        if (projectRow(row + 1, bottom) == false)
        {
            return {projectionError, {}};
        }
        for (uint32 column = 0; column < open.Width; ++column)
        {
            const usize index = usize{row} * open.Width + column;
            const float64 area = ComputeArea(top[column], top[column + 1], bottom[column + 1], bottom[column]);
            if (area <= 0 || std::isfinite(area) == false)
            {
                return {projectionError, {}};
            }
            float64 openValue = 0;
            float64 candidateValue = 0;
            float64 expectedValue = 0;
            if (open.ReadChannel(index, request.Channel, openValue) == false ||
                candidate.ReadChannel(index, request.Channel, candidateValue) == false ||
                expected.ReadChannel(index, request.Channel, expectedValue) == false)
            {
                return {
                    QStringLiteral("A pixel has invalid radiance or too few samples. Save completed linear captures."),
                    {}};
            }
            powers[index].Candidate = candidateValue * area;
            powers[index].Expected = expectedValue * area;
            openPower += openValue * area;
            measuredPower += powers[index].Candidate;
            expectedPower += powers[index].Expected;
        }
        qSwap(top, bottom);
    }
    if (openPower <= 0 || (expectedPower <= 0 && request.ExpectedFraction > 0))
    {
        return {QStringLiteral("The open capture, or the expected pattern for positive transmission, is dark. "
                               "Check the source or select a lit channel."),
                {}};
    }

    const float64 measuredFraction = measuredPower / openPower;
    if (std::isfinite(openPower) == false || std::isfinite(expectedPower) == false ||
        std::isfinite(measuredFraction * PERCENT_SCALE) == false)
    {
        return {QStringLiteral("The receiver power exceeds the supported range. Check the captures and Receiver Z."),
                {}};
    }

    float64 fullScale = 0;
    float64 excess = 0;
    float64 deficit = 0;
    for (PowerPixel& pixel : powers)
    {
        const float64 expectedShare = expectedPower > 0 ? pixel.Expected / expectedPower : 0;
        pixel.Candidate = pixel.Candidate / openPower - request.ExpectedFraction * expectedShare;
        if (pixel.Candidate > 0)
        {
            excess += pixel.Candidate;
        }
        else
        {
            deficit -= pixel.Candidate;
        }
        fullScale = qMax(fullScale, std::abs(pixel.Candidate));
    }

    QImage heatmap(static_cast<int32>(open.Width), static_cast<int32>(open.Height), QImage::Format_RGB32);
    if (heatmap.isNull() == true)
    {
        return {QStringLiteral("Could not allocate the heatmap. Capture all three images at a smaller resolution."),
                {}};
    }
    for (uint32 row = 0; row < open.Height; ++row)
    {
        QRgb* output = reinterpret_cast<QRgb*>(heatmap.scanLine(static_cast<int32>(row)));
        for (uint32 column = 0; column < open.Width; ++column)
        {
            const float64 residual = powers[usize{row} * open.Width + column].Candidate;
            const int32 brightness =
                residual == 0 ? 0 : qMax(1, qRound(MAXIMUM_DISPLAY_CHANNEL * (std::abs(residual) / fullScale)));
            const int32 green = brightness * 2 / TINT_SECONDARY_DIVISOR;
            output[column] = residual >= 0 ? qRgb(brightness, green, 0) : qRgb(0, green, brightness);
        }
    }

    const bool isWithinTolerance = std::abs(measuredFraction - request.ExpectedFraction) <= request.Tolerance;
    return {QStringLiteral("%1 · Measured %2% · Expected %3%\nExcess %4% · Deficit %5% of open power")
                .arg(isWithinTolerance ? QStringLiteral("Within tolerance") : QStringLiteral("Outside tolerance"))
                .arg(measuredFraction * PERCENT_SCALE, 0, 'f', PERCENT_DECIMALS)
                .arg(request.ExpectedFraction * PERCENT_SCALE, 0, 'f', PERCENT_DECIMALS)
                .arg(excess * PERCENT_SCALE, 0, 'f', PERCENT_DECIMALS)
                .arg(deficit * PERCENT_SCALE, 0, 'f', PERCENT_DECIMALS),
            heatmap};
}
} // namespace

ReferencePowerView::ReferencePowerView(QWidget* parentOrNull) : QWidget(parentOrNull)
{
    setObjectName(QStringLiteral("referencePowerTab"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setSpacing(design::SPACING_SMALL);
    QWidget* settings = new QWidget(this);
    QFormLayout* form = new QFormLayout(settings);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const auto addCapture = [settings, form](const QString& label, const QString& name) {
        QWidget* picker = new QWidget(settings);
        QHBoxLayout* row = new QHBoxLayout(picker);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(design::SPACING_SMALL);
        QLineEdit* path = new QLineEdit(picker);
        path->setObjectName(name);
        path->setAccessibleName(label);
        QPushButton* browse = new QPushButton(QStringLiteral("Browse…"), picker);
        browse->setAccessibleName(QStringLiteral("Browse for %1").arg(label));
        row->addWidget(path, 1);
        row->addWidget(browse);
        form->addRow(label, picker);
        connect(browse, &QPushButton::clicked, picker, [picker, label, path]() {
            const QString selected =
                QFileDialog::getOpenFileName(picker,
                                             label,
                                             path->text(),
                                             QStringLiteral("Linear captures (*.luhdr *.lurgcap)"));
            if (selected.isEmpty() == false)
            {
                path->setText(selected);
            }
        });
        return path;
    };
    QLineEdit* candidate = addCapture(QStringLiteral("Measured"), QStringLiteral("powerMeasured"));
    QLineEdit* open = addCapture(QStringLiteral("Open control"), QStringLiteral("powerOpen"));
    QLineEdit* expected = addCapture(QStringLiteral("Expected pattern"), QStringLiteral("powerExpected"));
    candidate->setToolTip(QStringLiteral("Completed linear capture of the render with the diffraction mask."));
    open->setToolTip(QStringLiteral("Matched capture with the mask removed. This sets the power scale."));
    expected->setToolTip(QStringLiteral("Matched independent diffraction pattern with the mask in place."));

    QDoubleSpinBox* receiverZ = new QDoubleSpinBox(settings);
    receiverZ->setObjectName(QStringLiteral("referencePowerReceiverZ"));
    receiverZ->setDecimals(RECEIVER_DECIMALS);
    receiverZ->setRange(-MAXIMUM_RECEIVER_DISTANCE_METRES, MAXIMUM_RECEIVER_DISTANCE_METRES);
    receiverZ->setValue(-1);
    receiverZ->setSuffix(QStringLiteral(" m"));
    form->addRow(QStringLiteral("Receiver Z"), receiverZ);
    QComboBox* channel = new QComboBox(settings);
    channel->addItems({QStringLiteral("Red"), QStringLiteral("Green"), QStringLiteral("Blue")});
    form->addRow(QStringLiteral("Channel"), channel);
    const auto addPercent = [settings, form](const QString& label, float64 value) {
        QDoubleSpinBox* field = new QDoubleSpinBox(settings);
        field->setDecimals(PERCENT_DECIMALS);
        field->setRange(0, PERCENT_SCALE);
        field->setValue(value);
        field->setSuffix(QStringLiteral("%"));
        form->addRow(label, field);
        return field;
    };
    QDoubleSpinBox* fraction = addPercent(QStringLiteral("Expected transmission"), DEFAULT_EXPECTED_PERCENT);
    QDoubleSpinBox* tolerance = addPercent(QStringLiteral("Tolerance of open power"), 1);
    QPushButton* measure = new QPushButton(QStringLiteral("Measure power"), settings);
    measure->setObjectName(QStringLiteral("measureReferencePower"));
    form->addRow(measure);
    layout->addWidget(settings);

    QLabel* status =
        new QLabel(QStringLiteral("Choose three completed captures from the same view of an XY receiver."), this);
    status->setObjectName(QStringLiteral("referencePowerStatus"));
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status);
    QLabel* legend = new QLabel(QStringLiteral("Orange: excess · Blue: deficit · Black: match"), this);
    legend->hide();
    layout->addWidget(legend);
    CapturedImageView* image = new CapturedImageView(QStringLiteral("power-error"), this);
    image->setObjectName(QStringLiteral("referencePowerImage"));
    image->setAccessibleName(QStringLiteral("Signed receiver power heatmap"));
    layout->addWidget(image, 1);

    QFutureWatcher<PowerResult>* watcher = new QFutureWatcher<PowerResult>(this);
    connect(watcher, &QFutureWatcher<PowerResult>::finished, this, [watcher, settings, status, legend, image]() {
        const PowerResult result = watcher->result();
        status->setText(result.Message);
        image->SetImage(result.Heatmap);
        legend->setVisible(result.Heatmap.isNull() == false);
        settings->setEnabled(true);
    });
    connect(image, &CapturedImageView::SaveReported, status, &QLabel::setText);
    const auto clearResult = [image, legend, status]() {
        image->SetImage({});
        legend->hide();
        status->setText(QStringLiteral("Choose Measure power to use these settings."));
    };
    for (QLineEdit* path : {candidate, open, expected})
    {
        connect(path, &QLineEdit::textChanged, this, clearResult);
    }
    for (QDoubleSpinBox* field : {receiverZ, fraction, tolerance})
    {
        connect(field, &QDoubleSpinBox::valueChanged, this, clearResult);
    }
    connect(channel, &QComboBox::currentIndexChanged, this, clearResult);
    connect(
        measure,
        &QPushButton::clicked,
        this,
        [candidate, open, expected, receiverZ, fraction, tolerance, channel, status, settings, watcher, clearResult]() {
            const PowerRequest request
            {
                .Candidate = candidate->text().trimmed(),
                .Open = open->text().trimmed(),
                .Expected = expected->text().trimmed(),
                .ReceiverZWorld = receiverZ->value(),
                .ExpectedFraction = fraction->value() / PERCENT_SCALE,
                .Tolerance = tolerance->value() / PERCENT_SCALE,
                .Channel = static_cast<usize>(channel->currentIndex()),
            };
            clearResult();
            if (request.Candidate.isEmpty() == true || request.Open.isEmpty() == true ||
                request.Expected.isEmpty() == true)
            {
                status->setText(QStringLiteral("Choose a measured capture, an open control and an expected pattern."));
                return;
            }
            settings->setEnabled(false);
            status->setText(QStringLiteral("Measuring receiver power…"));
            watcher->setFuture(QtConcurrent::run([request]() { return ComputePower(request); }));
        });
}
} // namespace ludus::editor
