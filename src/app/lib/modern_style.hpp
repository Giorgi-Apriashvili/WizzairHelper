#pragma once
// The application's widget style (DECISIONS D-53): a QProxyStyle over Fusion that
// paints the controls the app uses in one calm, flat look with the colours of
// style::theme():
//   * inputs (line edits, combo boxes, spin boxes, date pickers): rounded 5 px
//     fields, a subtle frame with a darker bottom stroke, a 2 px accent ring
//     while focused, chevrons instead of bevelled arrow buttons;
//   * push buttons: rounded neutral buttons; a dialog's default button is the
//     accent (primary) button; flat buttons only show a fill under the mouse;
//   * check boxes: rounded boxes, accent fill and a white tick when checked;
//   * tabs: text tabs with an accent underline on the selected one, the pane is
//     a rounded card;
//   * table headers: flat, semibold secondary text, a short divider between
//     sections, a hover fill, no bevels;
//   * scroll bars: thin rounded thumbs without arrow buttons;
//   * popups (combo lists, the place completer, the calendar, menus, tooltips):
//     the surface colour with a 1 px border; a selected list or menu item is the
//     selection tint with a 3 px accent pill at its left, like a focused grid row;
//     a menu's default action is bold;
//   * focus: a visible accent ring on buttons, check boxes, inputs, tabs and
//     calendar days;
//   * mnemonic underlines: shown while Alt is held, or always when Windows'
//     "Underline access keys" setting is on (SPI_GETKEYBOARDCUES), as on Windows.
// Everything not listed is Fusion with the theme palette. No style sheet is
// used: QSS needs image files for arrows and slows down large item views. High
// Contrast does not use this style (ui_style.hpp).

#include <QPointer>
#include <QProxyStyle>
#include <QWidget>

#include <functional>

class QComboBox;
class QFontMetrics;
class QStyleOptionMenuItem;

namespace wizz::app {

class ModernStyle final : public QProxyStyle {
public:
    /// Takes ownership of `base` (Fusion).
    explicit ModernStyle(QStyle* base);

    /// Whether the application's style is a ModernStyle (not High Contrast's).
    [[nodiscard]] static bool is_active();
    /// The height of every input and push button for `metrics` (the UI font): max(26, its
    /// line_height() + 10).
    [[nodiscard]] static int control_height(const QFontMetrics& metrics);
    /// The height of one line of text in `metrics`' font as Qt lays it out (QLabel, QComboBox,
    /// headers): the ascent plus the descent, rounded up. Without hinting (D-58) that can be a
    /// pixel more than QFontMetrics::height().
    [[nodiscard]] static int line_height(const QFontMetrics& metrics);
    /// The font a menu item is measured and drawn in: its option's font, bold for the menu's
    /// default action, with the weight axis of the face that picks (style::fit_text_rendering).
    [[nodiscard]] static QFont menu_item_font(const QStyleOptionMenuItem& item);
    /// Corner radius of inputs and buttons.
    static constexpr qreal kRadius = 5.0;
    /// Corner radius of cards (input sections, the tab pane).
    static constexpr qreal kCardRadius = 8.0;
    /// Width of the accent bar that marks a selected item of a focused list or grid.
    static constexpr int kSelectionBarWidth = 3;

    /// Draws a combo box's drop-down items with QStyledItemDelegate, so they get the
    /// list look (hover, tint and accent pill) instead of the plain QItemDelegate fill.
    static void use_styled_popup(QComboBox& combo);
    /// A widget that fills itself with the theme's surface colour at paint time (the
    /// corner where two scroll bars meet).
    [[nodiscard]] static QWidget* make_surface_corner(QWidget* parent);

    /// Tells whether Windows' "Underline access keys" setting is on; the default asks
    /// SystemParametersInfoW(SPI_GETKEYBOARDCUES). Replaces the probe (empty = the default)
    /// and returns the previous one (tests).
    using KeyboardCuesProbe = std::function<bool()>;
    static KeyboardCuesProbe set_keyboard_cues_probe(KeyboardCuesProbe probe);

    using QProxyStyle::polish;
    using QProxyStyle::unpolish;
    void polish(QWidget* widget) override;
    void unpolish(QWidget* widget) override;
    /// Watches the application's Alt key for the mnemonic underlines.
    void polish(QApplication* application) override;
    void unpolish(QApplication* application) override;

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
    [[nodiscard]] QIcon standardIcon(StandardPixmap standard_icon, const QStyleOption* option = nullptr,
                                     const QWidget* widget = nullptr) const override;
    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget = nullptr) const override;
    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                     const QWidget* widget = nullptr) const override;
    void drawComplexControl(ComplexControl control, const QStyleOptionComplex* option, QPainter* painter,
                            const QWidget* widget = nullptr) const override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// Shows the mnemonic underlines of `window` (Alt pressed) or hides them again (null).
    void set_alt_window(QWidget* window);

    QPointer<QWidget> alt_window_;  ///< the window whose Alt key is held
};

} // namespace wizz::app
