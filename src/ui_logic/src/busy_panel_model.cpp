#include "wizz/ui/busy_panel_model.hpp"

#include "wizz/ui/strings.hpp"

#include "wizz/core/format.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>
#include <utility>

namespace wizz::ui {

namespace {

constexpr double kTicksPerSecond = 10'000'000.0;

/// TimeSpan.TotalSeconds.
[[nodiscard]] double total_seconds(Duration duration) noexcept
{
    return static_cast<double>(duration.count()) / kTicksPerSecond;
}

} // namespace

// ---------------------------------------------------------------------------
// TaskbarProgress (01 §10.9)
// ---------------------------------------------------------------------------

void TaskbarProgress::set_state(TaskbarState state)
{
    if (state == last_state_) {
        return;
    }
    calls_.push_back(TaskbarCall{.kind = TaskbarCall::Kind::set_state, .state = state, .value = 0});
    last_state_ = state;
    last_value_ = kNoValue;
}

void TaskbarProgress::set_value(int permille)
{
    const int value = std::clamp(permille, 0, kMaxValue);
    if (value == last_value_) {
        return;
    }
    calls_.push_back(TaskbarCall{.kind = TaskbarCall::Kind::set_value, .state = TaskbarState::normal, .value = value});
    last_value_ = value;
    last_state_ = TaskbarState::normal;
}

std::vector<TaskbarCall> TaskbarProgress::take_calls()
{
    return std::exchange(calls_, {});
}

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------

std::string progress_status_text(const SearchProgress& progress)
{
    std::string text = progress.stage;
    if (!progress.detail.empty()) {
        text += ":  ";
        text += progress.detail;
    }
    if (progress.total > 0) {
        text += std::format("    ({} of {})", std::min(progress.completed + 1, progress.total), progress.total);
    }
    return text;
}

std::optional<SpinnerGeometry> spinner_geometry(int width, int height) noexcept
{
    const int size = std::min(width, height) - 8;
    if (size < 8) {
        return std::nullopt;
    }
    SpinnerGeometry geometry;
    geometry.size = size;
    geometry.left = (width - size) / 2;
    geometry.top = (height - size) / 2;
    geometry.thickness = std::max(2.5F, static_cast<float>(size) / 7.0F);
    geometry.dot_diameter = static_cast<int>(geometry.thickness) + 2;
    geometry.dot_left = geometry.left + size / 2 - geometry.dot_diameter / 2;
    geometry.dot_top = geometry.top + size / 2 - geometry.dot_diameter / 2;
    return geometry;
}

// ---------------------------------------------------------------------------
// BusyPanelModel (01 §10.2-§10.5)
// ---------------------------------------------------------------------------

BusyPanelModel::BusyPanelModel(const Clock& clock, std::shared_ptr<const Locale> locale)
    : clock_(clock), locale_(std::move(locale))
{
    if (!locale_) {
        throw std::invalid_argument("BusyPanelModel needs a locale.");
    }
}

void BusyPanelModel::begin(std::string_view text, bool cancellable)
{
    busy_ = true;
    last_.reset();
    eta_stage_.reset();
    updates_received_ = 0;
    started_utc_ = clock_.utc_now();
    target_ = -1.0;
    shown_ = 0.0;
    view_.status_tone = StatusTone::normal;
    view_.status_text = std::string{text};
    view_.wait_text.clear();
    view_.cancel_enabled = cancellable;
    view_.spinning = true;
    view_.bar_style = BarStyle::marquee;
    taskbar_.set_state(TaskbarState::indeterminate);
    animate();
}

void BusyPanelModel::report(const SearchProgress& progress)
{
    if (!busy_) {
        return;  // posted reports may arrive after End (01 §10.3)
    }
    last_ = progress;
    ++updates_received_;
    target_ = progress.total > 0
                  ? std::clamp(static_cast<double>(progress.completed) / static_cast<double>(progress.total), 0.0, 1.0)
                  : -1.0;
    if (progress.total > 0 && (!eta_stage_ || *eta_stage_ != progress.stage)) {
        eta_stage_ = progress.stage;
        eta_start_utc_ = clock_.utc_now();
        eta_base_completed_ = progress.completed;
    }
    view_.status_text = progress_status_text(progress);
    animate();
}

void BusyPanelModel::end(std::string_view summary, bool success)
{
    busy_ = false;
    view_.spinning = false;
    view_.cancel_enabled = false;
    view_.wait_text.clear();
    view_.bar_style = BarStyle::continuous;
    if (success) {
        shown_ = 1.0;
    }
    set_bar_value(static_cast<int>(shown_ * kBarMax));  // truncation (07 §2)
    view_.elapsed_text = format_elapsed(clock_.utc_now() - started_utc_);
    view_.status_tone = success ? StatusTone::success : StatusTone::failure;
    view_.status_text = std::string{summary};
    taskbar_.set_state(success ? TaskbarState::none : TaskbarState::error);
    if (!success) {
        error_reset_requested_ = true;
    }
    ++view_.frame;
}

bool BusyPanelModel::request_cancel()
{
    if (!busy_ || !view_.cancel_enabled) {
        return false;
    }
    view_.cancel_enabled = false;
    view_.status_text = std::string{strings::kCancelling};
    ++view_.frame;
    return true;
}

void BusyPanelModel::animate()
{
    if (!busy_) {
        return;
    }
    const UtcInstant now = clock_.utc_now();
    std::string elapsed = format_elapsed(now - started_utc_);
    if (last_ && last_->total > 0 && eta_stage_ && last_->stage == *eta_stage_) {
        const int done = last_->completed - eta_base_completed_;
        const int left = last_->total - last_->completed;
        if (done >= 1 && left > 0) {
            const double eta_seconds =
                total_seconds(now - eta_start_utc_) / static_cast<double>(done) * static_cast<double>(left);
            elapsed += "  ~" + format_remaining(eta_seconds) + " left";
        }
    }
    view_.elapsed_text = std::move(elapsed);
    if (view_.spinning) {
        view_.spinner_angle = next_spinner_angle(view_.spinner_angle);
    }

    if (target_ < 0) {
        view_.bar_style = BarStyle::marquee;
        taskbar_.set_state(TaskbarState::indeterminate);
    } else {
        view_.bar_style = BarStyle::continuous;
        const double gap = target_ - shown_;
        shown_ = std::abs(gap) < kSnapGap ? target_ : shown_ + gap * kEasing;
        const int value = static_cast<int>(round_half_even(shown_ * kBarMax));
        set_bar_value(value);
        taskbar_.set_value(value);
    }

    view_.wait_text = wait_note(now);
    ++view_.frame;
}

bool BusyPanelModel::take_error_reset_request() noexcept
{
    return std::exchange(error_reset_requested_, false);
}

void BusyPanelModel::error_reset_elapsed()
{
    if (!busy_) {
        taskbar_.set_state(TaskbarState::none);
    }
}

void BusyPanelModel::set_bar_value(int value) noexcept
{
    view_.bar_value = std::clamp(value, 0, kBarMax);
}

std::string BusyPanelModel::wait_note(UtcInstant now) const
{
    if (!last_) {
        return {};
    }
    const NumberSymbols& symbols = locale_->number_symbols();
    if (last_->waiting_until_utc && *last_->waiting_until_utc > now) {
        return strings::retry_countdown(format_label(total_seconds(*last_->waiting_until_utc - now), 1, symbols));
    }
    if (!last_->waiting_for_user && last_->step_started_utc && now - *last_->step_started_utc > kSlowStep) {
        return strings::waiting_for_server(format_label(total_seconds(now - *last_->step_started_utc), 0, symbols));
    }
    return {};
}

} // namespace wizz::ui
