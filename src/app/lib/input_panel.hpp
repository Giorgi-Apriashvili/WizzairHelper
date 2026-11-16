#pragma once
// The input panel at the top of the main window (01 §4.2-§4.5, §6.1-§6.4) in the
// look of DECISIONS D-53: the inputs are grouped into four titled cards
// (SectionCard) in a 2 x 2 grid, read left to right, top to bottom:
//
//   ┌ WHERE  (empty “To” = anywhere) ──────────┐  ┌ TRAVELLERS & PRICES ───────────────┐
//   │ From [_from        ] within [_fromRadius] km│  │ Adults [_adults]  [_fare          ] │
//   │ To   [_to          ] within [_toRadius  ] km│  │ Prices in [_currency]              │
//   └──────────────────────────────────────────────┘  └────────────────────────────────────┘
//   ┌ OUTBOUND ────────────────────────────────────┐  ┌ RETURN  Return flights go from … ──┐
//   │ between [_outFrom] and [_outTo]              │  │ between [_retFrom] and [_retTo]    │
//   │                           [Search outbound]  │  │ Round trips: stay [_minNights] to  │
//   └──────────────────────────────────────────────┘  │ [_maxNights] nights [Search return]│
//                                                      └────────────────────────────────────┘
//   then the muted, wrapping `_resolved` line.
//
// The place boxes stretch with the window; the other inputs keep their width.
// At narrow widths the Search return button moves under the night limits
// (TrailingRowLayout), so the minimum window (980 x 600) clips nothing. Both
// search buttons sit at the bottom right of their cards, on one line: the second
// row of cards is as tall as the Return card's two rows anyway (D-53).
//
// The date and nights coupling (01 §6.1, §6.2) is ui::DateRules: the panel
// forwards every picker / nights change (user or code) to the model and shows the
// model's values again. The rules are wired after the defaults are set, so start-up
// and load_from() trigger nothing. Dates are not persisted; the rest is loaded
// from and stored to AppSettings with the 01 §4.4 / §13 clamps and truncation.
//
// Keyboard (01 §6.3): the panel tracks which control group gained focus and what
// Enter (the window's AcceptButton) presses: Search outbound for the place,
// radius, outbound date, adults, fare and currency boxes; Search return for the
// return dates; nothing for the night boxes; other controls keep the last
// mapping. The tab order follows the cards: origin, radius, destination, radius,
// adults, fare, currency, outbound dates, Search outbound, return dates, night
// limits, Search return (01 §6.3 had the outbound dates before adults; D-54).

#include "wizz/core/domain.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/core/time.hpp"
#include "wizz/ui/date_rules.hpp"
#include "wizz/ui/return_request.hpp"
#include "wizz/ui/settings.hpp"

#include <QString>
#include <QWidget>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class QComboBox;
class QLabel;

namespace wizz::app {

class DatePicker;
class FlowLayout;
class FractionalSpinBox;
class PlaceCombo;
class SearchButton;

class InputPanel final : public QWidget {
    Q_OBJECT

public:
    /// The control group that gained focus (focus_group_changed argument).
    enum class FocusGroup : int {
        outbound = 0,  ///< places, radii, outbound dates, adults, fare, currency
        inbound = 1,   ///< return dates
        nights = 2,    ///< night limits
        other = 3,     ///< the search buttons
    };
    /// What Enter (the form's AcceptButton) presses.
    enum class AcceptTarget { search_outbound, search_return, none };

    /// Builds the panel with the defaults of 01 §4.4 for `today` (dates) and
    /// `settings` (everything else), then wires the coupling rules.
    InputPanel(std::shared_ptr<const Locale> locale, Date today, const ui::AppSettings& settings = {},
               QWidget* parent = nullptr);
    ~InputPanel() override;
    InputPanel(const InputPanel&) = delete;
    InputPanel& operator=(const InputPanel&) = delete;

