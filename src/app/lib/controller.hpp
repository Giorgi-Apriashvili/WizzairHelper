#pragma once
// Controller: the MainForm flows of 01 §11 (and the Log lines of §12.2, the
// tab titles of §5, SetWorking of §6.4) on the UI thread.
//
// Each .NET async method is a coroutine (task.hpp) with the same steps in the
// same order: LoadCatalogAsync, PrepareAsync (with ResolveAsync and the place
// chooser), SearchOutboundAsync / SearchReturnAsync, PlanReturnFromAsync,
// RunLegAsync, plus the synchronous OnCurrencyChanged, RecomputeTrips and
// UpdateTabTitles. Texts, decisions and request building come from
// wizz::ui (leg_text, return_request, leg_stream, round_trip_rows, strings);
// work runs on JobRunner workers and reports through its ordered channel.
//
// Busy phases (01 §7): each phase sets `_working`, owns a std::stop_source
// (`_cts`, cancelled by Cancel / Esc / closing the window) and ends the busy
// panel on every path; a `finally` guard disposes the stop source, clears
// `_working` and ends a still-busy panel with UnexpectedErrorText (UI-3).
// `_working` is also the re-entrancy guard of the three search entry points.
// Between phases the flows continue synchronously (no event-loop round trip),
// so no second search can slip in.
//
// UI thread only. The widgets belong to MainWindow and outlive the controller.

#include "app_services.hpp"
#include "controller_dialogs.hpp"
#include "job_runner.hpp"
#include "task.hpp"
#include "task_scope.hpp"

#include "wizz/core/catalog.hpp"
#include "wizz/core/domain.hpp"
#include "wizz/core/time.hpp"

#include <QObject>
#include <QString>

#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

class QTabWidget;
class QWidget;

namespace wizz::ui {
class LegStream;
}

namespace wizz::app {

class BusyPanel;
class FlightGrid;
class InputPanel;
class LogView;
class RoundTripGrid;

/// The widgets the flows drive (owned by MainWindow).
struct MainViews {
    InputPanel* input = nullptr;
    QTabWidget* tabs = nullptr;
    FlightGrid* outbound = nullptr;
    FlightGrid* inbound = nullptr;
    RoundTripGrid* trips = nullptr;
    LogView* log = nullptr;
    BusyPanel* busy = nullptr;
};

class Controller final : public QObject {
    Q_OBJECT

public:
    enum class Leg { outbound, inbound };

    /// `autotest` switches the 01 §17.1 differences (no chooser, no "Big search"
    /// question, no Fail / cap / leg-error boxes).
    Controller(std::shared_ptr<const AppServices> services, MainViews views, bool autotest, QObject* parent = nullptr);
    ~Controller() override;

    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    /// The modal boxes (ControllerDialogs::system(main window) by default).
    void set_dialogs(ControllerDialogs dialogs);

    // --- Flows (01 §11) -------------------------------------------------------------------
    /// LoadCatalogAsync (§11.2): the route map, loaded once; null after a failure.
    [[nodiscard]] Task<AirportCatalogPtr> load_catalog();
    /// SearchOutboundAsync (§11.3): Prepare, then RunLeg into the Outbound tab.
    [[nodiscard]] Task<> search_outbound();
    /// SearchReturnAsync (§11.3): `override_origin` is the right-click start (one search only).
    [[nodiscard]] Task<> search_return(std::optional<Place> override_origin = std::nullopt);
    /// PlanReturnFromAsync (§11.6): return window = outbound date + nights, then the return
    /// search from the fare's arrival airport.
    [[nodiscard]] Task<> plan_return_from(FlightResultPtr outbound);
    /// OnCurrencyChanged (§11.7): re-prices the stored legs without requests.
    void on_currency_changed();
    /// RecomputeTrips (§11.8).
    void recompute_trips();
    /// UpdateTabTitles (§5).
    void update_tab_titles();
    /// Log(line) (§12).
    void log(std::string_view line);

