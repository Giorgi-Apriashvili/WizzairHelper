#include "modern_style.hpp"

#include "ui_style.hpp"

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QFontMetrics>
#include <QHeaderView>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollBar>
#include <QStyleOption>
#include <QTabBar>

#include <algorithm>
#include <climits>

namespace wizz::app {

namespace {

constexpr int kComboArrowWidth = 28;
constexpr int kSpinButtonsWidth = 20;
constexpr int kFieldTextPadding = 9;
constexpr int kButtonPadding = 16;
constexpr int kHeaderHPadding = 9;
constexpr int kHeaderVPadding = 6;
constexpr int kHeaderArrowSpace = 14;
constexpr int kIndicatorSize = 18;
constexpr int kScrollBarExtent = 12;

[[nodiscard]] const style::Theme& t()
{
    return style::theme();
}

/// Mix `a` and `b` (0 = a, 1 = b).
[[nodiscard]] QColor mix(const QColor& a, const QColor& b, qreal amount)
{
    const qreal keep = 1.0 - amount;
    return QColor::fromRgbF(static_cast<float>(a.redF() * keep + b.redF() * amount),
                            static_cast<float>(a.greenF() * keep + b.greenF() * amount),
                            static_cast<float>(a.blueF() * keep + b.blueF() * amount));
}

/// A chevron centred on `centre`, `half` px from the centre to either end.
void draw_chevron(QPainter* painter, QPointF centre, qreal half, Qt::ArrowType direction, const QColor& colour,
                  qreal width = 1.5)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(colour, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    const qreal rise = half / 2.0;
    QPolygonF points;
    switch (direction) {
    case Qt::UpArrow:
        points << QPointF(-half, rise) << QPointF(0, -rise) << QPointF(half, rise);
        break;
    case Qt::LeftArrow:
        points << QPointF(rise, -half) << QPointF(-rise, 0) << QPointF(rise, half);
        break;
    case Qt::RightArrow:
        points << QPointF(-rise, -half) << QPointF(rise, 0) << QPointF(-rise, half);
        break;
    default:
        points << QPointF(-half, -rise) << QPointF(0, rise) << QPointF(half, -rise);
        break;
    }
    points.translate(centre);
    painter->drawPolyline(points);
    painter->restore();
}

/// The rounded input field: background, frame, darker bottom stroke, focus ring.
void draw_field(QPainter* painter, const QRect& rect, const QStyleOption& option, bool focused)
{
    const bool enabled = option.state.testFlag(QStyle::State_Enabled);
    const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QRectF box = QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5);
    painter->setPen(Qt::NoPen);
    painter->setBrush(enabled ? t().input : t().input_disabled);
    painter->drawRoundedRect(box, ModernStyle::kRadius, ModernStyle::kRadius);

