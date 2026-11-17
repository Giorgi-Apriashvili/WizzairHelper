#pragma once
// The Settings window (DECISIONS D-62, D-63): a fixed, window-modal dialog owned by the
// main window (no minimise / maximise, no taskbar button), titled "Settings", with one
// card, "Updates":
//
//   ┌ (⟳) UPDATES ─────────────────────────────────────────────────┐
//   │ WizzairHelper 2.1.0 · last checked today at 15:53  [Check now] │
//   │ You have the latest version.                                   │
//   │                                                                │
//   │ [x] Check for updates automatically   [Once a day      v]      │
//   │                                                                │
//   │ When an update is found:                                       │
//   │ (•) Download it and install when I close the app               │
//   │ ( ) Download it, then ask me before installing                 │
//   │ ( ) Only tell me that an update is available                   │
//   │ (i) This portable copy cannot install updates itself; …        │
//   └────────────────────────────────────────────────────────────────┘
//                                                           [ Close ]
//
// It renders two values: the options (update::UpdateOptions, from settings.json) and the
// status (ui::UpdateStatusView, from the update controller, or ui::updates_off_view() in a
// run without updates). It reports what the user does and changes nothing itself:
// options_changed() after every change of the check box, the drop-down or the choice
// (they apply at once; the main window saves settings.json), check_now_requested() for
// Check now. show_status() follows the controller while the window is open, so a check or
// download shows live; Check now is disabled and reads "Checking…" while it runs.
//
//   * The status line takes the theme's success colour when up to date, the failure
//     colour when something failed, the secondary text colour while checking or
//     downloading, the muted colour in a run without updates, else the text colour; it
//     wraps and keeps room for two lines, so the window keeps its size for every usual
//     status (a longer text grows it).
//   * The drop-down is enabled only while automatic checks are on; the three choices are
//     disabled for a portable copy, with the note under them; everything but Close is
//     disabled in a run without updates (--autotest, --replay).
//   * Close is the default (primary) button; Esc and the title bar's close box close too.
//   * Tab order: Check now, the check box, the drop-down, the three choices, Close. Focus
//     starts on the first of them that is enabled.
//   * Look (D-53): the window colour behind a SectionCard, the UI font, ModernStyle's
//     controls (radio buttons included), every colour taken from the theme again when the
//     look changes; High Contrast uses the plain native style and the system colours.
//     Labels are plain text (D-58). The width is fixed (kClientWidth); the height fits the
//     contents and is computed again when a text or the fonts change.

#include "wizz/services/update_options.hpp"
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
    /// Between the groups inside the card (status, automatic checks, the choices).
    static constexpr int kGroupGap = 12;

    SettingsDialog(const update::UpdateOptions& options, const ui::UpdateStatusView& status,
                   QWidget* parent = nullptr);

    /// Shows `status`: the version line, the status line and its colour, the busy and
    /// enabled states, the portable note.
    void show_status(const ui::UpdateStatusView& status);
    /// Shows `options` without reporting them.
    void show_options(const update::UpdateOptions& options);
    /// The options the controls show.
    [[nodiscard]] update::UpdateOptions options() const;
    [[nodiscard]] const ui::UpdateStatusView& status() const noexcept { return status_; }

    // --- Parts (tests, screenshots) -----------------------------------------------------
    [[nodiscard]] SectionCard* updates_card() const noexcept { return card_; }
    [[nodiscard]] QLabel* version_label() const noexcept { return version_; }
    [[nodiscard]] QLabel* status_label() const noexcept { return status_line_; }
    [[nodiscard]] QPushButton* check_now_button() const noexcept { return check_now_; }
    [[nodiscard]] QCheckBox* check_automatically_box() const noexcept { return automatic_; }
    [[nodiscard]] QComboBox* frequency_box() const noexcept { return frequency_; }
    [[nodiscard]] QLabel* when_found_caption() const noexcept { return when_found_caption_; }
    [[nodiscard]] QWidget* when_found_group() const noexcept { return when_found_group_; }
    [[nodiscard]] QRadioButton* when_found_button(update::WhenFound when_found) const;
    [[nodiscard]] QLabel* note_label() const noexcept { return note_; }
    [[nodiscard]] QPushButton* close_button() const noexcept { return close_; }
    /// The status line's colour for `kind` in the current look.
    [[nodiscard]] static QColor status_colour(ui::UpdateStatusKind kind);

Q_SIGNALS:
    /// The user changed an option; `options` are all of them as the controls show them.
    void options_changed(const wizz::update::UpdateOptions& options);
    /// Check now was pressed.
    void check_now_requested();

protected:
    void changeEvent(QEvent* event) override;

private:
    void report_options();
    void apply_status_colour();
    void update_enabled_states();
    /// Fixes the size: kClientWidth wide, as tall as the layout needs at that width.
    void fit_size();

    ui::UpdateStatusView status_;
    SectionCard* card_ = nullptr;
    QLabel* version_ = nullptr;
    QLabel* status_line_ = nullptr;
    QPushButton* check_now_ = nullptr;
    QCheckBox* automatic_ = nullptr;
    QComboBox* frequency_ = nullptr;
    QLabel* when_found_caption_ = nullptr;
    QWidget* when_found_group_ = nullptr;
    QButtonGroup* when_found_ = nullptr;
    QWidget* note_row_ = nullptr;
    QLabel* note_ = nullptr;
    QPushButton* close_ = nullptr;
    bool showing_ = false;
    bool fitting_ = false;
};

} // namespace wizz::app
