#include "internal/editor_icons.h"

#include <QByteArray>
#include <QFile>
#include <QGuiApplication>
#include <QIconEngine>
#include <QList>
#include <QMap>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QXmlStreamAttributes>
#include <QXmlStreamReader>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <tuple>

namespace ludus::editor
{
namespace
{
constexpr qreal HALF = 0.5;
constexpr qreal FULL_TURN = 2.0 * std::numbers::pi_v<qreal>;
constexpr qreal QUARTER_TURN = std::numbers::pi_v<qreal> * HALF;
constexpr qreal BEZIER_ARC_NUMERATOR = 4.0;
constexpr qreal BEZIER_ARC_DENOMINATOR = 3.0;
constexpr qreal QUARTER = 0.25;
constexpr qreal ARC_SEGMENT_TOLERANCE = 0.001;
/// Variants of one icon the cache may hold: sizes, pixel ratios, modes and themes.
constexpr int CACHE_VARIANTS_PER_ICON = 24;
constexpr int MAXIMUM_COMMAND_VALUES = 7;
constexpr int CUBIC_VALUE_COUNT = 6;
constexpr int SMOOTH_CUBIC_VALUE_COUNT = 4;
constexpr int QUADRATIC_VALUE_COUNT = 4;
constexpr int ARC_LEADING_VALUE_COUNT = 3;

struct IconDescriptor final
{
    const char* FileName;
    /// Drawn filled as well as stroked: the live-state actions (see Iconography in the art direction).
    bool IsFilled;
};

constexpr IconDescriptor ICON_DESCRIPTORS[EDITOR_ICON_COUNT] = {
    {"folder-open", false}, {"save", false},
    {"play", true},         {"hammer", false},
    {"rocket", false},      {"square", true},
    {"folder", false},      {"settings", false},
    {"box", false},         {"workflow", false},
    {"code", false},        {"volume-2", false},
    {"image", false},       {"sliders-horizontal", false},
    {"circle-x", false},    {"triangle-alert", false},
    {"info", false},        {"terminal", false},
    {"timer", false},       {"gauge", false},
    {"scan-search", false}, {"waypoints", false},
};

/// Cursor over SVG path data.
struct PathReader final
{
    QStringView Text;
    qsizetype Position = 0;

    void SkipSeparators()
    {
        while (Position < Text.size() && (Text[Position].isSpace() == true || Text[Position] == QLatin1Char(',')))
        {
            ++Position;
        }
    }

    [[nodiscard]] bool IsDone()
    {
        SkipSeparators();
        return Position >= Text.size();
    }

    [[nodiscard]] bool IsAtCommand()
    {
        SkipSeparators();
        return Position < Text.size() && Text[Position].isLetter() == true;
    }

    [[nodiscard]] QChar ReadCommand()
    {
        const QChar command = Text[Position];
        ++Position;
        return command;
    }

    [[nodiscard]] bool TryReadNumber(qreal& outValue)
    {
        SkipSeparators();
        const qsizetype begin = Position;
        qsizetype end = begin;
        const auto isDigit = [this](qsizetype index) { return index < Text.size() && Text[index].isDigit() == true; };
        if (end < Text.size() && (Text[end] == QLatin1Char('-') || Text[end] == QLatin1Char('+')))
        {
            ++end;
        }
        const qsizetype digitsBegin = end;
        while (isDigit(end) == true)
        {
            ++end;
        }
        if (end < Text.size() && Text[end] == QLatin1Char('.'))
        {
            ++end;
            while (isDigit(end) == true)
            {
                ++end;
            }
        }
        if (end == digitsBegin || (end == digitsBegin + 1 && Text[digitsBegin] == QLatin1Char('.')))
        {
            return false;
        }
        if (end < Text.size() && (Text[end] == QLatin1Char('e') || Text[end] == QLatin1Char('E')))
        {
            qsizetype exponentEnd = end + 1;
            if (exponentEnd < Text.size() &&
                (Text[exponentEnd] == QLatin1Char('-') || Text[exponentEnd] == QLatin1Char('+')))
            {
                ++exponentEnd;
            }
            if (isDigit(exponentEnd) == true)
            {
                while (isDigit(exponentEnd) == true)
                {
                    ++exponentEnd;
                }
                end = exponentEnd;
            }
        }
        bool isNumber = false;
        outValue = Text.mid(begin, end - begin).toDouble(&isNumber);
        Position = end;
        return isNumber;
    }

    [[nodiscard]] bool TryReadNumbers(qreal* outValues, int count)
    {
        for (int index = 0; index < count; ++index)
        {
            if (TryReadNumber(outValues[index]) == false)
            {
                return false;
            }
        }
        return true;
    }