    painter->setBrush(Qt::NoBrush);
    if (focused && enabled) {
        painter->setPen(QPen(t().accent_line, 2.0));
        painter->drawRoundedRect(QRectF(rect).adjusted(1, 1, -1, -1), ModernStyle::kRadius - 0.5,
                                 ModernStyle::kRadius - 0.5);
    } else {
        const QColor frame = !enabled ? t().border : hovered ? t().input_border_hover : t().input_border;
        painter->setPen(QPen(frame, 1.0));
        painter->drawRoundedRect(box, ModernStyle::kRadius, ModernStyle::kRadius);
        if (enabled) {
            // The bottom stroke carries the 3:1 contrast of the field boundary.
            painter->setClipRect(QRectF(box.left(), box.bottom() - 1.0, box.width(), 2.0));
            painter->setPen(QPen(hovered ? t().input_border_hover.darker(115) : t().input_underline, 1.0));
            painter->drawRoundedRect(box, ModernStyle::kRadius, ModernStyle::kRadius);
        }
    }
    painter->restore();
}

[[nodiscard]] bool is_primary(const QStyleOptionButton& button)
{
    return button.features.testFlag(QStyleOptionButton::DefaultButton);
}

/// The rounded push-button body.
void draw_button_panel(QPainter* painter, const QStyleOptionButton& button)
{
    const bool enabled = button.state.testFlag(QStyle::State_Enabled);
    const bool pressed = button.state.testAnyFlags(QStyle::State_Sunken | QStyle::State_On);
    const bool hovered = button.state.testFlag(QStyle::State_MouseOver);
    const bool flat = button.features.testFlag(QStyleOptionButton::Flat);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QRectF box = QRectF(button.rect).adjusted(0.5, 0.5, -0.5, -0.5);
    if (is_primary(button) && !flat) {
        QColor fill = pressed ? t().accent_pressed : hovered ? t().accent_hover : t().accent;
        if (!enabled) {
            fill = mix(t().surface, t().accent, 0.45);
        }
        painter->setPen(Qt::NoPen);
        painter->setBrush(fill);
        painter->drawRoundedRect(box, ModernStyle::kRadius, ModernStyle::kRadius);
    } else if (flat) {
        if (enabled && (hovered || pressed)) {
            painter->setPen(Qt::NoPen);
            QColor fill = t().text;
            fill.setAlphaF(pressed ? 0.12F : 0.07F);
            painter->setBrush(fill);
            painter->drawRoundedRect(box, ModernStyle::kRadius, ModernStyle::kRadius);
        }
    } else {
        const QColor fill = !enabled ? t().input_disabled
                            : pressed ? t().button_pressed
                            : hovered ? t().button_hover
                                      : t().button;
        painter->setPen(QPen(enabled && hovered ? t().input_border_hover : t().input_border, 1.0));
        painter->setBrush(fill);
        painter->drawRoundedRect(box, ModernStyle::kRadius, ModernStyle::kRadius);
    }
    painter->restore();
}

/// The keyboard focus ring: 2 px accent inside `rect`.
void draw_focus_ring(QPainter* painter, const QRect& rect, const QColor& colour, qreal radius)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(colour, 2.0));
    painter->drawRoundedRect(QRectF(rect).adjusted(1, 1, -1, -1), radius, radius);
    painter->restore();
}

[[nodiscard]] bool parent_is_spin_box(const QWidget* widget)
{
    return widget != nullptr && qobject_cast<const QAbstractSpinBox*>(widget->parentWidget()) != nullptr;
}

[[nodiscard]] bool parent_is_combo_box(const QWidget* widget)
{
    return widget != nullptr && qobject_cast<const QComboBox*>(widget->parentWidget()) != nullptr;
}

[[nodiscard]] bool in_item_view(const QWidget* widget)
{
    for (const QWidget* w = widget; w != nullptr; w = w->parentWidget()) {
        if (qobject_cast<const QAbstractItemView*>(w) != nullptr) {
            return true;
        }
        if (w->isWindow()) {
            break;
        }
    }
    return false;
}

} // namespace

ModernStyle::ModernStyle(QStyle* base) : QProxyStyle(base) {}

int ModernStyle::control_height(const QFontMetrics& metrics)
{
    return std::max(26, metrics.height() + 10);
}

void ModernStyle::polish(QWidget* widget)
{
    QProxyStyle::polish(widget);
    if (qobject_cast<QAbstractButton*>(widget) != nullptr || qobject_cast<QComboBox*>(widget) != nullptr ||
        qobject_cast<QAbstractSpinBox*>(widget) != nullptr || qobject_cast<QTabBar*>(widget) != nullptr ||
        qobject_cast<QScrollBar*>(widget) != nullptr || qobject_cast<QLineEdit*>(widget) != nullptr ||
        qobject_cast<QHeaderView*>(widget) != nullptr) {
        widget->setAttribute(Qt::WA_Hover);
    }
    if (auto* area = qobject_cast<QAbstractScrollArea*>(widget);
        area != nullptr && area->cornerWidget() == nullptr && qobject_cast<QAbstractItemView*>(widget) != nullptr) {
        // Where the two scroll bars meet: the surface, not a grey square.
        auto* corner = new QWidget(area);
        corner->setAutoFillBackground(true);
        QPalette colours = corner->palette();
        colours.setColor(QPalette::Window, t().surface);
        corner->setPalette(colours);
        area->setCornerWidget(corner);
    }
}

void ModernStyle::unpolish(QWidget* widget)
{
    QProxyStyle::unpolish(widget);
}

