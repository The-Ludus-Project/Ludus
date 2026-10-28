#include "internal/theme_fonts.h"
#include "design_tokens.h"

#include <QApplication>

namespace ludus::editor
{
double GetTypeScale(TypeRole role) noexcept
{
    switch (role)
    {
        case TypeRole::Caption:
            return design::TYPE_SCALE_CAPTION;
        case TypeRole::Title:
            return design::TYPE_SCALE_TITLE;
        case TypeRole::Heading:
            return design::TYPE_SCALE_HEADING;
        case TypeRole::Body:
            break;
    }
    return design::TYPE_SCALE_BODY;
}

QFont MakeTypeFont(const QFont& base, TypeRole role)
{
    QFont font = base;
    const double scale = GetTypeScale(role);
    if (base.pointSizeF() > 0)
    {
        font.setPointSizeF(base.pointSizeF() * scale);
    }
    else if (base.pixelSize() > 0)
    {
        font.setPixelSize(static_cast<int>(base.pixelSize() * scale + 0.5));
    }
    font.setBold(role == TypeRole::Title || role == TypeRole::Heading);
    return font;
}

QFont MakeTabularFont(const QFont& base)
{
    QFont font = base;
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    font.setFeature(QFont::Tag("tnum"), 1);
#endif
    return font;
}

void ApplyThemeFonts()
{
    QApplication::setFont(MakeTabularFont(QApplication::font("QAbstractSpinBox")), "QAbstractSpinBox");
}
} // namespace ludus::editor
