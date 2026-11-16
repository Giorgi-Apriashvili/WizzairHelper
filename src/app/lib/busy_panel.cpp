#include "busy_panel.hpp"

#include "elided_label.hpp"
#include "spinner_widget.hpp"
#include "ui_style.hpp"

#include "wizz/ui/strings.hpp"

#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QProgressBar>
#include <QPushButton>
#include <QString>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace wizz::app {

namespace {

// 01 §10.1 constants, logical pixels (L(x) = x in Qt).
constexpr int kMinBarRow = 22;
constexpr int kElapsedRowExtra = 4;
constexpr int kStatusRowExtra = 6;
constexpr int kSpinnerRightMargin = 6;
constexpr QMargins kBarMargins{0, 2, 14, 2};
constexpr int kAutoColumnRightMargin = 12;  // _elapsed and _wait
constexpr QMargins kButtonMargins{3, 3, 3, 3};  // WinForms default Control.Margin

[[nodiscard]] QString to_qstring(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

/// TextRenderer.MeasureText(text, font).Height: the single-line text height.
[[nodiscard]] int text_height(const QFont& font, const QString& text, const QPaintDevice* device)
{
    const QFontMetrics metrics = device != nullptr ? QFontMetrics(font, device) : QFontMetrics(font);
    return metrics.size(Qt::TextSingleLine, text).height();
}

/// The bar's fill at rest: the outcome's colour, softened halfway towards the track (High
/// Contrast: the system highlight, nothing mixed).
[[nodiscard]] QColor resting_fill(const QColor& outcome)
{
    const style::Theme& theme = style::theme();
    if (theme.high_contrast) {
        return theme.accent;
    }
    const QColor track = theme.track;
    return QColor((outcome.red() + track.red()) / 2, (outcome.green() + track.green()) / 2,
                  (outcome.blue() + track.blue()) / 2);
}

/// A plain container that gives `child` the WinForms cell margins.
[[nodiscard]] QWidget* boxed(QWidget* child, QMargins margins, QWidget* parent)
{
    auto* box = new QWidget(parent);
    auto* layout = new QHBoxLayout(box);
    layout->setContentsMargins(margins);
    layout->setSpacing(0);
    child->setParent(box);
    layout->addWidget(child);
    return box;
}

} // namespace

BusyPanel::BusyPanel(const Clock& clock, std::shared_ptr<const Locale> locale,
                     const TaskbarProgress::Factory& taskbar_factory, QWidget* parent)
    : QWidget(parent), model_(clock, std::move(locale)), taskbar_(this, taskbar_factory)
{
    // The background is the theme's surface, painted in paintEvent (not a palette role:
    // styles repaint those, and the children keep the style's palette).
    setAttribute(Qt::WA_OpaquePaintEvent);

    spinner_ = new SpinnerWidget(this);
    spinner_->setContentsMargins(0, 0, kSpinnerRightMargin, 0);
    spinner_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);

    bar_ = new PanelProgressBar;
    bar_->setRange(0, kBarRangeMax);
    bar_->setValue(0);
    bar_->setTextVisible(false);
    bar_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    bar_box_ = boxed(bar_, kBarMargins, this);
    bar_box_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

    elapsed_ = new QLabel(this);
    elapsed_->setTextFormat(Qt::PlainText);
    elapsed_->setFont(elapsed_font());
    elapsed_->setContentsMargins(0, 0, kAutoColumnRightMargin, 0);

    status_ = new ElidedLabel(this);
    status_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    status_->setForegroundRole(QPalette::WindowText);

    wait_ = new QLabel(this);
    wait_->setTextFormat(Qt::PlainText);
    QFont bold;  // only the weight is set: family and size follow the panel (form) font
    bold.setWeight(QFont::DemiBold);
    wait_->setFont(bold);
    wait_->setContentsMargins(0, 0, kAutoColumnRightMargin, 0);

    cancel_ = new QPushButton(to_qstring(ui::strings::kCancel));
    cancel_->setEnabled(false);
    QWidget* cancel_box = boxed(cancel_, kButtonMargins, this);
    cancel_box->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(cancel_, &QPushButton::clicked, this, &BusyPanel::request_cancel);

    grid_ = new QGridLayout(this);
    grid_->setSpacing(0);
    grid_->addWidget(spinner_, 0, 0, 2, 1);
    grid_->addWidget(bar_box_, 0, 1);
    grid_->addWidget(elapsed_, 0, 2, Qt::AlignLeft | Qt::AlignVCenter);
    grid_->addWidget(cancel_box, 0, 3, 2, 1, Qt::AlignRight | Qt::AlignVCenter);
    grid_->addWidget(status_, 1, 1);
    grid_->addWidget(wait_, 1, 2, Qt::AlignLeft | Qt::AlignVCenter);
    grid_->setColumnStretch(1, 1);