    /// Starts a root flow (button, menu, start-up); escaping exceptions go to the
    /// crash reporter and the app keeps running (01 §3.2).
    void spawn(Task<> flow);

    // --- State ----------------------------------------------------------------------------
    /// `_working` (01 §6.4).
    [[nodiscard]] bool is_working() const noexcept { return working_; }
    /// A busy phase owns a stop source right now.
    [[nodiscard]] bool has_active_phase() const noexcept { return stop_source_.has_value(); }
    /// `_cts?.Cancel()` (Cancel button, Esc, closing the window).
    void cancel_work();
    [[nodiscard]] const AirportCatalogPtr& catalog() const noexcept { return catalog_; }
    [[nodiscard]] const LegSearchResultPtr& outbound_result() const noexcept { return outbound_result_; }
    [[nodiscard]] const LegSearchResultPtr& return_result() const noexcept { return return_result_; }
    [[nodiscard]] bool is_autotest() const noexcept { return autotest_; }
    [[nodiscard]] const AppServices& services() const noexcept { return *services_; }
    [[nodiscard]] JobRunner& jobs() noexcept { return *jobs_; }
    [[nodiscard]] TaskScope& flows() noexcept { return *flows_; }
    [[nodiscard]] const MainViews& views() const noexcept { return views_; }

public Q_SLOTS:
    /// The Search outbound button / Enter mapping.
    void request_search_outbound();
    /// The Search return button / Enter mapping (never with an override).
    void request_search_return();
    /// The Outbound grid's "Search return flights from this destination".
    void request_plan_return(wizz::FlightResultPtr fare);

private:
    /// `finally` of a busy phase.
    class PhaseGuard;
    /// ResolveAsync's answer: a place, or the Fail message ("" = chooser cancelled).
    struct Resolved {
        std::optional<Place> place;
        std::string error;
    };

    [[nodiscard]] Task<std::optional<LegSearchRequest>> prepare(bool is_return, std::optional<Place> override_origin);
    [[nodiscard]] Task<Resolved> resolve(std::string text, std::string_view role, AirportCatalogPtr catalog,
                                         std::stop_token stop);
    [[nodiscard]] Task<> run_leg(LegSearchRequest request, Leg leg);

    /// PrepareAsync's Fail(message).
    void fail(std::string_view message);
    /// A leg exception (WizzApiException / HttpRequestException / IOException).
    void leg_failed(const LegSearchRequest& request, ui::LegStream& stream, std::string_view message);
    /// The 150 ms flush of a leg run.
    void flush(ui::LegStream& stream, FlightGrid& grid);
    void reprice(LegSearchResultPtr& result, FlightGrid& grid, const std::string& currency);

    /// `_cts = new CTS`: the token of the new phase.
    [[nodiscard]] std::stop_token begin_phase();
    /// `finally`: dispose the stop source, SetWorking(false), End(UnexpectedErrorText) if busy.
    void end_phase() noexcept;
    void set_working(bool working);
    void store(Leg leg, LegSearchResultPtr result);
    void select_tab(Leg leg);
    void show_error(std::string_view text) const;
    [[nodiscard]] FlightGrid& grid_of(Leg leg) const noexcept;
    [[nodiscard]] Date today() const;
    [[nodiscard]] const Locale& locale() const noexcept { return *services_->locale; }

    std::shared_ptr<const AppServices> services_;
    MainViews views_;
    bool autotest_;
    ControllerDialogs dialogs_;

    bool working_ = false;
    bool shutting_down_ = false;
    std::optional<std::stop_source> stop_source_;
    AirportCatalogPtr catalog_;
    LegSearchResultPtr outbound_result_;
    LegSearchResultPtr return_result_;

    std::unique_ptr<JobRunner> jobs_;
    std::unique_ptr<TaskScope> flows_;
};

} // namespace wizz::app
