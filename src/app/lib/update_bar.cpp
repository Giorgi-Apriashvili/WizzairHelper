#include "update_bar.hpp"

#include "qt_strings.hpp"
#include "search_button.hpp"
#include "ui_style.hpp"

#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>

#include <algorithm>

namespace wizz::app {

namespace {

/// Horizontal padding of the "Later" text inside its flat button.
constexpr int kLaterPadding = 10;

/// A small, flat "Later": its text plus a little padding, never the style's 75 px
/// minimum of a push button, at least as high as a WinForms button (23 px). Still a
/// QPushButton, so a focused one is clicked by Space and by the window's Enter (01 §6.3).
class LaterButton final : public QPushButton {
public:
    explicit LaterButton(const QString& text, QWidget* parent) : QPushButton(text, parent)
    {
        setFlat(true);
        setAutoDefault(false);
        setDefault(false);
        setFocusPolicy(Qt::StrongFocus);
    }

    [[nodiscard]] QSize sizeHint() const override
    {
        ensurePolished();
        const QFontMetrics metrics{font()};
        const QSize text = metrics.size(Qt::TextShowMnemonic, this->text());
        return QSize{text.width() + 2 * kLaterPadding, std::max(SearchButton::kMinimumSize.height(), text.height() + 8)};
    }
    [[nodiscard]] QSize minimumSizeHint() const override { return sizeHint(); }
};


} // namespace

UpdateBar::UpdateBar(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("updateBar"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(kAccentWidth + kPadding.left(), kPadding.top(), kPadding.right(), kPadding.bottom());
    layout->setSpacing(kSpacing);

    message_ = new QLabel(this);
    message_->setTextFormat(Qt::PlainText);
    message_->setWordWrap(true);  // wraps instead of clipping in a narrow window or at 200 %
    message_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    message_->setFocusPolicy(Qt::NoFocus);
    message_->setFont(style::form_font());

    action_ = new SearchButton(QString{}, this);
    later_ = new LaterButton(to_qstring(ui::update_strings::kLater), this);
    later_->setFont(style::form_font());
    // The buttons keep their size; the message takes the rest of the width and wraps.
    action_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    later_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    layout->addWidget(message_, 1);
    layout->addWidget(action_, 0, Qt::AlignVCenter);
    layout->addWidget(later_, 0, Qt::AlignVCenter);

    connect(action_, &QPushButton::clicked, this, &UpdateBar::action_clicked);
    connect(later_, &QPushButton::clicked, this, &UpdateBar::later_clicked);
    apply_text_colour();
    style::on_theme_change(this, [this] { apply_text_colour(); });
    hide();  // until an update is offered
}

void UpdateBar::show_view(const ui::UpdateBarView& view)
{
    const QString text = to_qstring(view.text);
    if (message_->text() != text) {
        message_->setText(text);
    }
    const QString action = to_qstring(view.action);
    if (action_->text() != action) {
        action_->setText(action);
    }
    action_->setEnabled(view.action_enabled);
    // Showing a child never moves the focus; hiding one that has it passes it on.
    setVisible(view.visible);
    view_ = view;
}

QColor UpdateBar::background_colour()
{
    return style::theme().update_tint;
}

void UpdateBar::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter{this};
    painter.fillRect(rect(), background_colour());
    painter.fillRect(QRect{0, 0, kAccentWidth, height()}, style::theme().accent_line);
    painter.fillRect(QRect{0, height() - 1, width(), 1}, style::theme().border);
}

void UpdateBar::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        apply_text_colour();
    }
}

void UpdateBar::apply_text_colour()
{
    // The theme's text on the tint, whatever the style paints.
    const QColor text = style::theme().text;
    for (QWidget* widget : {static_cast<QWidget*>(message_), static_cast<QWidget*>(later_)}) {
        QPalette palette = widget->palette();
        if (palette.color(QPalette::WindowText) != text || palette.color(QPalette::ButtonText) != text) {
            palette.setColor(QPalette::WindowText, text);
            palette.setColor(QPalette::ButtonText, text);
            widget->setPalette(palette);
        }
    }
}

} // namespace wizz::app