int ModernStyle::pixelMetric(PixelMetric metric, const QStyleOption* option, const QWidget* widget) const
{
    switch (metric) {
    case PM_ScrollBarExtent:
        return kScrollBarExtent;
    case PM_ScrollBarSliderMin:
        return 32;
    case PM_TabBarTabHSpace:
        return 28;
    case PM_TabBarTabVSpace:
        return 14;
    case PM_TabBarBaseOverlap:
    case PM_TabBarBaseHeight:
    case PM_TabBarTabShiftHorizontal:
    case PM_TabBarTabShiftVertical:
        return 0;
    case PM_TabBarTabOverlap:
        return 0;
    case PM_HeaderMargin:
        return kHeaderVPadding;
    case PM_IndicatorWidth:
    case PM_IndicatorHeight:
    case PM_ExclusiveIndicatorWidth:
    case PM_ExclusiveIndicatorHeight:
        return kIndicatorSize;
    case PM_CheckBoxLabelSpacing:
        return 8;
    case PM_ButtonDefaultIndicator:
    case PM_ButtonShiftHorizontal:
    case PM_ButtonShiftVertical:
        return 0;
    case PM_ComboBoxFrameWidth:
    case PM_SpinBoxFrameWidth:
        return 2;
    case PM_FocusFrameHMargin:
        if (in_item_view(widget)) {
            return 7;  // grid cell text 8 px from the cell edges
        }
        break;
    case PM_ToolTipLabelFrameWidth:
        return 6;
    case PM_MenuHMargin:
    case PM_MenuVMargin:
        return 4;
    default:
        break;
    }
    return QProxyStyle::pixelMetric(metric, option, widget);
}

int ModernStyle::styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget,
                           QStyleHintReturn* data) const
{
    switch (hint) {
    case SH_ComboBox_Popup:
        return 0;  // a list under the field, not a menu over it
    case SH_ItemView_ShowDecorationSelected:
        return 1;
    case SH_Table_GridLineColor:
        return static_cast<int>(t().row_separator.rgb());
    case SH_ToolTipLabel_Opacity:
        return 255;
    case SH_SpinBox_ButtonsInsideFrame:
        return 1;
    case SH_Widget_Animation_Duration:
        return 0;  // nothing moves unless the app moves it (D-34)
    case SH_DialogButtonBox_ButtonsHaveIcons:
        return 0;
    default:
        break;
    }
    return QProxyStyle::styleHint(hint, option, widget, data);
}

QSize ModernStyle::sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& size,
                                    const QWidget* widget) const
{
    switch (type) {
    case CT_LineEdit:
        if (parent_is_spin_box(widget) || parent_is_combo_box(widget)) {
            break;
        }
        return QSize(size.width() + 2 * kFieldTextPadding, control_height(option->fontMetrics));
    case CT_ComboBox: {
        QSize result = QProxyStyle::sizeFromContents(type, option, size, widget);
        result.setWidth(size.width() + kFieldTextPadding + kComboArrowWidth + 4);
        result.setHeight(std::max(result.height(), control_height(option->fontMetrics)));
        return result;
    }
    case CT_SpinBox: {
        QSize result = QProxyStyle::sizeFromContents(type, option, size, widget);
        result.setHeight(std::max(result.height(), control_height(option->fontMetrics)));
        return result;
    }
    case CT_PushButton: {
        const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option);
        if (button == nullptr) {
            break;
        }
        const QSize text = option->fontMetrics.size(Qt::TextShowMnemonic, button->text);
        int width = text.width() + 2 * kButtonPadding;
        if (!button->icon.isNull()) {
            width += button->iconSize.width() + 6;
        }
        const bool flat = button->features.testFlag(QStyleOptionButton::Flat);
        return QSize(flat ? width : std::max(width, 88), control_height(option->fontMetrics));
    }
    case CT_HeaderSection: {
        const auto* header = qstyleoption_cast<const QStyleOptionHeader*>(option);
        if (header == nullptr) {
            break;
        }
        const QSize text = header->fontMetrics.size(0, header->text);
        int width = text.width() + 2 * kHeaderHPadding;
        if (header->sortIndicator != QStyleOptionHeader::None) {
            width += kHeaderArrowSpace;
        }
        return QSize(width, text.height() + 2 * kHeaderVPadding);
    }
    case CT_CheckBox: {
        const QSize result = QProxyStyle::sizeFromContents(type, option, size, widget);
        return QSize(result.width(), std::max(result.height(), kIndicatorSize + 4));
    }
    default:
        break;
    }
    return QProxyStyle::sizeFromContents(type, option, size, widget);
}