    // --- Settings (01 §4.4, §13) --------------------------------------------------
    /// Applies the persisted inputs with their clamps (unknown currency → EUR, max
    /// nights ≥ min nights). No coupling handler runs and no signal is emitted.
    void load_from(const ui::AppSettings& settings);
    /// Stores From/To (trimmed), the radii, adults and nights (truncated toward
    /// zero), DiscountClub and Currency. Window fields are left alone. The numbers are
    /// the committed values: call commit_numeric_edits() first.
    void store_to(ui::AppSettings& settings) const;
    /// Commits text typed into a numeric box but not yet entered (no Enter, no focus
    /// loss), in the 01 §13 save order — what reading NumericUpDown.Value does. The
    /// usual handlers run (a night limit drags the other one along, 01 §6.2).
    void commit_numeric_edits();

    // --- Inputs ------------------------------------------------------------------------
    [[nodiscard]] std::string from_text() const;
    [[nodiscard]] std::string to_text() const;
    /// Radii keep typed fractions (01 §4.4).
    [[nodiscard]] double from_radius_km() const;
    [[nodiscard]] double to_radius_km() const;
    /// Truncated toward zero.
    [[nodiscard]] int adults() const;
    [[nodiscard]] int fare_index() const;
    [[nodiscard]] PriceType price_type() const;
    [[nodiscard]] std::string currency() const;
    [[nodiscard]] Date out_from() const { return rules_.out_from(); }
    [[nodiscard]] Date out_to() const { return rules_.out_to(); }
    [[nodiscard]] Date ret_from() const { return rules_.ret_from(); }
    [[nodiscard]] Date ret_to() const { return rules_.ret_to(); }
    /// Night filters, truncated toward zero.
    [[nodiscard]] int min_nights() const { return rules_.min_nights(); }
    [[nodiscard]] int max_nights() const { return rules_.max_nights(); }
    /// The values a search reads (01 §11.4).
    [[nodiscard]] ui::SearchInputs search_inputs() const;
    [[nodiscard]] const ui::DateRules& date_rules() const noexcept { return rules_; }

    void set_from_text(std::string_view text);
    void set_to_text(std::string_view text);
    /// Clamped to 0..1500.
    void set_from_radius_km(double km);
    void set_to_radius_km(double km);
    /// Clamped to 1..9.
    void set_adults(double adults);
    /// 0 = Regular fares, 1 = Wizz Discount Club fares.
    void set_fare_index(int index);
    /// Selects `code` when it is one of the 21 listed codes (exact, case-sensitive);
    /// returns false and changes nothing otherwise. Emits currency_changed on a change.
    bool set_currency(std::string_view code);
    /// Date assignments as code changes: clamped, and the 01 §6.1 handlers run.
    void set_out_from(Date date);
    void set_out_to(Date date);
    void set_ret_from(Date date);
    void set_ret_to(Date date);
    /// Night limits as code changes (01 §6.2 coupling; nights_changed on a change).
    void set_min_nights(double nights);
    void set_max_nights(double nights);
    /// The right-click return window (01 §11.6): outbound date + [min, max] nights.
    void plan_return_window(Date outbound_date);
    /// The autotest date order (01 §17.2): outTo, outFrom, retTo, retFrom.
    void apply_autotest_dates(const std::optional<ui::DateWindow>& outbound,
                              const std::optional<ui::DateWindow>& inbound);

    // --- Map, busy state, resolved line ---------------------------------------------
    /// Both place boxes get `names` as drop-down items and autocomplete source; the
    /// typed texts are kept (01 §11.2).
    void set_suggestions(std::span<const std::string> names);
    /// `SetWorking`'s enable list (01 §6.4): the search buttons, place and radius
    /// boxes, the four dates, adults, fare and currency; the night boxes stay enabled.
    void set_working(bool working);
    [[nodiscard]] bool is_working() const noexcept { return working_; }
    void set_resolved_text(std::string_view text);
    [[nodiscard]] QString resolved_text() const;

    // --- Keyboard (01 §6.3) -------------------------------------------------------------
    /// Focuses "From" (start-up, missing-origin error).
    void focus_from();
    [[nodiscard]] AcceptTarget accept_target() const noexcept { return accept_target_; }
    /// The group of a widget inside the panel (other for anything unmapped).
    [[nodiscard]] FocusGroup focus_group_of(const QWidget* widget) const;
    /// Presses the AcceptButton: emits search_outbound_requested or
    /// search_return_requested for the current mapping when that button is enabled.
    /// Returns whether a signal was emitted.
    bool trigger_accept();

