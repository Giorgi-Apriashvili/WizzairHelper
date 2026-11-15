#pragma once
// "Which starting point?" / "Which destination?" (01 §11.5 PlaceChooserDialog):
// a fixed modal dialog (no minimise / maximise, owned by the main window and
// therefore not shown in the taskbar, centred on it) with client size
// 560 × 280 and padding 10:
//   * intro (top, wrapping, 6 px below): "Did you mean this place for “{typed}”?"
//     for one candidate, "Did you mean one of these places for “{typed}”? Pick
//     the {role}:" for more; with a hint, the hint on the line above;
//   * a list with one row per candidate (Place.Description), the first selected;
//     double-clicking a row is OK;
//   * bottom right, 8 px below the list: "Use this place" (OK, the default
//     button) and "Cancel", so Cancel is rightmost.
// OK gives the selected place; Cancel, Esc and the close box give nothing
// (Esc = Cancel, D-11). While it is open, the nested event loop keeps the busy
// panel animating.

#include "wizz/core/domain.hpp"

#include <QDialog>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class QLabel;
class QListWidget;
class QPushButton;

namespace wizz::app {

class PlaceChooserDialog final : public QDialog {
    Q_OBJECT

public:
    /// Client size L(560) × L(280) and padding 10 (01 §11.5).
    static constexpr int kClientWidth = 560;
    static constexpr int kClientHeight = 280;
    static constexpr int kPadding = 10;

    /// `role` is "starting point" or "destination"; `typed` is the trimmed text.
    PlaceChooserDialog(std::string_view typed, std::string_view role, std::span<const Place> candidates,
                       const std::optional<std::string>& hint, QWidget* parent = nullptr);

    /// Shows the chooser modally; the chosen place, or nullopt when cancelled.
    [[nodiscard]] static std::optional<Place> choose(QWidget* parent, std::string_view typed, std::string_view role,
                                                     std::span<const Place> candidates,
                                                     const std::optional<std::string>& hint);
    /// As choose(), but the index of the chosen candidate (the controller needs
    /// "chosen === Candidates[0]" for the "Interpreted …" log line).
    [[nodiscard]] static std::optional<std::size_t> choose_index(QWidget* parent, std::string_view typed,
                                                                 std::string_view role,
                                                                 std::span<const Place> candidates,
                                                                 const std::optional<std::string>& hint);

    /// The selected row, nullopt when no row is selected.
    [[nodiscard]] std::optional<std::size_t> selected_index() const;
    [[nodiscard]] std::optional<Place> selected_place() const;
    /// The result after exec(): the selected index when accepted, else nullopt.
    [[nodiscard]] std::optional<std::size_t> chosen_index() const;

    [[nodiscard]] QLabel* intro_label() const noexcept { return intro_; }
    [[nodiscard]] QListWidget* list() const noexcept { return list_; }
    [[nodiscard]] QPushButton* ok_button() const noexcept { return ok_; }
    [[nodiscard]] QPushButton* cancel_button() const noexcept { return cancel_; }

private:
    std::vector<Place> candidates_;
    QLabel* intro_ = nullptr;
    QListWidget* list_ = nullptr;
    QPushButton* ok_ = nullptr;
    QPushButton* cancel_ = nullptr;
};

} // namespace wizz::app
