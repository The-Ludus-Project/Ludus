#pragma once

#include <QPalette>
#include <QProxyStyle>

namespace ludus::editor
{
/// Dynamic property naming the padding, in logical pixels, an item view adds around each row.
inline constexpr const char* ITEM_PADDING_PROPERTY = "itemPadding";
/// Row padding of the Welcome recent-projects list.
inline constexpr int RECENT_PROJECT_ITEM_PADDING = 8;

/// The editor palette: the platform palette with every role the widgets read taken from the design tokens, so the
/// style needs no per-widget colour rules. Appearance and contrast pick the token set.
[[nodiscard]] QPalette MakeEditorPalette(const QPalette& platform, bool isDark, bool isHighContrast);

/// Control geometry and state painting for every widget, over the platform style: flat controls with a one pixel
/// edge, the accent for focus, hover and the primary action, and an accent underline on the selected tab. It replaces
/// the window-wide style sheet; colours come from the palette and the tokens, widths from the contrast preference.
class EditorStyle final : public QProxyStyle
{
public:
    EditorStyle();

    /// Re-read the appearance, tokens and theme preferences from the application palette. Call after either changes.
    void Refresh();

    void polish(QWidget* widget) override;
    void unpolish(QWidget* widget) override;
    void drawPrimitive(PrimitiveElement element,
                       const QStyleOption* option,
                       QPainter* painter,
                       const QWidget* widgetOrNull) const override;
    void drawControl(ControlElement element,
                     const QStyleOption* option,
                     QPainter* painter,
                     const QWidget* widgetOrNull) const override;
    void drawComplexControl(ComplexControl control,
                            const QStyleOptionComplex* option,
                            QPainter* painter,
                            const QWidget* widgetOrNull) const override;
    [[nodiscard]] QRect
    subElementRect(SubElement element, const QStyleOption* option, const QWidget* widgetOrNull) const override;
    [[nodiscard]] QSize sizeFromContents(ContentsType type,
                                         const QStyleOption* option,
                                         const QSize& contentsSize,
                                         const QWidget* widgetOrNull) const override;
    [[nodiscard]] int
    pixelMetric(PixelMetric metric, const QStyleOption* optionOrNull, const QWidget* widgetOrNull) const override;

    /// Width of a control edge in logical pixels; high contrast thickens it.
    [[nodiscard]] int GetEdgeWidth() const noexcept;
    /// Width of the focus ring in logical pixels; high contrast thickens it.
    [[nodiscard]] int GetFocusRingWidth() const noexcept;

private:
    struct Colours final
    {
        QColor Workspace;
        QColor Panel;
        QColor Raised;
        QColor Text;
        QColor Secondary;
        QColor Border;
        QColor SubtleBorder;
        QColor Accent;
        QColor AccentText;
        QColor Selected;
    };
    Colours mColours;
    bool mIsHighContrast = false;
};

/// Install an EditorStyle as the application style once, then refresh it from the current palette and preferences.
void InstallEditorStyle();

/// The installed editor style, or null before InstallEditorStyle runs.
[[nodiscard]] EditorStyle* GetEditorStyleOrNull();
} // namespace ludus::editor
