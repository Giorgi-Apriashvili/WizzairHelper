#include "controller_dialogs.hpp"

#include "place_chooser_dialog.hpp"
#include "qt_strings.hpp"

#include "wizz/ui/strings.hpp"

#include <QMessageBox>
#include <QPointer>
#include <QWidget>

namespace wizz::app {

ControllerDialogs ControllerDialogs::system(QWidget* owner)
{
    const QPointer<QWidget> parent{owner};
    return ControllerDialogs{
        .show_error =
            [parent](const QString& text) {
                QMessageBox box{QMessageBox::Information, to_qstring(ui::strings::kAppCaption), text, QMessageBox::Ok,
                                parent.data()};
                box.exec();
            },
        .confirm_big_search =
            [parent](const QString& text) {
                QMessageBox box{QMessageBox::Question, to_qstring(ui::strings::kBigSearchCaption), text,
                                QMessageBox::Yes | QMessageBox::No, parent.data()};
                return box.exec() == QMessageBox::Yes;
            },
        .choose_place =
            [parent](std::string_view typed, std::string_view role, std::span<const Place> candidates,
                     const std::optional<std::string>& hint) {
                return PlaceChooserDialog::choose_index(parent.data(), typed, role, candidates, hint);
            },
    };
}

} // namespace wizz::app