    timer_ = new QTimer(this);
    timer_->setObjectName(QStringLiteral("animationTimer"));
    timer_->setTimerType(Qt::PreciseTimer);  // a coarse 40 ms timer can slip past R-04's 50 ms
    timer_->setInterval(ui::BusyPanelModel::kTimerInterval);
    connect(timer_, &QTimer::timeout, this, &BusyPanel::animate);

    error_reset_timer_ = new QTimer(this);
    error_reset_timer_->setObjectName(QStringLiteral("errorResetTimer"));
    error_reset_timer_->setSingleShot(true);
    error_reset_timer_->setInterval(ui::BusyPanelModel::kErrorResetDelay);
    connect(error_reset_timer_, &QTimer::timeout, this, &BusyPanel::error_reset_elapsed);

    refresh_theme_colours();
    apply_metrics();
    render();
    style::on_theme_change(this, [this] {
        refresh_theme_colours();
        render();  // the bar's fill
    });
}

BusyPanel::BusyPanel(const Clock& clock, std::shared_ptr<const Locale> locale, QWidget* parent)
    : BusyPanel(clock, std::move(locale), TaskbarProgress::Factory{}, parent)
{
}

BusyPanel::~BusyPanel() = default;

void BusyPanel::begin(std::string_view text, bool cancellable)
{
    model_.begin(text, cancellable);
    render();
}

void BusyPanel::report(const SearchProgress& progress)
{
    model_.report(progress);
    render();
}

void BusyPanel::report(std::span<const SearchProgress> reports)
{
    if (reports.empty()) {
        return;
    }
    for (const SearchProgress& progress : reports) {
        model_.report(progress);
    }
    render();
}

void BusyPanel::end(std::string_view summary, bool success)
{
    model_.end(summary, success);
    render();
}

void BusyPanel::end_if_still_busy()
{
    if (model_.is_busy()) {
        end(ui::strings::kUnexpectedErrorText, false);
    }
}

void BusyPanel::request_cancel()
{
    if (!model_.request_cancel()) {
        return;  // not busy, not cancellable, or already cancelling (D-11)
    }
    render();
    Q_EMIT cancel_requested();
}

bool BusyPanel::is_busy() const noexcept
{
    return model_.is_busy();
}

const std::string& BusyPanel::status_text() const noexcept
{
    return model_.status_text();
}

int BusyPanel::updates_received() const noexcept
{
    return model_.updates_received();
}

QColor BusyPanel::status_colour() const
{
    switch (model_.view().status_tone) {
    case ui::StatusTone::success:
        return style::theme().success;
    case ui::StatusTone::failure:
        return style::theme().failure;
    case ui::StatusTone::normal:
        break;
    }
    return control_text_;
}

BusyPanelMetrics BusyPanel::compute_metrics(const QFont& panel_font, const QFont& wait_font, const QFont& elapsed_font,
                                            const QPaintDevice* device)
{
    const QString line_sample = QStringLiteral("Ag");
    BusyPanelMetrics metrics;
    metrics.line = std::max(text_height(panel_font, line_sample, device), text_height(wait_font, line_sample, device));
    metrics.bar_row =
        std::max(kMinBarRow, text_height(elapsed_font, QStringLiteral("0:00"), device) + kElapsedRowExtra);
    metrics.status_row = metrics.line + kStatusRowExtra;
    metrics.height = metrics.bar_row + metrics.status_row + metrics.padding.top() + metrics.padding.bottom();
    return metrics;
}

QFont BusyPanel::elapsed_font()
{
    return style::mono_font(10.0, true);  // Cascadia Mono, Consolas on Windows 10
}

void BusyPanel::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    switch (event->type()) {
    case QEvent::FontChange:
    case QEvent::StyleChange:
        apply_metrics();
        break;
    case QEvent::PaletteChange:
        refresh_theme_colours();  // a colour scheme change re-resolves the palettes
        break;
    default:
        break;
    }
}

bool BusyPanel::event(QEvent* event)
{
    const bool handled = QWidget::event(event);
    switch (event->type()) {
    case QEvent::Polish:
    case QEvent::DevicePixelRatioChange:
    case QEvent::ScreenChangeInternal:
        apply_metrics();  // OnDpiChangedAfterParent: fonts may measure differently on the new screen
        break;
    case QEvent::ThemeChange:
        refresh_theme_colours();
        break;
    default:
        break;
    }
    return handled;
}

