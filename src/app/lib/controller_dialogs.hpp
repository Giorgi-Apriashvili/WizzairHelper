#pragma once
// The modal boxes the main-window flows show (01 §11.4, §11.5, §11.9, §18.1).
// They are injectable so widget tests never block in a nested event loop.
//
//   show_error         caption "WizzairHelper", Information icon, OK, owned by the
//                      main window: the two validation texts, Fail messages, the
//                      >500 cap, leg exceptions;
//   confirm_big_search caption "Big search", Question icon, Yes/No, owned by the
//                      main window: true only for Yes;
//   choose_place       the modal PlaceChooserDialog: the index of the chosen
//                      candidate, nullopt for Cancel / Esc / close.

#include "wizz/core/domain.hpp"

#include <QString>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

class QWidget;

namespace wizz::app {

struct ControllerDialogs {
    std::function<void(const QString& text)> show_error;
    std::function<bool(const QString& text)> confirm_big_search;
    std::function<std::optional<std::size_t>(std::string_view typed, std::string_view role,
                                             std::span<const Place> candidates, const std::optional<std::string>& hint)>
        choose_place;

    /// The real QMessageBox / PlaceChooserDialog boxes owned by `owner`.
    [[nodiscard]] static ControllerDialogs system(QWidget* owner);
};

} // namespace wizz::app