QRect ModernStyle::subControlRect(ComplexControl control, const QStyleOptionComplex* option, SubControl sub_control,
                                  const QWidget* widget) const
{
    const QRect r = option->rect;
    switch (control) {
    case CC_ComboBox:
        switch (sub_control) {
        case SC_ComboBoxFrame:
        case SC_ComboBoxListBoxPopup:
            return r;
        case SC_ComboBoxArrow:
            return visualRect(option->direction, r,
                              QRect(r.right() - kComboArrowWidth + 1, r.top(), kComboArrowWidth, r.height()));
        case SC_ComboBoxEditField:
            return visualRect(option->direction, r,
                              QRect(r.left() + kFieldTextPadding, r.top() + 3,
                                    r.width() - kFieldTextPadding - kComboArrowWidth, r.height() - 6));
        default:
            break;
        }
        break;
    case CC_SpinBox: {
        const auto* spin = qstyleoption_cast<const QStyleOptionSpinBox*>(option);
        const bool buttons = spin == nullptr || spin->buttonSymbols != QAbstractSpinBox::NoButtons;
        const int buttons_width = buttons ? kSpinButtonsWidth : 0;
        const QRect buttons_rect(r.right() - buttons_width - 1, r.top() + 3, buttons_width, r.height() - 6);
        switch (sub_control) {
        case SC_SpinBoxFrame:
            return r;
        case SC_SpinBoxUp:
            return buttons ? visualRect(option->direction, r,
                                        QRect(buttons_rect.left(), buttons_rect.top(), buttons_rect.width(),
                                              buttons_rect.height() / 2))
                           : QRect();
        case SC_SpinBoxDown:
            return buttons ? visualRect(option->direction, r,
                                        QRect(buttons_rect.left(), buttons_rect.top() + buttons_rect.height() / 2,
                                              buttons_rect.width(), buttons_rect.height() - buttons_rect.height() / 2))
                           : QRect();
        case SC_SpinBoxEditField:
            return visualRect(option->direction, r,
                              QRect(r.left() + 8, r.top() + 3, r.width() - 8 - buttons_width - 4, r.height() - 6));
        default:
            break;
        }
        break;
    }
    case CC_ScrollBar: {
        const auto* bar = qstyleoption_cast<const QStyleOptionSlider*>(option);
        if (bar == nullptr) {
            break;
        }
        const bool horizontal = bar->orientation == Qt::Horizontal;
        const int length = horizontal ? r.width() : r.height();
        int slider_length = length;
        if (bar->maximum != bar->minimum) {
            const qint64 range = qint64{bar->maximum} - bar->minimum;
            slider_length = static_cast<int>((qint64{bar->pageStep} * length) / (range + bar->pageStep));
            const int minimum = proxy()->pixelMetric(PM_ScrollBarSliderMin, bar, widget);
            if (slider_length < minimum || range > INT_MAX / 2) {
                slider_length = minimum;
            }
            slider_length = std::min(slider_length, length);
        }
        const int start = sliderPositionFromValue(bar->minimum, bar->maximum, bar->sliderPosition,
                                                  length - slider_length, bar->upsideDown);
        const auto along = [&](int from, int extent) {
            return horizontal ? QRect(r.left() + from, r.top(), extent, r.height())
                              : QRect(r.left(), r.top() + from, r.width(), extent);
        };
        switch (sub_control) {
        case SC_ScrollBarAddLine:
        case SC_ScrollBarSubLine:
        case SC_ScrollBarFirst:
        case SC_ScrollBarLast:
            return {};
        case SC_ScrollBarGroove:
            return r;
        case SC_ScrollBarSlider:
            return visualRect(option->direction, r, along(start, slider_length));
        case SC_ScrollBarSubPage:
            return visualRect(option->direction, r, along(0, start));
        case SC_ScrollBarAddPage:
            return visualRect(option->direction, r, along(start + slider_length, length - start - slider_length));
        default:
            break;
        }
        break;
    }
    default:
        break;
    }
    return QProxyStyle::subControlRect(control, option, sub_control, widget);
}

