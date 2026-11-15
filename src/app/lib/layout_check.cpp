#include "layout_check.hpp"

#include "elided_label.hpp"
#include "qt_strings.hpp"

#include <QAbstractButton>
#include <QLabel>
#include <QLayout>
#include <QTabBar>
#include <QWidget>

#include <format>
#include <utility>

namespace wizz::app {

namespace {

constexpr qsizetype kMaxNameLength = 40;

/// The visible widgets `layout` (and its nested layouts) places, in layout order.
void collect_placed(const QLayout& layout, std::vector<QWidget*>& out)
{
    for (int i = 0; i < layout.count(); ++i) {
        QLayoutItem* const item = layout.itemAt(i);
        if (item == nullptr) {
            continue;
        }
        if (QWidget* const widget = item->widget(); widget != nullptr) {
            if (widget->isVisible()) {
                out.push_back(widget);
            }
        } else if (const QLayout* const nested = item->layout(); nested != nullptr) {
            collect_placed(*nested, out);
        }
    }
}

/// A label or button whose text is cut because it got less room than it needs.
[[nodiscard]] bool text_is_cut(const QWidget& widget)
{
    const QSizePolicy policy = widget.sizePolicy();
    const bool check_width = policy.horizontalPolicy() != QSizePolicy::Ignored;
    const bool check_height = policy.verticalPolicy() != QSizePolicy::Ignored;
    if (qobject_cast<const ElidedLabel*>(&widget) != nullptr) {
        return false;  // elides by design (01 §10.1 status line)
    }
    if (const auto* label = qobject_cast<const QLabel*>(&widget); label != nullptr) {
        if (label->text().isEmpty()) {
            return false;
        }
        if (label->wordWrap()) {
            const int needed = label->heightForWidth(label->width());
            return check_height && needed > label->height();
        }
    } else if (qobject_cast<const QAbstractButton*>(&widget) == nullptr) {
        return false;  // combos, spin boxes and date pickers clip their contents like WinForms
    }
    const QSize needed = widget.minimumSizeHint();
    return (check_width && widget.width() < needed.width()) || (check_height && widget.height() < needed.height());
}

[[nodiscard]] bool tabs_fit(const QTabBar& bar)
{
    int width = 0;
    for (int i = 0; i < bar.count(); ++i) {
        width += bar.tabRect(i).width();
    }
    return width <= bar.width();
}

void check_layout_of(const QWidget& owner, std::vector<std::string>& problems)
{
    std::vector<QWidget*> placed;
    collect_placed(*owner.layout(), placed);
    const QRect owner_rect = owner.rect();
    for (std::size_t i = 0; i < placed.size(); ++i) {
        const QWidget& widget = *placed[i];
        if (widget.parentWidget() == &owner && !owner_rect.contains(widget.geometry())) {
            problems.push_back(std::format("{}: clipped by its parent {}", describe_widget(widget), describe_widget(owner)));
        } else if (!is_fully_visible(widget)) {
            problems.push_back(std::format("{}: cut off by the window", describe_widget(widget)));
        }
        if (text_is_cut(widget)) {
            problems.push_back(std::format("{}: smaller than its text ({}x{} < {}x{})", describe_widget(widget),
                                           widget.width(), widget.height(), widget.minimumSizeHint().width(),
                                           widget.minimumSizeHint().height()));
        }
        for (std::size_t j = i + 1; j < placed.size(); ++j) {
            const QWidget& other = *placed[j];
            if (other.parentWidget() == widget.parentWidget() && widget.geometry().intersects(other.geometry())) {
                problems.push_back(std::format("{}: overlaps {}", describe_widget(widget), describe_widget(other)));
            }
        }
    }
}

} // namespace

std::string describe_widget(const QWidget& widget)
{
    QString name;
    if (const auto* label = qobject_cast<const QLabel*>(&widget); label != nullptr) {
        name = label->text();
    } else if (const auto* button = qobject_cast<const QAbstractButton*>(&widget); button != nullptr) {
        name = button->text();
    }
    if (name.isEmpty()) {
        name = widget.objectName();
    }
    std::string text{widget.metaObject()->className()};
    if (!name.isEmpty()) {
        if (name.size() > kMaxNameLength) {
            name = name.left(kMaxNameLength) + QStringLiteral("…");
        }
        text += std::format(" '{}'", to_utf8(name.simplified()));
    }
    return text;
}

bool is_fully_visible(const QWidget& widget)
{
    if (!widget.isVisible()) {
        return false;
    }
    for (const QWidget* ancestor = widget.parentWidget(); ancestor != nullptr; ancestor = ancestor->parentWidget()) {
        const QRect mapped{widget.mapTo(ancestor, QPoint{0, 0}), widget.size()};
        if (!ancestor->rect().contains(mapped)) {
            return false;
        }
        if (ancestor->isWindow()) {
            break;
        }
    }
    return true;
}

std::vector<std::string> layout_problems(const QWidget& root)
{
    std::vector<std::string> problems;
    if (!root.isVisible()) {
        return problems;
    }
    std::vector<const QWidget*> widgets{&root};
    for (const QWidget* child : root.findChildren<QWidget*>()) {
        if (child->isVisible() && child->window() == root.window()) {
            widgets.push_back(child);
        }
    }
    for (const QWidget* widget : widgets) {
        if (widget->layout() != nullptr) {
            check_layout_of(*widget, problems);
        }
        if (const auto* bar = qobject_cast<const QTabBar*>(widget); bar != nullptr && !tabs_fit(*bar)) {
            problems.push_back(std::format("{}: tabs do not fit", describe_widget(*bar)));
        }
    }
    return problems;
}

} // namespace wizz::app
