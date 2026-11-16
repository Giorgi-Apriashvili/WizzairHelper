#pragma once
// The update bar (D-46): a slim, non-modal strip between the input panel and the
// result tabs — a deliberate addition to the 01 §4.2 layout — hidden unless an
// update is ready (installed copy) or available (portable copy).
//
//   [▌] WizzairHelper 1.2.0 is ready to install. It will be …   [Restart and update]  Later
//
// Look: the window's form font (Segoe UI 9 pt) in ControlText on a light tint of
// the Wizz magenta, a 4 px magenta accent at the left edge, the action as a
// magenta search-style button (01 §4.5) and "Later" as a small flat button.
// Padding (10 + accent, 4, 8, 4). The message wraps when the window is too narrow,
// so nothing is clipped at 100–200 %.
//
// Showing the bar never moves the keyboard focus; its two buttons follow the input
// panel in the Tab order while it is visible. It renders ui::UpdateBarView and
// reports clicks; the update controller decides what they mean. UI thread only.

#include "wizz/ui/update_model.hpp"

#include <QColor>
#include <QMargins>
#include <QWidget>

class QLabel;
class QPushButton;

namespace wizz::app {

class SearchButton;

class UpdateBar final : public QWidget {
    Q_OBJECT

public:
    /// Width of the magenta accent at the left edge.
    static constexpr int kAccentWidth = 4;
    /// Padding around the contents, the accent not included.
    static constexpr QMargins kPadding{10, 4, 8, 4};
    /// Space between the message, the action and "Later".
    static constexpr int kSpacing = 8;

    /// Hidden until show_view() gets a visible view.
    explicit UpdateBar(QWidget* parent = nullptr);

    /// Shows `view`: message, action text and enabled state, visibility. Never takes focus.
    void show_view(const ui::UpdateBarView& view);
    [[nodiscard]] const ui::UpdateBarView& view() const noexcept { return view_; }

    [[nodiscard]] QLabel* message_label() const noexcept { return message_; }
    /// "Restart and update" or "Download".
    [[nodiscard]] SearchButton* action_button() const noexcept { return action_; }
    [[nodiscard]] QPushButton* later_button() const noexcept { return later_; }

    /// The light magenta tint behind the message.
    [[nodiscard]] static QColor background_colour();

Q_SIGNALS:
    void action_clicked();
    void later_clicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void apply_text_colour();

    ui::UpdateBarView view_;
    QLabel* message_ = nullptr;
    SearchButton* action_ = nullptr;
    QPushButton* later_ = nullptr;
};

} // namespace wizz::app
