#pragma once
// The Settings window (DECISIONS D-62, D-63, D-68, D-73, D-74): a fixed, window-modal dialog owned by
// the main window (no minimise / maximise, no taskbar button), titled "Settings", with two
// cards, "Updates" and "Searches":
//
//   ┌ (⟳) UPDATES ─────────────────────────────────────────────────┐
//   │ WizzairHelper 2.1.0 · last checked today at 15:53  [Check now] │
//   │ You have the latest version.                                   │
//   │   — or, with an update on offer (D-74):                        │
//   │ WizzairHelper 2.2.0 is ready to install. [Restart and update]  │
//   │                                                                │
//   │ [x] Check for updates automatically   [Once a day      v]      │
//   │                                                                │
//   │ When an update is found:                                       │
//   │ (•) Download it and install when I close the app               │
//   │ ( ) Download it, then ask me before installing                 │
//   │ ( ) Only tell me that an update is available                   │
//   │ (i) This portable copy cannot install updates itself; …        │
//   └────────────────────────────────────────────────────────────────┘
//   ┌ (⌚) SEARCHES ────────────────────────────────────────────────┐
//   │ Reuse fares fetched within  [30 minutes v]                     │
//   │ Searches ask Wizz Air only for days not fetched within this …  │
//   │ Homes on Airbnb within  [5 km v]  of the city centre           │
//   └────────────────────────────────────────────────────────────────┘
//                                                           [ Close ]
//
// It renders four values: the options (update::UpdateOptions, from settings.json), the
// status (ui::UpdateStatusView, from the update controller, or ui::updates_off_view() in a
// run without updates), the reuse window of fetched fares (settings.json's
// ReuseFaresWithin, D-68) and how far "Homes on Airbnb" reaches (HomesWithin, D-73). It
// reports what the user does and changes nothing itself: options_changed() after every
// change of the check box, the drop-down or the choice, fare_reuse_changed() and
// homes_radius_changed() after a change of theirs (they apply at once; the main window
// saves settings.json), check_now_requested() for Check now, update_action_requested() for
// the update's button. show_status() follows the controller while the window is open, so a
// check or download shows live; Check now is disabled and reads "Checking…" while it runs.
//
//   * The update's button (D-74) shows the update bar's action — "Restart and update",
//     "Update now" or "Download" (ui::UpdateStatusView::action) — right under Check now,
//     right-aligned, beside the status line, whose text is centred on it. It is hidden
//     when nothing is on offer and while the status says "Downloading…"; "Restart and
//     update" and "Update now" are disabled while a search runs, as in the bar. So the
//     update Check now found is installed from here, not from the bar behind the window.
//     It costs no height: it fits in the room the status line keeps.
//
//   * The status line takes the theme's success colour when up to date, the failure
//     colour when something failed, the secondary text colour while checking or
//     downloading, the muted colour in a run without updates, else the text colour; it
//     wraps and keeps room for two lines, so the window keeps its size for every usual
//     status (a longer text grows it).
//   * The drop-down is enabled only while automatic checks are on; the three choices are
//     disabled for a portable copy, with the note under them; everything of "Updates" is
//     disabled in a run without updates (--autotest, --replay). The reuse window and the
//     homes distance are always enabled.
//   * Close is the default (primary) button; Enter clicks the focused push button (Check
//     now, the update's button or Close), else Close; Esc and the title bar's close box
//     close too.
//   * Tab order: Check now, the update's button, the check box, the drop-down, the selected choice (the arrow
//     keys move among the three and select — and so apply — each one, as an exclusive
//     radio group does), the reuse window, the homes distance, Close. Focus starts on the
//     first of Check now, the check box and the reuse window that is enabled (the reuse
//     window in a run without updates) — never on the update's button, so an Enter right
//     after opening cannot restart the app. When
//     Check now is disabled under the focus (its check or a download runs), the focus
//     waits on the status line and returns to Check now when it is enabled again; so does
//     the focus of the update's button when that hides or is disabled ("Update now" starts
//     its download).
//   * Look (D-53): the window colour behind a SectionCard, the UI font, ModernStyle's
//     controls (radio buttons included), every colour taken from the theme again when the
//     look changes; High Contrast uses the plain native style and the system colours.
//     Labels are plain text (D-58). The width is fixed (kClientWidth); the height fits the
//     contents and is computed again when a text or the fonts change.

#include "wizz/core/fare_days.hpp"
#include "wizz/services/update_options.hpp"
#include "wizz/ui/homes_stay.hpp"
#include "wizz/ui/update_model.hpp"

#include <QDialog>
#include <QMargins>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QRadioButton;

namespace wizz::app {

class GlyphLabel;
class SectionCard;

class SettingsDialog final : public QDialog {
    Q_OBJECT

public:
    /// The fixed client width in logical pixels; the height follows the contents.
    static constexpr int kClientWidth = 600;
    /// Around the card and the Close button.
    static constexpr QMargins kMargins{16, 16, 16, 16};
    /// Between the card and the row of Close.
    static constexpr int kButtonsGap = 12;
    /// Between the groups inside the card (status, automatic checks, the choices), and
    /// between the two cards.
    static constexpr int kGroupGap = 12;
    /// Between Check now and the update's button under it (D-74).
    static constexpr int kActionGap = 6;