void BusyPanel::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.fillRect(rect(), control_light_);
    painter.fillRect(QRect(0, 0, width(), 1), style::theme().border);  // the status bar's top edge
}

void BusyPanel::animate()
{
    model_.animate();
    render();
}

void BusyPanel::error_reset_elapsed()
{
    model_.error_reset_elapsed();
    render();
}

void BusyPanel::render()
{
    const ui::BusyView& view = model_.view();

    status_->setText(to_qstring(view.status_text));
    if (view.status_tone != shown_tone_) {
        shown_tone_ = view.status_tone;
        apply_status_colour();
    }
    elapsed_->setText(to_qstring(view.elapsed_text));
    wait_->setText(to_qstring(view.wait_text));
    cancel_->setEnabled(view.cancel_enabled);

    if (view.bar_style == ui::BarStyle::marquee) {
        if (bar_->maximum() != 0) {
            bar_->setRange(0, 0);  // busy indicator (PBS_MARQUEE); the style sets its speed
        }
    } else {
        if (bar_->maximum() == 0) {
            bar_->setRange(0, kBarRangeMax);
        }
        set_bar_value(view.bar_value);
    }

    // The bar moves only while something is being processed (D-34); at rest its fill takes
    // the colour of the outcome.
    bar_->set_live(is_busy());
    bar_->set_fill(is_busy() ? style::theme().accent : resting_fill(status_colour()));
    if (view.bar_style == ui::BarStyle::marquee) {
        bar_->update();  // the marquee segment advances with the 40 ms timer
    }
    spinner_->set_spinning(view.spinning);
    spinner_->set_angle(view.spinner_angle);

    for (const ui::TaskbarCall& call : model_.take_taskbar_calls()) {
        taskbar_.apply(call);
    }
    if (model_.take_error_reset_request()) {
        // One timer: a later failure restarts it, so Error stays 4 s after the latest one.
        error_reset_timer_->start();
    }

    if (model_.timer_running()) {
        if (!timer_->isActive()) {
            timer_->start();
            Q_EMIT animation_started();
        }
    } else if (timer_->isActive()) {
        timer_->stop();
        Q_EMIT animation_stopped();
    }
}

void BusyPanel::apply_metrics()
{
    if (grid_ == nullptr) {
        return;  // change events sent while the constructor builds the controls
    }
    const BusyPanelMetrics metrics = compute_metrics(font(), wait_->font(), elapsed_->font(), this);
    if (metrics == metrics_ && maximumHeight() == metrics.height) {
        return;
    }
    metrics_ = metrics;
    grid_->setContentsMargins(metrics_.padding);
    grid_->setColumnMinimumWidth(0, metrics_.spinner_column);
    grid_->setRowMinimumHeight(0, metrics_.bar_row);
    grid_->setRowMinimumHeight(1, metrics_.status_row);
    spinner_->setFixedWidth(metrics_.spinner_column);
    bar_box_->setFixedHeight(metrics_.bar_row);
    setFixedHeight(metrics_.height);
}

void BusyPanel::refresh_theme_colours()
{
    if (status_ == nullptr || wait_ == nullptr) {
        return;  // change events sent while the constructor builds the controls
    }
    const style::Theme& theme = style::theme();
    if (theme.surface != control_light_) {
        control_light_ = theme.surface;
        update();
    }
    control_text_ = theme.text;
    const auto set_text = [](QWidget& widget, const QColor& colour) {
        if (widget.palette().color(QPalette::WindowText) != colour) {
            QPalette palette = widget.palette();
            palette.setColor(QPalette::WindowText, colour);
            widget.setPalette(palette);
        }
    };
    set_text(*elapsed_, theme.text_secondary);
    set_text(*wait_, theme.warning);
    apply_status_colour();
}

void BusyPanel::apply_status_colour()
{
    const QColor colour = status_colour();
    if (status_->palette().color(QPalette::WindowText) != colour) {
        QPalette status_palette = status_->palette();
        status_palette.setColor(QPalette::WindowText, colour);
        status_->setPalette(status_palette);
    }
}

void BusyPanel::set_bar_value(int value)
{
    // 01 §10.6 sets value + 1 and then value because the native themed bar animates
    // forward moves slowly; QProgressBar paints the new value at once (D-04).
    const int clamped = std::clamp(value, 0, ui::BusyPanelModel::kBarMax);
    if (bar_->value() != clamped) {
        bar_->setValue(clamped);
    }
}

} // namespace wizz::app