    // --- Widgets (main window wiring and tests) ----------------------------------------
    [[nodiscard]] PlaceCombo* from_box() const noexcept { return from_; }
    [[nodiscard]] PlaceCombo* to_box() const noexcept { return to_; }
    [[nodiscard]] FractionalSpinBox* from_radius_box() const noexcept { return from_radius_; }
    [[nodiscard]] FractionalSpinBox* to_radius_box() const noexcept { return to_radius_; }
    [[nodiscard]] DatePicker* out_from_picker() const noexcept { return out_from_; }
    [[nodiscard]] DatePicker* out_to_picker() const noexcept { return out_to_; }
    [[nodiscard]] DatePicker* ret_from_picker() const noexcept { return ret_from_; }
    [[nodiscard]] DatePicker* ret_to_picker() const noexcept { return ret_to_; }
    [[nodiscard]] FractionalSpinBox* adults_box() const noexcept { return adults_; }
    [[nodiscard]] QComboBox* fare_box() const noexcept { return fare_; }
    [[nodiscard]] QComboBox* currency_box() const noexcept { return currency_; }
    [[nodiscard]] FractionalSpinBox* min_nights_box() const noexcept { return min_nights_; }
    [[nodiscard]] FractionalSpinBox* max_nights_box() const noexcept { return max_nights_; }
    [[nodiscard]] SearchButton* search_outbound_button() const noexcept { return search_out_; }
    [[nodiscard]] SearchButton* search_return_button() const noexcept { return search_ret_; }
    [[nodiscard]] QLabel* resolved_label() const noexcept { return resolved_; }
    /// The section cards 0..3: Where, Travellers & prices, Outbound, Return.
    [[nodiscard]] QWidget* row(int index) const;
    /// The grouped layout has no wrapping FlowLayout rows: always null (kept for the GUI
    /// tests of the old three-row layout until they move to the cards).
    [[nodiscard]] FlowLayout* row_layout(int index) const;
    /// The controls SetWorking disables, in the 01 §6.4 order.
    [[nodiscard]] std::vector<QWidget*> working_controls() const;
    /// The tab order of 01 §6.3.
    [[nodiscard]] std::vector<QWidget*> tab_order() const;

Q_SIGNALS:
    void search_outbound_requested();
    void search_return_requested();
    /// The "Prices in" selection changed (user or code; 01 §11.7).
    void currency_changed(const QString& currency);
    /// A night filter changed (RecomputeTrips, 01 §6.2).
    void nights_changed();
    /// Focus entered a control of `group` (a FocusGroup value; 01 §6.3).
    void focus_group_changed(int group);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum class DateField { out_from, out_to, ret_from, ret_to };

    void build();
    void wire();
    void apply_tab_order();
    void on_picker_changed(DateField field, Date value);
    void on_nights_changed(bool minimum, double value);
    void on_focus_entered(FocusGroup group);
    /// Shows the model's dates / nights in the widgets (handlers suppressed).
    void sync_pickers();
    void sync_nights();

    std::shared_ptr<const Locale> locale_;
    ui::DateRules rules_;
    bool syncing_ = false;
    bool working_ = false;
    AcceptTarget accept_target_ = AcceptTarget::search_outbound;

    QWidget* rows_[4]{};
    PlaceCombo* from_ = nullptr;
    FractionalSpinBox* from_radius_ = nullptr;
    PlaceCombo* to_ = nullptr;
    FractionalSpinBox* to_radius_ = nullptr;
    DatePicker* out_from_ = nullptr;
    DatePicker* out_to_ = nullptr;
    FractionalSpinBox* adults_ = nullptr;
    QComboBox* fare_ = nullptr;
    QComboBox* currency_ = nullptr;
    SearchButton* search_out_ = nullptr;
    DatePicker* ret_from_ = nullptr;
    DatePicker* ret_to_ = nullptr;
    FractionalSpinBox* min_nights_ = nullptr;
    FractionalSpinBox* max_nights_ = nullptr;
    SearchButton* search_ret_ = nullptr;
    QLabel* resolved_ = nullptr;
};

} // namespace wizz::app