QRect ModernStyle::subElementRect(SubElement element, const QStyleOption* option, const QWidget* widget) const
{
    switch (element) {
    case SE_PushButtonFocusRect:
        return option->rect;
    case SE_LineEditContents:
        if (const auto* frame = qstyleoption_cast<const QStyleOptionFrame*>(option);
            frame != nullptr && frame->lineWidth > 0) {
            return option->rect.adjusted(kFieldTextPadding, 2, -kFieldTextPadding, -2);
        }
        break;
    case SE_HeaderLabel: {
        QRect label = option->rect.adjusted(kHeaderHPadding, kHeaderVPadding, -kHeaderHPadding, -kHeaderVPadding);
        if (const auto* header = qstyleoption_cast<const QStyleOptionHeader*>(option);
            header != nullptr && header->sortIndicator != QStyleOptionHeader::None) {
            label.setRight(label.right() - kHeaderArrowSpace);
        }
        return label;
    }
    case SE_HeaderArrow: {
        const QRect r = option->rect;
        return QRect(r.right() - kHeaderHPadding - 10 + 1, r.center().y() - 5, 10, 10);
    }
    case SE_TabWidgetTabContents: {
        // Inside the pane's 1 px frame; the bottom keeps clear of the rounded corners.
        const QRect pane = proxy()->subElementRect(SE_TabWidgetTabPane, option, widget);
        return pane.adjusted(1, 1, -1, -6);
    }
    default:
        break;
    }
    return QProxyStyle::subElementRect(element, option, widget);
}

void ModernStyle::drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                                const QWidget* widget) const
{
    switch (element) {
    case PE_PanelLineEdit: {
        if (parent_is_spin_box(widget)) {
            return;  // the spin box paints the field
        }
        if (parent_is_combo_box(widget)) {
            painter->fillRect(option->rect, option->palette.brush(QPalette::Base));
            return;
        }
        const auto* frame = qstyleoption_cast<const QStyleOptionFrame*>(option);
        if (frame != nullptr && frame->lineWidth > 0) {
            draw_field(painter, option->rect, *option, option->state.testFlag(State_HasFocus));
        } else {
            painter->fillRect(option->rect, option->palette.brush(QPalette::Base));
        }
        return;
    }
    case PE_FrameLineEdit:
        return;  // part of PE_PanelLineEdit
    case PE_PanelButtonCommand:
        if (const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option)) {
            draw_button_panel(painter, *button);
        } else {
            QStyleOptionButton plain;
            plain.QStyleOption::operator=(*option);
            draw_button_panel(painter, plain);
        }
        return;
    case PE_FrameDefaultButton:
        return;
    case PE_FrameFocusRect:
        if (qobject_cast<const QAbstractButton*>(widget) != nullptr) {
            draw_focus_ring(painter, option->rect, t().accent_line, kRadius);
        }
        return;  // item views show focus through the selection
    case PE_IndicatorCheckBox: {
        const bool enabled = option->state.testFlag(State_Enabled);
        const bool checked = option->state.testAnyFlags(State_On | State_NoChange);
        const bool hovered = option->state.testFlag(State_MouseOver);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRect square(option->rect.left(), option->rect.center().y() - kIndicatorSize / 2 + 1, kIndicatorSize,
                           kIndicatorSize);
        const QRectF box = QRectF(square).adjusted(0.5, 0.5, -0.5, -0.5);
        if (checked) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(!enabled ? t().text_disabled : hovered ? t().accent_hover : t().accent);
            painter->drawRoundedRect(box, 4, 4);
            painter->setPen(QPen(t().on_accent, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->setBrush(Qt::NoBrush);
            const QPointF o = box.topLeft();
            const qreal s = box.width();
            painter->drawPolyline(QPolygonF() << o + QPointF(s * 0.25, s * 0.52) << o + QPointF(s * 0.43, s * 0.70)
                                              << o + QPointF(s * 0.76, s * 0.33));
        } else {
            painter->setPen(QPen(!enabled ? t().border : hovered ? t().input_border_hover : t().input_underline, 1.0));
            painter->setBrush(enabled ? t().input : t().input_disabled);
            painter->drawRoundedRect(box, 4, 4);
        }
        painter->restore();
        return;
    }
    case PE_IndicatorArrowDown:
    case PE_IndicatorArrowUp:
    case PE_IndicatorArrowLeft:
    case PE_IndicatorArrowRight: {
        const Qt::ArrowType direction = element == PE_IndicatorArrowUp     ? Qt::UpArrow
                                        : element == PE_IndicatorArrowLeft  ? Qt::LeftArrow
                                        : element == PE_IndicatorArrowRight ? Qt::RightArrow
                                                                            : Qt::DownArrow;
        const qreal half = std::clamp(std::min(option->rect.width(), option->rect.height()) / 4.0, 2.5, 5.0);
        const QColor colour = option->state.testFlag(State_Enabled) ? t().text_secondary : t().text_disabled;
        draw_chevron(painter, QRectF(option->rect).center(), half, direction, colour);
        return;
    }
    case PE_IndicatorHeaderArrow:
        if (const auto* header = qstyleoption_cast<const QStyleOptionHeader*>(option)) {
            const Qt::ArrowType direction =
                header->sortIndicator == QStyleOptionHeader::SortUp ? Qt::UpArrow : Qt::DownArrow;
            draw_chevron(painter, QRectF(option->rect).center(), 3.5, direction, t().text_secondary, 1.4);
        }
        return;
    case PE_FrameTabWidget: {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(t().border, 1.0));
        painter->setBrush(t().surface);
        painter->drawRoundedRect(QRectF(option->rect).adjusted(0.5, 0.5, -0.5, -0.5), kCardRadius, kCardRadius);
        painter->restore();
        return;
    }
    case PE_FrameTabBarBase:
        return;
    case PE_PanelTipLabel: {
        painter->save();
        painter->fillRect(option->rect, t().dark ? t().surface_alt : t().surface);
        painter->setPen(t().input_border);
        painter->drawRect(option->rect.adjusted(0, 0, -1, -1));
        painter->restore();
        return;
    }
    default:
        break;
    }
    QProxyStyle::drawPrimitive(element, option, painter, widget);
}

