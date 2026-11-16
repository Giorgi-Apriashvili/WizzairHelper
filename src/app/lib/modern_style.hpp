#pragma once
// The application's widget style (prototype "layout" direction): a QProxyStyle
// over Fusion that paints the controls the app uses in one calm, flat look
// with the colours of style::theme():
//   * inputs (line edits, combo boxes, spin boxes, date pickers): rounded 5 px
//     fields, a subtle frame with a darker bottom stroke, a 2 px accent ring
//     while focused, chevrons instead of bevelled arrow buttons;
//   * push buttons: rounded neutral buttons; a dialog's default button is the
//     accent (primary) button; flat buttons only show a fill under the mouse;
//   * check boxes: rounded boxes, accent fill and a white tick when checked;
//   * tabs: text tabs with an accent underline on the selected one, the pane is
//     a rounded card;
//   * table headers: flat, semibold secondary text, a short divider between
//     sections, no bevels;
//   * scroll bars: thin rounded thumbs without arrow buttons;
//   * focus: a visible accent ring on buttons, check boxes and inputs.
// Everything not listed is Fusion with the theme palette. No style sheet is
// used: QSS needs image files for arrows and slows down large item views.

#include <QProxyStyle>

class QFontMetrics;

namespace wizz::app {

class ModernStyle final : public QProxyStyle {
public:
    /// Takes ownership of `base` (Fusion).
    explicit ModernStyle(QStyle* base);

    /// The height of every input and push button for `metrics` (the UI font).
    [[nodiscard]] static int control_height(const QFontMetrics& metrics);
    /// Corner radius of inputs and buttons.
    static constexpr qreal kRadius = 5.0;
    /// Corner radius of cards (input sections, the tab pane).
    static constexpr qreal kCardRadius = 8.0;

    using QProxyStyle::polish;
    using QProxyStyle::unpolish;
    void polish(QWidget* widget) override;
    void unpolish(QWidget* widget) override;

    [[nodiscard]] int pixelMetric(PixelMetric metric, const QStyleOption* option = nullptr,
                                  const QWidget* widget = nullptr) const override;
    [[nodiscard]] int styleHint(StyleHint hint, const QStyleOption* option = nullptr, const QWidget* widget = nullptr,
                                QStyleHintReturn* data = nullptr) const override;
    [[nodiscard]] QSize sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& size,
                                         const QWidget* widget) const override;
    [[nodiscard]] QRect subControlRect(ComplexControl control, const QStyleOptionComplex* option,
                                       SubControl sub_control, const QWidget* widget) const override;
    [[nodiscard]] QRect subElementRect(SubElement element, const QStyleOption* option,
                                       const QWidget* widget) const override;
    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget = nullptr) const override;
    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                     const QWidget* widget = nullptr) const override;
    void drawComplexControl(ComplexControl control, const QStyleOptionComplex* option, QPainter* painter,
                            const QWidget* widget = nullptr) const override;
};

} // namespace wizz::app
