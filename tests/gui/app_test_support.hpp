#pragma once
// A whole main window for the flow tests (01 §11-§13, §17): the real widgets,
// controller and services, with
//   * the HTTP traffic answered by a ReplayTransport — the --autotest replay
//     scenarios under tests/replay, without their delays, or rules built in code;
//   * the fixed test clock (2026-11-02T10:00:00Z, local zone UTC) and en-US;
//   * no request pacing, no retry sleeps, no disk caches, a temporary data root;
//   * recorded MessageBoxes, chooser and grid side effects instead of modal boxes.

#include "busy_panel.hpp"
#include "controller.hpp"
#include "controller_dialogs.hpp"
#include "flight_grid.hpp"
#include "grid_test_support.hpp"
#include "input_panel.hpp"
#include "log_view.hpp"
#include "main_window.hpp"
#include "round_trip_grid.hpp"
#include "task.hpp"

#include "wizz/core/cancellation.hpp"
#include "wizz/core/clocks.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/ui/replay_transport.hpp"
#include "wizz/ui/settings.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTabWidget>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace wizz::test::app {

using namespace std::chrono_literals;

/// tests/replay in the source tree (the --autotest scenarios, D-30).
[[nodiscard]] inline std::filesystem::path replay_root()
{
    return std::filesystem::path{WIZZ_TEST_REPLAY_DIR};
}

/// The rules of a replay scenario; `keep_delays` false drops the slow-server delays.
[[nodiscard]] inline std::vector<ui::ReplayTransport::Rule> scenario_rules(std::string_view scenario,
                                                                          bool keep_delays = false)
{
    const ui::ReplayTransport loaded{replay_root() / scenario};
    std::vector<ui::ReplayTransport::Rule> rules = loaded.rules();
    if (!keep_delays) {
        for (ui::ReplayTransport::Rule& rule : rules) {
            rule.delay = std::chrono::milliseconds{0};
        }
    }
    return rules;
}

/// A rule answering `url_contains` with an inline JSON body.
[[nodiscard]] inline ui::ReplayTransport::Rule json_rule(std::string url_contains, std::string body, int status = 200)
{
    ui::ReplayTransport::Rule rule;
    rule.url_contains = std::move(url_contains);
    rule.body = std::move(body);
    rule.status = status;
    rule.content_type = "application/json; charset=utf-8";
    return rule;
}

/// The modal boxes, recorded (no nested event loops).
struct DialogLog {
    struct ChooserCall {
        std::string typed;
        std::string role;
        std::vector<std::string> candidates;
        std::optional<std::string> hint;
        std::string busy_status;  ///< the busy line while the chooser is open
        bool busy = false;
    };

    std::vector<QString> errors;
    std::vector<QString> questions;
    std::vector<ChooserCall> chooser_calls;
    bool answer_big_search = false;
    std::optional<std::size_t> chooser_answer;

    [[nodiscard]] wizz::app::ControllerDialogs make(wizz::app::BusyPanel* busy)
    {
        return wizz::app::ControllerDialogs{
            .show_error = [this](const QString& text) { errors.push_back(text); },
            .confirm_big_search =
                [this](const QString& text) {
                    questions.push_back(text);
                    return answer_big_search;
                },
            .choose_place =
                [this, busy](std::string_view typed, std::string_view role, std::span<const Place> candidates,
                             const std::optional<std::string>& hint) {
                    ChooserCall call{.typed = std::string{typed},
                                     .role = std::string{role},
                                     .candidates = {},
                                     .hint = hint,
                                     .busy_status = busy->status_text(),
                                     .busy = busy->is_busy()};
                    for (const Place& place : candidates) {
                        call.candidates.push_back(place.description);
                    }
                    chooser_calls.push_back(std::move(call));
                    return chooser_answer;
                },
        };
    }
};

struct WindowRigOptions {
    std::string scenario = "named_destination";
    /// Replaces the scenario's rules when set.
    std::optional<std::vector<ui::ReplayTransport::Rule>> rules;
    bool keep_delays = false;
    ui::AppSettings settings{};
    std::optional<ui::AutoTestOptions> autotest;
    /// Wraps the replay transport (tests that inject failures).
    std::function<std::shared_ptr<http::Transport>(std::shared_ptr<http::Transport>)> wrap_transport;
};

