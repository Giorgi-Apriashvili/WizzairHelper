#include "place_chooser_dialog.hpp"

#include "qt_strings.hpp"

#include "wizz/ui/strings.hpp"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace wizz::app {

namespace {

constexpr int kIntroBottomPadding = 6;
constexpr int kButtonsTopPadding = 8;
/// Two WinForms buttons side by side: 3 px margin each.
constexpr int kButtonSpacing = 6;

} // namespace

PlaceChooserDialog::PlaceChooserDialog(std::string_view typed, std::string_view role,
                                       std::span<const Place> candidates, const std::optional<std::string>& hint,
                                       QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                          Qt::WindowCloseButtonHint),
      candidates_(candidates.begin(), candidates.end())
{
    setWindowTitle(to_qstring(ui::strings::chooser_title(role)));
    setFixedSize(kClientWidth, kClientHeight);
    setSizeGripEnabled(false);
    setModal(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(kPadding, kPadding, kPadding, kPadding);
    layout->setSpacing(0);

    // The catalogue text uses Environment.NewLine between hint and intro.
    QString intro = to_qstring(ui::strings::chooser_intro(typed, role, candidates_.size(), hint));
    intro.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    intro_ = new QLabel(intro, this);
    intro_->setTextFormat(Qt::PlainText);
    intro_->setWordWrap(true);
    intro_->setContentsMargins(0, 0, 0, kIntroBottomPadding);
    layout->addWidget(intro_);

    list_ = new QListWidget(this);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    for (const Place& place : candidates_) {
        list_->addItem(to_qstring(place.description));
    }
    if (list_->count() > 0) {
        list_->setCurrentRow(0);
    }
    layout->addWidget(list_, 1);

    auto* buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, kButtonsTopPadding, 0, 0);
    buttons->setSpacing(kButtonSpacing);
    buttons->addStretch(1);
    ok_ = new QPushButton(to_qstring(ui::strings::kUseThisPlace), this);
    cancel_ = new QPushButton(to_qstring(ui::strings::kCancel), this);
    ok_->setDefault(true);
    buttons->addWidget(ok_);
    buttons->addWidget(cancel_);  // Cancel is rightmost
    layout->addLayout(buttons);

    connect(ok_, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancel_, &QPushButton::clicked, this, &QDialog::reject);
    connect(list_, &QListWidget::itemDoubleClicked, this, &QDialog::accept);

    list_->setFocus(Qt::OtherFocusReason);  // the chooser owns keyboard focus (06 §2)
}

std::optional<Place> PlaceChooserDialog::choose(QWidget* parent, std::string_view typed, std::string_view role,
                                                std::span<const Place> candidates,
                                                const std::optional<std::string>& hint)
{
    const std::optional<std::size_t> index = choose_index(parent, typed, role, candidates, hint);
    if (!index) {
        return std::nullopt;
    }
    return candidates[*index];
}

std::optional<std::size_t> PlaceChooserDialog::choose_index(QWidget* parent, std::string_view typed,
                                                            std::string_view role, std::span<const Place> candidates,
                                                            const std::optional<std::string>& hint)
{
    // Always owned (by the active window when no parent is given), so the chooser
    // never gets its own taskbar button.
    PlaceChooserDialog dialog(typed, role, candidates, hint, parent != nullptr ? parent : QApplication::activeWindow());
    dialog.exec();
    return dialog.chosen_index();
}

std::optional<std::size_t> PlaceChooserDialog::selected_index() const
{
    const QList<QListWidgetItem*> selected = list_->selectedItems();
    if (selected.isEmpty()) {
        return std::nullopt;
    }
    const int row = list_->row(selected.front());
    if (row < 0 || static_cast<std::size_t>(row) >= candidates_.size()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(row);
}

std::optional<Place> PlaceChooserDialog::selected_place() const
{
    const std::optional<std::size_t> index = selected_index();
    if (!index) {
        return std::nullopt;
    }
    return candidates_[*index];
}

std::optional<std::size_t> PlaceChooserDialog::chosen_index() const
{
    if (result() != QDialog::Accepted) {
        return std::nullopt;
    }
    return selected_index();
}

} // namespace wizz::app