void ModernStyle::drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                              const QWidget* widget) const
{
    switch (element) {
    case CE_PushButton:
        if (const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option)) {
            draw_button_panel(painter, *button);
            QStyleOptionButton label = *button;
            label.rect = button->rect.adjusted(4, 2, -4, -2);
            const bool primary = is_primary(*button) && !button->features.testFlag(QStyleOptionButton::Flat);
            if (primary) {
                label.palette.setColor(QPalette::ButtonText, t().on_accent);
                label.palette.setColor(QPalette::Disabled, QPalette::ButtonText,
                                       style::search_button_disabled_text());
            }
            QProxyStyle::drawControl(CE_PushButtonLabel, &label, painter, widget);
            if (button->state.testFlag(State_HasFocus) && button->state.testFlag(State_KeyboardFocusChange)) {
                if (primary) {
                    QColor ring = t().on_accent;
                    draw_focus_ring(painter, button->rect.adjusted(2, 2, -2, -2), ring, kRadius - 2);
                } else {
                    draw_focus_ring(painter, button->rect, t().accent_line, kRadius);
                }
            }
        }
        return;
    case CE_PushButtonBevel:
        if (const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option)) {
            draw_button_panel(painter, *button);
        }
        return;
    case CE_CheckBox:
        if (const auto* box = qstyleoption_cast<const QStyleOptionButton*>(option)) {
            QStyleOptionButton part = *box;
            part.rect = QRect(box->rect.left(), box->rect.top(), kIndicatorSize, box->rect.height());
            proxy()->drawPrimitive(PE_IndicatorCheckBox, &part, painter, widget);
            part.rect = box->rect.adjusted(kIndicatorSize + proxy()->pixelMetric(PM_CheckBoxLabelSpacing, box, widget),
                                           0, 0, 0);
            QProxyStyle::drawControl(CE_CheckBoxLabel, &part, painter, widget);
            if (box->state.testFlag(State_HasFocus) && box->state.testFlag(State_KeyboardFocusChange)) {
                const QRect square(box->rect.left(), box->rect.center().y() - kIndicatorSize / 2 + 1, kIndicatorSize,
                                   kIndicatorSize);
                draw_focus_ring(painter, square.adjusted(-3, -3, 3, 3), t().accent_line, 6);
            }
        }
        return;
    case CE_TabBarTabShape:
        if (const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option)) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            const QRect r = tab->rect;
            if (tab->state.testFlag(State_Selected)) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(t().accent_line);
                painter->drawRoundedRect(QRectF(r.left() + 10, r.bottom() - 2, r.width() - 20, 3), 1.5, 1.5);
            } else if (tab->state.testFlag(State_MouseOver)) {
                QColor fill = t().text;
                fill.setAlphaF(0.06F);
                painter->setPen(Qt::NoPen);
                painter->setBrush(fill);
                painter->drawRoundedRect(QRectF(r).adjusted(3, 4, -3, -5), kRadius, kRadius);
            }
            painter->restore();
        }
        return;
    case CE_TabBarTabLabel:
        if (const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option)) {
            QStyleOptionTab label = *tab;
            const bool selected = tab->state.testFlag(State_Selected);
            label.palette.setColor(QPalette::WindowText, selected ? t().text : t().text_secondary);
            QProxyStyle::drawControl(element, &label, painter, widget);
            if (tab->state.testFlag(State_HasFocus) && tab->state.testFlag(State_KeyboardFocusChange)) {
                draw_focus_ring(painter, tab->rect.adjusted(2, 3, -2, -4), t().accent_line, kRadius);
            }
        }
        return;
    case CE_Header:
        if (const auto* header = qstyleoption_cast<const QStyleOptionHeader*>(option)) {
            painter->save();
            painter->setClipRect(option->rect);
            proxy()->drawControl(CE_HeaderSection, header, painter, widget);
            QStyleOptionHeader label = *header;
            label.rect = proxy()->subElementRect(SE_HeaderLabel, header, widget);
            if (label.rect.isValid()) {
                proxy()->drawControl(CE_HeaderLabel, &label, painter, widget);
            }
            if (header->sortIndicator != QStyleOptionHeader::None) {
                label.rect = proxy()->subElementRect(SE_HeaderArrow, header, widget);
                proxy()->drawPrimitive(PE_IndicatorHeaderArrow, &label, painter, widget);
            }
            painter->restore();
        }
        return;
    case CE_HeaderSection: {
        const QRect r = option->rect;
        painter->fillRect(r, t().surface_alt);
        if (option->state.testFlag(State_MouseOver) && option->state.testFlag(State_Enabled)) {
            painter->fillRect(r, t().surface_alt);
        }
        painter->fillRect(QRect(r.left(), r.bottom(), r.width(), 1), t().border);
        const auto* header = qstyleoption_cast<const QStyleOptionHeader*>(option);
        const bool last = header != nullptr && (header->position == QStyleOptionHeader::End ||
                                                header->position == QStyleOptionHeader::OnlyOneSection);
        if (!last && r.height() > 16) {
            painter->fillRect(QRect(r.right(), r.top() + 8, 1, r.height() - 16), t().border);
        }
        return;
    }
    case CE_HeaderEmptyArea: {
        const QRect r = option->rect;
        painter->fillRect(r, t().surface_alt);
        painter->fillRect(QRect(r.left(), r.bottom(), r.width(), 1), t().border);
        return;
    }
    case CE_HeaderLabel:
        if (const auto* header = qstyleoption_cast<const QStyleOptionHeader*>(option)) {
            painter->save();
            painter->setPen(t().text_secondary);
            painter->drawText(header->rect, static_cast<int>(header->textAlignment), header->text);
            painter->restore();
        }
        return;
    case CE_MenuItem:
        if (const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option)) {
            QStyleOptionMenuItem quiet = *item;
            quiet.palette.setColor(QPalette::Highlight, t().selection);
            quiet.palette.setColor(QPalette::HighlightedText, t().selection_text);
            QProxyStyle::drawControl(element, &quiet, painter, widget);
            return;
        }
        break;
    default:
        break;
    }
    QProxyStyle::drawControl(element, option, painter, widget);
}