class WindowRig {
public:
    explicit WindowRig(WindowRigOptions options = {})
    {
        EXPECT_TRUE(data_root_.isValid());
        replay_ = std::make_shared<ui::ReplayTransport>(
            options.rules ? std::move(*options.rules) : scenario_rules(options.scenario, options.keep_delays));
        std::shared_ptr<http::Transport> transport = replay_;
        if (options.wrap_transport) {
            transport = options.wrap_transport(transport);
        }
        wizz::app::ServiceConfig config;
        config.data_root = data_root();
        config.transport = std::move(transport);
        config.clock = clock_;
        config.locale = Locale::en_us();
        config.use_disk_cache = false;
        config.customize_client = [](WizzClientOptions& client) {
            client.request_spacing = Duration::zero();
            client.request_jitter = Duration::zero();
            client.delay = [](Duration, std::stop_token stop) { throw_if_cancelled(stop); };
        };
        config.geocoder_delay = [](Duration, std::stop_token stop) { throw_if_cancelled(stop); };
        services_ = wizz::app::AppServices::create(std::move(config));
        window_ = std::make_unique<wizz::app::MainWindow>(services_, options.settings, std::move(options.autotest));
        controller().set_dialogs(dialogs.make(window_->busy_panel()));
        controller().flows().set_error_handler([this](std::exception_ptr error) { flow_errors.push_back(error); });
        for (wizz::app::FlightGrid* grid : {window_->outbound_grid(), window_->return_grid()}) {
            grid->set_actions(actions.actions());
        }
        window_->round_trip_grid()->set_actions(actions.actions());
        QObject::connect(window_.get(), &wizz::app::MainWindow::startup_finished, [this] { started_ = true; });
    }

    WindowRig(const WindowRig&) = delete;
    WindowRig& operator=(const WindowRig&) = delete;
    ~WindowRig()
    {
        if (window_) {
            window_->hide();
        }
    }

    /// Shows the window and waits for OnShownAsync (the map load; the autotest script).
    void start(std::chrono::milliseconds timeout = kWaitCap)
    {
        window_->show_main_window();
        ASSERT_TRUE(gui::wait_until([this] { return started_; }, timeout)) << "start-up did not finish";
    }

    /// Runs a flow to its end (the event loop runs meanwhile).
    template <class T>
    T run(wizz::app::Task<T> task, std::chrono::milliseconds timeout = kWaitCap)
    {
        task.start();
        if (!gui::wait_until([&task] { return task.done(); }, timeout)) {
            throw std::runtime_error("the flow did not finish in time");
        }
        return task.result();
    }

    [[nodiscard]] wizz::app::MainWindow& window() { return *window_; }
    [[nodiscard]] wizz::app::Controller& controller() { return *window_->controller(); }
    [[nodiscard]] wizz::app::InputPanel& input() { return *window_->input_panel(); }
    [[nodiscard]] wizz::app::BusyPanel& busy() { return *window_->busy_panel(); }
    [[nodiscard]] wizz::app::FlightGrid& outbound() { return *window_->outbound_grid(); }
    [[nodiscard]] wizz::app::FlightGrid& inbound() { return *window_->return_grid(); }
    [[nodiscard]] wizz::app::RoundTripGrid& trips() { return *window_->round_trip_grid(); }
    [[nodiscard]] ui::ReplayTransport& replay() { return *replay_; }
    [[nodiscard]] const wizz::app::AppServices& services() const { return *services_; }
    [[nodiscard]] std::filesystem::path data_root() const
    {
        return std::filesystem::path{data_root_.path().toStdWString()};
    }
    [[nodiscard]] std::int64_t wizz_requests() const { return services_->wizz->request_count(); }

    /// The Log lines without their "[HH:mm:ss] " prefix.
    [[nodiscard]] std::vector<std::string> log_lines() const
    {
        std::vector<std::string> lines;
        const std::string& text = window_->log_view()->text();
        std::size_t start = 0;
        while (start < text.size()) {
            const std::size_t end = text.find("\r\n", start);
            const std::string line = text.substr(start, end - start);
            lines.push_back(line.size() > 11 ? line.substr(11) : line);
            if (end == std::string::npos) {
                break;
            }
            start = end + 2;
        }
        return lines;
    }
    [[nodiscard]] bool log_contains(std::string_view line) const
    {
        const std::vector<std::string> lines = log_lines();
        return std::ranges::find(lines, line) != lines.end();
    }

    [[nodiscard]] QString tab_text(wizz::app::MainWindow::Page page) const
    {
        return window_->tabs()->tabText(static_cast<int>(page));
    }

    DialogLog dialogs;
    gui::ActionRecorder actions;
    std::vector<std::exception_ptr> flow_errors;

private:
    QTemporaryDir data_root_;
    std::shared_ptr<FixedClock> clock_ = std::make_shared<FixedClock>();
    std::shared_ptr<ui::ReplayTransport> replay_;
    std::shared_ptr<const wizz::app::AppServices> services_;
    std::unique_ptr<wizz::app::MainWindow> window_;
    bool started_ = false;
};

} // namespace wizz::test::app