    /// An arc flag is one character, 0 or 1, and may be written without a separator after it.
    [[nodiscard]] bool TryReadFlag(bool& outValue)
    {
        SkipSeparators();
        if (Position >= Text.size() || (Text[Position] != QLatin1Char('0') && Text[Position] != QLatin1Char('1')))
        {
            return false;
        }
        outValue = Text[Position] == QLatin1Char('1');
        ++Position;
        return true;
    }
};

[[nodiscard]] qreal AngleBetween(qreal fromX, qreal fromY, qreal toX, qreal toY)
{
    const qreal dot = fromX * toX + fromY * toY;
    const qreal length = std::hypot(fromX, fromY) * std::hypot(toX, toY);
    const qreal clamped = std::clamp(dot / length, -1.0, 1.0);
    const qreal angle = std::acos(clamped);
    return fromX * toY - fromY * toX < 0.0 ? -angle : angle;
}

/// Append the SVG elliptical arc from the current point to target as cubic segments of at most a quarter turn.
/// Thanks to the W3C, "SVG 1.1, Implementation notes, F.6.5 and F.6.6", for the endpoint to centre conversion:
/// https://www.w3.org/TR/SVG11/implnote.html#ArcImplementationNotes
void AppendArc(QPainterPath& outPath,
               QPointF from,
               qreal radiusX,
               qreal radiusY,
               qreal rotationDegrees,
               bool isLarge,
               bool isSweep,
               QPointF to)
{
    radiusX = std::abs(radiusX);
    radiusY = std::abs(radiusY);
    if (from == to)
    {
        return;
    }
    if (radiusX == 0.0 || radiusY == 0.0)
    {
        outPath.lineTo(to);
        return;
    }
    const qreal rotation = qDegreesToRadians(rotationDegrees);
    const qreal cosine = std::cos(rotation);
    const qreal sine = std::sin(rotation);
    const qreal halfDeltaX = (from.x() - to.x()) * HALF;
    const qreal halfDeltaY = (from.y() - to.y()) * HALF;
    const qreal primeX = cosine * halfDeltaX + sine * halfDeltaY;
    const qreal primeY = -sine * halfDeltaX + cosine * halfDeltaY;
    const qreal scale = (primeX * primeX) / (radiusX * radiusX) + (primeY * primeY) / (radiusY * radiusY);
    if (scale > 1.0)
    {
        radiusX *= std::sqrt(scale);
        radiusY *= std::sqrt(scale);
    }
    const qreal radiusXSquared = radiusX * radiusX;
    const qreal radiusYSquared = radiusY * radiusY;
    const qreal denominator = radiusXSquared * primeY * primeY + radiusYSquared * primeX * primeX;
    const qreal numerator = radiusXSquared * radiusYSquared - denominator;
    const qreal sign = isLarge == isSweep ? -1.0 : 1.0;
    const qreal coefficient = sign * std::sqrt(std::max(0.0, numerator / denominator));
    const qreal centerPrimeX = coefficient * radiusX * primeY / radiusY;
    const qreal centerPrimeY = -coefficient * radiusY * primeX / radiusX;
    const qreal centerX = cosine * centerPrimeX - sine * centerPrimeY + (from.x() + to.x()) * HALF;
    const qreal centerY = sine * centerPrimeX + cosine * centerPrimeY + (from.y() + to.y()) * HALF;
    const qreal startAngle =
        AngleBetween(1.0, 0.0, (primeX - centerPrimeX) / radiusX, (primeY - centerPrimeY) / radiusY);
    qreal sweepAngle = AngleBetween((primeX - centerPrimeX) / radiusX,
                                    (primeY - centerPrimeY) / radiusY,
                                    (-primeX - centerPrimeX) / radiusX,
                                    (-primeY - centerPrimeY) / radiusY);
    if (isSweep == false && sweepAngle > 0.0)
    {
        sweepAngle -= FULL_TURN;
    }
    else if (isSweep == true && sweepAngle < 0.0)
    {
        sweepAngle += FULL_TURN;
    }
    const int segmentCount =
        std::max(1, static_cast<int>(std::ceil(std::abs(sweepAngle) / QUARTER_TURN - ARC_SEGMENT_TOLERANCE)));
    const qreal segmentAngle = sweepAngle / static_cast<qreal>(segmentCount);
    const qreal handle = BEZIER_ARC_NUMERATOR / BEZIER_ARC_DENOMINATOR * std::tan(segmentAngle * QUARTER);
    const auto pointAt = [&](qreal angle) {
        const qreal x = radiusX * std::cos(angle);
        const qreal y = radiusY * std::sin(angle);
        return QPointF(centerX + cosine * x - sine * y, centerY + sine * x + cosine * y);
    };
    const auto tangentAt = [&](qreal angle) {
        const qreal x = -radiusX * std::sin(angle);
        const qreal y = radiusY * std::cos(angle);
        return QPointF(cosine * x - sine * y, sine * x + cosine * y);
    };
    for (int segment = 0; segment < segmentCount; ++segment)
    {
        const qreal begin = startAngle + segmentAngle * static_cast<qreal>(segment);
        const qreal end = begin + segmentAngle;
        const QPointF endPoint = segment + 1 == segmentCount ? to : pointAt(end);
        outPath.cubicTo(pointAt(begin) + tangentAt(begin) * handle, endPoint - tangentAt(end) * handle, endPoint);
    }
}

[[nodiscard]] bool IsCubicCommand(QChar command)
{
    return command == QLatin1Char('C') || command == QLatin1Char('S');
}

[[nodiscard]] bool IsQuadraticCommand(QChar command)
{
    return command == QLatin1Char('Q') || command == QLatin1Char('T');
}

/// Cache of rasterised icons, shared by every icon.
struct CacheKey final
{
    int Icon;
    int Width;
    int Height;
    int Mode;
    int ScaleHundredths;
    QRgb Colour;