    SettingsDialog(const update::UpdateOptions& options, const ui::UpdateStatusView& status,
                   fare_days::ReuseWindow fare_reuse, QWidget* parent = nullptr);

    /// Shows `status`: the version line, the status line and its colour, the busy and
    /// enabled states, the portable note.
    void show_status(const ui::UpdateStatusView& status);
    /// Shows `options` without reporting them.
    void show_options(const update::UpdateOptions& options);
    /// The options the controls show.
    [[nodiscard]] update::UpdateOptions options() const;
    /// Shows `window` without reporting it.
    void show_fare_reuse(fare_days::ReuseWindow window);
    /// The reuse window the drop-down shows.
    [[nodiscard]] fare_days::ReuseWindow fare_reuse() const;
    /// Shows `radius` without reporting it (5 km until the main window shows its own).
    void show_homes_radius(ui::HomesRadius radius);
    /// The homes distance the drop-down shows.
    [[nodiscard]] ui::HomesRadius homes_radius() const;
    [[nodiscard]] const ui::UpdateStatusView& status() const noexcept { return status_; }

    // --- Parts (tests, screenshots) -----------------------------------------------------
    [[nodiscard]] SectionCard* updates_card() const noexcept { return card_; }
    [[nodiscard]] QLabel* version_label() const noexcept { return version_; }
    [[nodiscard]] QLabel* status_label() const noexcept { return status_line_; }
    [[nodiscard]] QPushButton* check_now_button() const noexcept { return check_now_; }
    /// "Restart and update", "Update now" or "Download" under Check now (D-74).
    [[nodiscard]] QPushButton* update_action_button() const noexcept { return update_action_; }
    [[nodiscard]] QCheckBox* check_automatically_box() const noexcept { return automatic_; }
    [[nodiscard]] QComboBox* frequency_box() const noexcept { return frequency_; }
    [[nodiscard]] QLabel* when_found_caption() const noexcept { return when_found_caption_; }
    [[nodiscard]] QWidget* when_found_group() const noexcept { return when_found_group_; }
    [[nodiscard]] QRadioButton* when_found_button(update::WhenFound when_found) const;
    [[nodiscard]] QLabel* note_label() const noexcept { return note_; }
    [[nodiscard]] SectionCard* searches_card() const noexcept { return searches_card_; }
    [[nodiscard]] QLabel* reuse_label() const noexcept { return reuse_label_; }
    [[nodiscard]] QComboBox* reuse_box() const noexcept { return reuse_; }
    [[nodiscard]] QLabel* reuse_hint() const noexcept { return reuse_hint_; }
    [[nodiscard]] QLabel* homes_label() const noexcept { return homes_label_; }
    [[nodiscard]] QComboBox* homes_box() const noexcept { return homes_; }
    [[nodiscard]] QLabel* homes_suffix() const noexcept { return homes_suffix_; }
    [[nodiscard]] QPushButton* close_button() const noexcept { return close_; }
    /// The status line's colour for `kind` in the current look.
    [[nodiscard]] static QColor status_colour(ui::UpdateStatusKind kind);

Q_SIGNALS:
    /// The user changed an option; `options` are all of them as the controls show them.
    void options_changed(const wizz::update::UpdateOptions& options);
    /// Check now was pressed.
    void check_now_requested();
    /// The update's button was pressed (the update bar's action, D-74).
    void update_action_requested();
    /// The user chose another reuse window of fetched fares.
    void fare_reuse_changed(wizz::fare_days::ReuseWindow window);
    /// The user chose another distance for "Homes on Airbnb".
    void homes_radius_changed(wizz::ui::HomesRadius radius);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void report_options();
    void apply_status_colour();
    void update_enabled_states();
    /// Shows or hides the update's button.
    void show_update_action();
    /// Beside the update's button, the status's text centred on it (within its two lines).
    void centre_status_on_action();
    /// Fixes the size: kClientWidth wide, as tall as the layout needs at that width.
    void fit_size();

    ui::UpdateStatusView status_;
    SectionCard* card_ = nullptr;
    QLabel* version_ = nullptr;
    QLabel* status_line_ = nullptr;
    QPushButton* check_now_ = nullptr;
    QPushButton* update_action_ = nullptr;
    QCheckBox* automatic_ = nullptr;
    QComboBox* frequency_ = nullptr;
    QLabel* when_found_caption_ = nullptr;
    QWidget* when_found_group_ = nullptr;
    QButtonGroup* when_found_ = nullptr;
    QWidget* note_row_ = nullptr;
    QLabel* note_ = nullptr;
    SectionCard* searches_card_ = nullptr;
    QLabel* reuse_label_ = nullptr;
    QComboBox* reuse_ = nullptr;
    QLabel* reuse_hint_ = nullptr;
    QLabel* homes_label_ = nullptr;
    QComboBox* homes_ = nullptr;
    QLabel* homes_suffix_ = nullptr;
    QPushButton* close_ = nullptr;
    bool showing_ = false;
    bool fitting_ = false;
    /// Check now (or the update's button) had the focus when it was disabled or hidden: the
    /// status line holds it until Check now is enabled.
    bool check_now_focus_parked_ = false;
};

} // namespace wizz::app