void ModernStyle::drawComplexControl(ComplexControl control, const QStyleOptionComplex* option, QPainter* painter,
                                     const QWidget* widget) const
{
    switch (control) {
    case CC_ComboBox:
        if (const auto* combo = qstyleoption_cast<const QStyleOptionComboBox*>(option)) {
            draw_field(painter, combo->rect, *combo, combo->state.testFlag(State_HasFocus));
            const QRect arrow = proxy()->subControlRect(CC_ComboBox, combo, SC_ComboBoxArrow, widget);
            const QColor colour = combo->state.testFlag(State_Enabled) ? t().text_secondary : t().text_disabled;
            draw_chevron(painter, QRectF(arrow).center() + QPointF(-2, 0.5), 4.5, Qt::DownArrow, colour);
        }
        return;
    case CC_SpinBox:
        if (const auto* spin = qstyleoption_cast<const QStyleOptionSpinBox*>(option)) {
            draw_field(painter, spin->rect, *spin, spin->state.testFlag(State_HasFocus));
            if (spin->buttonSymbols == QAbstractSpinBox::NoButtons) {
                return;
            }
            const bool enabled = spin->state.testFlag(State_Enabled);
            for (const SubControl part : {SC_SpinBoxUp, SC_SpinBoxDown}) {
                const QRect r = proxy()->subControlRect(CC_SpinBox, spin, part, widget);
                const bool step_enabled = part == SC_SpinBoxUp
                                              ? spin->stepEnabled.testFlag(QAbstractSpinBox::StepUpEnabled)
                                              : spin->stepEnabled.testFlag(QAbstractSpinBox::StepDownEnabled);
                const bool active = spin->activeSubControls.testFlag(part) && enabled && step_enabled;
                if (active && spin->state.testFlag(State_MouseOver)) {
                    painter->save();
                    painter->setRenderHint(QPainter::Antialiasing);
                    QColor fill = t().text;
                    fill.setAlphaF(spin->state.testFlag(State_Sunken) ? 0.14F : 0.07F);
                    painter->setPen(Qt::NoPen);
                    painter->setBrush(fill);
                    painter->drawRoundedRect(QRectF(r).adjusted(1, 0.5, -1, -0.5), 3, 3);
                    painter->restore();
                }
                const QColor colour = enabled && step_enabled ? t().text_secondary : t().text_disabled;
                const QPointF centre = QRectF(r).center() + QPointF(0, part == SC_SpinBoxUp ? 1.0 : -1.0);
                draw_chevron(painter, centre, 3.5, part == SC_SpinBoxUp ? Qt::UpArrow : Qt::DownArrow, colour, 1.3);
            }
        }
        return;
    case CC_ScrollBar:
        if (const auto* bar = qstyleoption_cast<const QStyleOptionSlider*>(option)) {
            painter->fillRect(bar->rect, t().surface);
            if (bar->maximum == bar->minimum) {
                return;  // nothing to scroll: no thumb
            }
            const QRect slider = proxy()->subControlRect(CC_ScrollBar, bar, SC_ScrollBarSlider, widget);
            const bool hovered = bar->state.testFlag(State_MouseOver);
            const bool dragging = bar->activeSubControls.testFlag(SC_ScrollBarSlider) &&
                                  bar->state.testFlag(State_Sunken);
            const qreal thickness = hovered || dragging ? 8.0 : 6.0;
            QRectF thumb(slider);
            if (bar->orientation == Qt::Horizontal) {
                thumb = QRectF(thumb.left() + 2, thumb.center().y() - thickness / 2, thumb.width() - 4, thickness);
            } else {
                thumb = QRectF(thumb.center().x() - thickness / 2, thumb.top() + 2, thickness, thumb.height() - 4);
            }
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(hovered || dragging ? t().scroll_thumb_hover : t().scroll_thumb);
            painter->drawRoundedRect(thumb, thickness / 2, thickness / 2);
            painter->restore();
        }
        return;
    default:
        break;
    }
    QProxyStyle::drawComplexControl(control, option, painter, widget);
}

} // namespace wizz::app
