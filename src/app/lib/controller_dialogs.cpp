#include "controller_dialogs.hpp"

#include "place_chooser_dialog.hpp"
#include "qt_strings.hpp"

#include "wizz/ui/strings.hpp"

#include <QMessageBox>
#include <QPointer>
#include <QWidget>

namespace wizz::app {

std::unique_ptr<QMessageBox> make_error_box(const QString& text, QWidget* owner)
{
    auto box = std::make_unique<QMessageBox>(QMessageBox::Information, to_qstring(ui::strings::kAppCaption), text,
                                             QMessageBox::Ok, owner);
    box->setTextFormat(Qt::PlainText);  // typed text and exception messages, never markup
    return box;
}

std::unique_ptr<QMessageBox> make_big_search_box(const QString& text, QWidget* owner)
{
    auto box = std::make_unique<QMessageBox>(QMessageBox::Question, to_qstring(ui::strings::kBigSearchCaption), text,
                                             QMessageBox::Yes | QMessageBox::No, owner);
    box->setTextFormat(Qt::PlainText);
    return box;
}

ControllerDialogs ControllerDialogs::system(QWidget* owner)
{
    const QPointer<QWidget> parent{owner};
    return ControllerDialogs{
        .show_error = [parent](const QString& text) { make_error_box(text, parent.data())->exec(); },
        .confirm_big_search =
            [parent](const QString& text) {
                return make_big_search_box(text, parent.data())->exec() == QMessageBox::Yes;
            },
        .choose_place =
            [parent](std::string_view typed, std::string_view role, std::span<const Place> candidates,
                     const std::optional<std::string>& hint) {
                return PlaceChooserDialog::choose_index(parent.data(), typed, role, candidates, hint);
            },
    };
}

} // namespace wizz::app