    [[nodiscard]] bool operator<(const CacheKey& other) const noexcept
    {
        return std::tie(Icon, Width, Height, Mode, ScaleHundredths, Colour) <
               std::tie(other.Icon, other.Width, other.Height, other.Mode, other.ScaleHundredths, other.Colour);
    }
};

constexpr int PERCENT = 100;

QMap<CacheKey, QPixmap>& GetCache()
{
    static QMap<CacheKey, QPixmap> cache;
    return cache;
}

[[nodiscard]] QColor GetIconColour(QIcon::Mode mode)
{
    const QPalette palette = QGuiApplication::palette();
    return mode == QIcon::Disabled ? palette.color(QPalette::Disabled, QPalette::ButtonText)
                                   : palette.color(QPalette::Active, QPalette::ButtonText);
}

[[nodiscard]] QSize GetLogicalIconSize(const QSize& size, qreal scale)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    (void)scale;
    return size;
#else
    const qreal safeScale = scale < 1.0 ? 1.0 : scale;
    return QSize(static_cast<int>(std::lround(static_cast<qreal>(size.width()) / safeScale)),
                 static_cast<int>(std::lround(static_cast<qreal>(size.height()) / safeScale)));
#endif
}

[[nodiscard]] QPixmap Rasterise(EditorIcon icon, const QSize& deviceSize, qreal scale, const QColor& colour)
{
    QPixmap pixmap(deviceSize);
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(scale);
    const QPainterPath& outline = GetEditorIconOutline(icon);
    if (outline.isEmpty() == true || deviceSize.isEmpty() == true)
    {
        return pixmap;
    }
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal logicalWidth = static_cast<qreal>(deviceSize.width()) / scale;
    const qreal logicalHeight = static_cast<qreal>(deviceSize.height()) / scale;
    painter.scale(logicalWidth / EDITOR_ICON_GRID, logicalHeight / EDITOR_ICON_GRID);
    QPen pen(colour, EDITOR_ICON_STROKE_WIDTH, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(ICON_DESCRIPTORS[static_cast<foundation::usize>(icon)].IsFilled == true ? QBrush(colour)
                                                                                             : QBrush());
    painter.drawPath(outline);
    return pixmap;
}

[[nodiscard]] QPixmap GetCachedPixmap(EditorIcon icon, const QSize& logicalSize, QIcon::Mode mode, qreal scale)
{
    const qreal safeScale = scale < 1.0 ? 1.0 : scale;
    const QColor colour = GetIconColour(mode);
    const QSize deviceSize(static_cast<int>(std::lround(static_cast<qreal>(logicalSize.width()) * safeScale)),
                           static_cast<int>(std::lround(static_cast<qreal>(logicalSize.height()) * safeScale)));
    const CacheKey key{static_cast<int>(icon),
                       deviceSize.width(),
                       deviceSize.height(),
                       mode == QIcon::Disabled ? 1 : 0,
                       static_cast<int>(std::lround(safeScale * static_cast<qreal>(PERCENT))),
                       colour.rgba()};
    QMap<CacheKey, QPixmap>& cache = GetCache();
    const QMap<CacheKey, QPixmap>::const_iterator found = cache.constFind(key);
    if (found != cache.constEnd())
    {
        return found.value();
    }
    if (cache.size() >= GetEditorIconCacheLimit())
    {
        cache.clear();
    }
    const QPixmap pixmap = Rasterise(icon, deviceSize, safeScale, colour);
    cache.insert(key, pixmap);
    return pixmap;
}

class EditorIconEngine final : public QIconEngine
{
public:
    explicit EditorIconEngine(EditorIcon icon) : mIcon(icon) {}

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override
    {
        Q_UNUSED(state);
        const qreal scale = painter->device() != nullptr ? painter->device()->devicePixelRatio() : 1.0;
        painter->drawPixmap(rect, GetCachedPixmap(mIcon, rect.size(), mode, scale));
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        Q_UNUSED(state);
        return GetCachedPixmap(mIcon, GetLogicalIconSize(size, scale), mode, scale);
    }

    QSize actualSize(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        Q_UNUSED(mode);
        Q_UNUSED(state);
        return size;
    }

    QIconEngine* clone() const override
    {
        return new EditorIconEngine(mIcon);
    }

private:
    EditorIcon mIcon;
};
} // namespace

bool TryParseIconPathData(QStringView pathData, QPainterPath& outPath)
{
    outPath = QPainterPath();
    PathReader reader{pathData, 0};
    QPainterPath path;
    QPointF current;
    QPointF subpathStart;
    QPointF lastControl;
    QChar command;
    QChar previousUpper;
    bool isRepeat = false;
    while (reader.IsDone() == false)
    {
        if (reader.IsAtCommand() == true)
        {
            command = reader.ReadCommand();
            isRepeat = false;
        }
        else if (command.isNull() == true || command.toUpper() == QLatin1Char('Z'))
        {
            return false;
        }
        else
        {
            isRepeat = true;
        }
        const bool isRelative = command.isLower();
        const QChar upper = command.toUpper();
        const QPointF origin = isRelative == true ? current : QPointF();
        qreal values[MAXIMUM_COMMAND_VALUES] = {};
        if (upper == QLatin1Char('Z'))
        {
            path.closeSubpath();
            current = subpathStart;
        }
        else if (upper == QLatin1Char('M') || upper == QLatin1Char('L') || upper == QLatin1Char('T'))
        {
            if (reader.TryReadNumbers(values, 2) == false)
            {
                return false;
            }
            const QPointF point = origin + QPointF(values[0], values[1]);
            if (upper == QLatin1Char('M') && isRepeat == false)
            {
                path.moveTo(point);
                subpathStart = point;
            }
            else if (upper == QLatin1Char('T'))
            {
                const QPointF control =
                    IsQuadraticCommand(previousUpper) == true ? current * 2.0 - lastControl : current;
                path.quadTo(control, point);
                lastControl = control;
            }
            else
            {
                path.lineTo(point);
            }
            current = point;
        }
        else if (upper == QLatin1Char('H') || upper == QLatin1Char('V'))
        {
            if (reader.TryReadNumbers(values, 1) == false)
            {
                return false;
            }
            const bool isHorizontal = upper == QLatin1Char('H');
            const qreal base = isRelative == true ? (isHorizontal == true ? current.x() : current.y()) : 0.0;
            current =
                isHorizontal == true ? QPointF(base + values[0], current.y()) : QPointF(current.x(), base + values[0]);
            path.lineTo(current);
        }
        else if (upper == QLatin1Char('C') || upper == QLatin1Char('S'))
        {
            const int count = upper == QLatin1Char('C') ? CUBIC_VALUE_COUNT : SMOOTH_CUBIC_VALUE_COUNT;
            if (reader.TryReadNumbers(values, count) == false)
            {
                return false;
            }
            QPointF first = current;
            int offset = 0;
            if (upper == QLatin1Char('C'))
            {
                first = origin + QPointF(values[0], values[1]);
                offset = 2;
            }
            else if (IsCubicCommand(previousUpper) == true)
            {
                first = current * 2.0 - lastControl;
            }
            const QPointF second = origin + QPointF(values[offset], values[offset + 1]);
            const QPointF point = origin + QPointF(values[offset + 2], values[offset + 3]);
            path.cubicTo(first, second, point);
            lastControl = second;
            current = point;
        }
        else if (upper == QLatin1Char('Q'))
        {
            if (reader.TryReadNumbers(values, QUADRATIC_VALUE_COUNT) == false)
            {
                return false;
            }
            const QPointF control = origin + QPointF(values[0], values[1]);
            const QPointF point = origin + QPointF(values[2], values[3]);
            path.quadTo(control, point);
            lastControl = control;
            current = point;
        }
        else if (upper == QLatin1Char('A'))
        {
            bool isLargeArc = false;
            bool isSweepArc = false;
            if (reader.TryReadNumbers(values, ARC_LEADING_VALUE_COUNT) == false ||
                reader.TryReadFlag(isLargeArc) == false || reader.TryReadFlag(isSweepArc) == false ||
                reader.TryReadNumbers(values + ARC_LEADING_VALUE_COUNT, 2) == false)
            {
                return false;
            }
            const QPointF point =
                origin + QPointF(values[ARC_LEADING_VALUE_COUNT], values[ARC_LEADING_VALUE_COUNT + 1]);
            AppendArc(path, current, values[0], values[1], values[2], isLargeArc, isSweepArc, point);
            current = point;
        }
        else
        {
            return false;
        }
        previousUpper = upper;
    }
    outPath = path;
    return path.isEmpty() == false;
}

bool TryParseIconSvg(const QByteArray& document, QPainterPath& outPath)
{
    outPath = QPainterPath();
    QXmlStreamReader xml(document);
    QPainterPath combined;
    bool hasRoot = false;
    while (xml.readNext() != QXmlStreamReader::EndDocument && xml.hasError() == false)
    {
        if (xml.isStartElement() == false)
        {
            continue;
        }
        const QXmlStreamAttributes attributes = xml.attributes();
        const auto number = [&attributes](const char* name, qreal fallback) {
            const QString text = attributes.value(QLatin1String(name)).toString();
            bool isNumber = false;
            const qreal value = text.toDouble(&isNumber);
            return isNumber == true ? value : fallback;
        };
        const QStringView name = xml.name();
        if (name == QLatin1String("svg"))
        {
            hasRoot = attributes.value(QLatin1String("viewBox")) == QLatin1String("0 0 24 24");
        }
        else if (name == QLatin1String("path"))
        {
            QPainterPath shape;
            if (TryParseIconPathData(attributes.value(QLatin1String("d")), shape) == false)
            {
                return false;
            }
            combined.addPath(shape);
        }
        else if (name == QLatin1String("circle"))
        {
            const qreal radius = number("r", 0.0);
            combined.addEllipse(QPointF(number("cx", 0.0), number("cy", 0.0)), radius, radius);
        }
        else if (name == QLatin1String("rect"))
        {
            const qreal radiusX = number("rx", number("ry", 0.0));
            const qreal radiusY = number("ry", radiusX);
            combined.addRoundedRect(
                QRectF(number("x", 0.0), number("y", 0.0), number("width", 0.0), number("height", 0.0)),
                radiusX,
                radiusY);
        }
        else
        {
            return false;
        }
    }
    if (xml.hasError() == true || hasRoot == false || combined.isEmpty() == true)
    {
        return false;
    }
    outPath = combined;
    return true;
}

const QPainterPath& GetEditorIconOutline(EditorIcon icon)
{
    static QList<QPainterPath> outlines;
    static QList<bool> wasLoaded;
    if (outlines.isEmpty() == true)
    {
        outlines.resize(static_cast<qsizetype>(EDITOR_ICON_COUNT));
        wasLoaded.resize(static_cast<qsizetype>(EDITOR_ICON_COUNT));
    }
    const qsizetype index = static_cast<qsizetype>(icon);
    if (wasLoaded[index] == false)
    {
        wasLoaded[index] = true;
        QFile file(QStringLiteral(":/icons/") + QLatin1String(ICON_DESCRIPTORS[index].FileName) +
                   QStringLiteral(".svg"));
        if (file.open(QIODevice::ReadOnly) == true)
        {
            QPainterPath outline;
            if (TryParseIconSvg(file.readAll(), outline) == true)
            {
                outlines[index] = outline;
            }
        }
    }
    return outlines[index];
}

QIcon GetEditorIcon(EditorIcon icon)
{
    static QList<QIcon> icons;
    if (icons.isEmpty() == true)
    {
        icons.resize(static_cast<qsizetype>(EDITOR_ICON_COUNT));
    }
    QIcon& cached = icons[static_cast<qsizetype>(icon)];
    if (cached.isNull() == true)
    {
        cached = QIcon(new EditorIconEngine(icon));
    }
    return cached;
}

int GetEditorIconCacheCount() noexcept
{
    return static_cast<int>(GetCache().size());
}

int GetEditorIconCacheLimit() noexcept
{
    return static_cast<int>(EDITOR_ICON_COUNT) * CACHE_VARIANTS_PER_ICON;
}

void ClearEditorIconCache()
{
    GetCache().clear();
}
} // namespace ludus::editor
