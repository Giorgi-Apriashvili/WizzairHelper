#include "settings_dialog.hpp"

#include "modern_style.hpp"
#include "qt_strings.hpp"
#include "section_card.hpp"
#include "ui_style.hpp"

#include "wizz/ui/strings.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMetaObject>
#include <QPushButton>
#include <QRadioButton>
#include <QStyleOptionButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <utility>

namespace wizz::app {

namespace strings = ui::update_strings;
namespace search_strings = ui::strings;

namespace {

constexpr std::array<update::CheckFrequency, 3> kFrequencies{
    update::CheckFrequency::daily, update::CheckFrequency::weekly, update::CheckFrequency::monthly};
constexpr std::array<update::WhenFound, 3> kChoices{
    update::WhenFound::install_on_close, update::WhenFound::download_then_ask, update::WhenFound::notify_only};

/// Between the version line and Check now, and between the check box and the drop-down.
constexpr int kColumnGap = 12;
/// Between the version line and the status line.
constexpr int kStatusGap = 2;
/// Between the caption and the first choice, and between the choices.
constexpr int kChoiceGap = 2;
/// Between the info glyph and the note.
constexpr int kNoteGap = 4;

/// A plain-text label (D-58: rich text would draw its bold unfitted).
[[nodiscard]] QLabel* plain_label(QWidget* parent, bool wrap)
{
    auto* label = new QLabel(parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(wrap);
    label->setFocusPolicy(Qt::NoFocus);
    return label;
}

/// The width `button` asks for with `text` on it (QPushButton::sizeHint for another text).
[[nodiscard]] int button_width(const QPushButton& button, const QString& text)
{
    QStyleOptionButton option;
    option.initFrom(&button);
    option.text = text;
    if (button.isDefault()) {
        option.features |= QStyleOptionButton::DefaultButton;
    }
    if (button.autoDefault()) {
        option.features |= QStyleOptionButton::AutoDefaultButton;
    }
    const QSize contents = button.fontMetrics().size(Qt::TextShowMnemonic, text);
    return button.style()->sizeFromContents(QStyle::CT_PushButton, &option, contents, &button).width();
}

/// "When an update is found" (the caption without its colon), the choices' group name.
[[nodiscard]] QString group_name()
{
    QString caption = to_qstring(strings::kWhenFoundCaption);
    if (caption.endsWith(u':')) {
        caption.chop(1);
    }
    return caption;
}

} // namespace

SettingsDialog::SettingsDialog(const update::UpdateOptions& options, const ui::UpdateStatusView& status,
                               fare_days::ReuseWindow fare_reuse, QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                          Qt::WindowCloseButtonHint)
{
    setWindowTitle(to_qstring(strings::kSettingsTitle));
    setObjectName(QStringLiteral("settingsDialog"));
    setSizeGripEnabled(false);
    setModal(true);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kMargins);
    outer->setSpacing(0);

    card_ = new SectionCard(to_qstring(strings::kUpdatesSection), style::glyph::kSync, false, this);
    card_->setObjectName(QStringLiteral("updatesCard"));
    QVBoxLayout* body = card_->body();

    // The version with the last check, Check now beside it, the status line under both.
    version_ = plain_label(card_, true);
    version_->setObjectName(QStringLiteral("versionLine"));
    status_line_ = plain_label(card_, true);
    status_line_->setObjectName(QStringLiteral("statusLine"));
    status_line_->setAlignment(Qt::AlignLeft | Qt::AlignTop);  // right under the version; the room it keeps is below
    check_now_ = new QPushButton(to_qstring(strings::kCheckNow), card_);
    check_now_->setObjectName(QStringLiteral("checkNow"));
    check_now_->setAutoDefault(false);  // Close stays the primary button; keyPressEvent clicks a focused one
    auto* top = new QGridLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->setHorizontalSpacing(kColumnGap);
    top->setVerticalSpacing(kStatusGap);
    top->addWidget(version_, 0, 0, Qt::AlignVCenter);
    top->addWidget(check_now_, 0, 1, Qt::AlignRight | Qt::AlignVCenter);
    top->addWidget(status_line_, 1, 0, 1, 2);
    top->setColumnStretch(0, 1);
    body->addLayout(top);  // the status line's second line, empty or not, separates the next group

    // Automatic checks and how often.
    automatic_ = new QCheckBox(to_qstring(strings::kCheckAutomatically), card_);
    automatic_->setObjectName(QStringLiteral("checkAutomatically"));
    frequency_ = new QComboBox(card_);
    frequency_->setObjectName(QStringLiteral("checkFrequency"));
    frequency_->setAccessibleName(to_qstring(strings::kCheckFrequencyName));
    ModernStyle::use_styled_popup(*frequency_);
    for (const update::CheckFrequency frequency : kFrequencies) {
        frequency_->addItem(to_qstring(strings::frequency_label(frequency)), static_cast<int>(frequency));
    }
    frequency_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    auto* automatic_row = new QHBoxLayout;
    automatic_row->setContentsMargins(0, 0, 0, 0);
    automatic_row->setSpacing(kColumnGap);
    automatic_row->addWidget(automatic_, 0, Qt::AlignVCenter);
    automatic_row->addWidget(frequency_, 0, Qt::AlignVCenter);
    automatic_row->addStretch(1);
    body->addLayout(automatic_row);
    body->addSpacing(kGroupGap - SectionCard::kRowGap);

    // What happens when an update is found.
    when_found_caption_ = plain_label(card_, true);
    when_found_caption_->setText(to_qstring(strings::kWhenFoundCaption));
    when_found_caption_->setObjectName(QStringLiteral("whenFoundCaption"));
    style::set_text_tone(*when_found_caption_, style::TextTone::secondary);
    body->addWidget(when_found_caption_);
    when_found_group_ = new QWidget(card_);
    when_found_group_->setObjectName(QStringLiteral("whenFoundGroup"));
    when_found_group_->setAccessibleName(group_name());
    auto* choices = new QVBoxLayout(when_found_group_);
    choices->setContentsMargins(0, 0, 0, 0);
    choices->setSpacing(kChoiceGap);
    when_found_ = new QButtonGroup(this);
    when_found_->setExclusive(true);
    for (const update::WhenFound choice : kChoices) {
        auto* button = new QRadioButton(to_qstring(strings::when_found_label(choice)), when_found_group_);
        when_found_->addButton(button, static_cast<int>(choice));
        choices->addWidget(button);
    }
    body->addWidget(when_found_group_);

    // Why the choices are disabled (a portable copy).
    note_row_ = new QWidget(card_);
    note_row_->setObjectName(QStringLiteral("noteRow"));
    auto* note_layout = new QHBoxLayout(note_row_);
    note_layout->setContentsMargins(2, 0, 0, 0);
    note_layout->setSpacing(kNoteGap);
    note_layout->addWidget(new GlyphLabel(style::glyph::kInfo, false, note_row_, GlyphLabel::Tone::muted, 9.0), 0,
                           Qt::AlignTop);
    note_ = plain_label(note_row_, true);
    note_->setObjectName(QStringLiteral("portableNote"));
    note_->setFont(style::hint_font());
    style::set_text_tone(*note_, style::TextTone::muted);
    note_layout->addWidget(note_, 1);
    body->addWidget(note_row_);

    outer->addWidget(card_);
    outer->addSpacing(kGroupGap);

    // Searches (D-68): how long fetched fares are reused.
    searches_card_ = new SectionCard(to_qstring(search_strings::kSearchesSection), style::glyph::kHistory, false, this);
    searches_card_->setObjectName(QStringLiteral("searchesCard"));
    reuse_label_ = plain_label(searches_card_, false);
    reuse_label_->setObjectName(QStringLiteral("reuseLabel"));
    reuse_label_->setText(to_qstring(search_strings::kReuseFaresWithin));
    reuse_ = new QComboBox(searches_card_);
    reuse_->setObjectName(QStringLiteral("reuseFaresWithin"));
    reuse_->setAccessibleName(to_qstring(search_strings::kReuseFaresWithin));
    ModernStyle::use_styled_popup(*reuse_);
    for (const fare_days::ReuseWindow window : search_strings::kReuseWindows) {
        reuse_->addItem(to_qstring(search_strings::reuse_window_label(window)), static_cast<int>(window));
    }
    reuse_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    reuse_label_->setBuddy(reuse_);
    auto* reuse_row = new QHBoxLayout;
    reuse_row->setContentsMargins(0, 0, 0, 0);
    reuse_row->setSpacing(kColumnGap);
    reuse_row->addWidget(reuse_label_, 0, Qt::AlignVCenter);
    reuse_row->addWidget(reuse_, 0, Qt::AlignVCenter);
    reuse_row->addStretch(1);
    searches_card_->body()->addLayout(reuse_row);
    reuse_hint_ = plain_label(searches_card_, true);
    reuse_hint_->setObjectName(QStringLiteral("reuseHint"));
    reuse_hint_->setText(to_qstring(search_strings::kReuseFaresHint));
    reuse_hint_->setFont(style::hint_font());
    style::set_text_tone(*reuse_hint_, style::TextTone::muted);
    searches_card_->body()->addWidget(reuse_hint_);
    outer->addWidget(searches_card_);
    outer->addSpacing(kButtonsGap);
    auto* buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->addStretch(1);
    close_ = new QPushButton(to_qstring(strings::kClose), this);
    close_->setObjectName(QStringLiteral("close"));
    close_->setDefault(true);  // the primary button; Enter closes unless another button has the focus
    buttons->addWidget(close_);
    outer->addLayout(buttons);

    // Reading order (D-63).
    QWidget::setTabOrder(check_now_, automatic_);
    QWidget::setTabOrder(automatic_, frequency_);
    QWidget* previous = frequency_;
    for (QAbstractButton* button : when_found_->buttons()) {
        QWidget::setTabOrder(previous, button);
        previous = button;
    }
    QWidget::setTabOrder(previous, reuse_);
    QWidget::setTabOrder(reuse_, close_);

    connect(check_now_, &QPushButton::clicked, this, &SettingsDialog::check_now_requested);
    connect(close_, &QPushButton::clicked, this, &QDialog::accept);
    connect(automatic_, &QCheckBox::toggled, this, [this] {
        update_enabled_states();
        report_options();
    });
    connect(frequency_, &QComboBox::currentIndexChanged, this, [this] { report_options(); });
    connect(when_found_, &QButtonGroup::idToggled, this, [this](int /*id*/, bool checked) {
        if (checked) {
            report_options();
        }
    });
    connect(reuse_, &QComboBox::currentIndexChanged, this, [this] {
        if (!showing_) {
            Q_EMIT fare_reuse_changed(this->fare_reuse());
        }
    });
    style::on_theme_change(this, [this] {
        apply_status_colour();
        QMetaObject::invokeMethod(this, [this] { fit_size(); }, Qt::QueuedConnection);  // High Contrast's metrics
    });

    show_options(options);
    show_fare_reuse(fare_reuse);
    show_status(status);

    // Focus starts on the first control that can take it.
    for (QWidget* first : std::initializer_list<QWidget*>{check_now_, automatic_, reuse_}) {
        if (first->isEnabled()) {
            first->setFocus(Qt::OtherFocusReason);
            return;
        }
    }
    close_->setFocus(Qt::OtherFocusReason);
}

QRadioButton* SettingsDialog::when_found_button(update::WhenFound when_found) const
{
    return qobject_cast<QRadioButton*>(when_found_->button(static_cast<int>(when_found)));
}

void SettingsDialog::show_options(const update::UpdateOptions& options)
{
    const bool was_showing = std::exchange(showing_, true);  // no options_changed for what is shown
    automatic_->setChecked(options.check_automatically);
    frequency_->setCurrentIndex(std::max(0, frequency_->findData(static_cast<int>(options.check_frequency))));
    if (QRadioButton* button = when_found_button(options.when_found); button != nullptr) {
        button->setChecked(true);
    }
    showing_ = was_showing;
    update_enabled_states();
}

update::UpdateOptions SettingsDialog::options() const
{
    update::UpdateOptions options;
    options.check_automatically = automatic_->isChecked();
    const QVariant frequency = frequency_->currentData();
    if (frequency.isValid()) {
        options.check_frequency = static_cast<update::CheckFrequency>(frequency.toInt());
    }
    if (const int choice = when_found_->checkedId(); choice >= 0) {
        options.when_found = static_cast<update::WhenFound>(choice);
    }
    return options;
}

void SettingsDialog::show_fare_reuse(fare_days::ReuseWindow window)
{
    const bool was_showing = std::exchange(showing_, true);  // no fare_reuse_changed for what is shown
    reuse_->setCurrentIndex(std::max(0, reuse_->findData(static_cast<int>(window))));
    showing_ = was_showing;
}

fare_days::ReuseWindow SettingsDialog::fare_reuse() const
{
    const QVariant window = reuse_->currentData();
    return window.isValid() ? static_cast<fare_days::ReuseWindow>(window.toInt())
                            : fare_days::ReuseWindow::thirty_minutes;
}

void SettingsDialog::report_options()
{
    if (showing_) {
        return;
    }
    Q_EMIT options_changed(options());
}

void SettingsDialog::show_status(const ui::UpdateStatusView& status)
{
    status_ = status;
    const QString version = to_qstring(status.version_line);
    if (version_->text() != version) {
        version_->setText(version);
    }
    const QString line = to_qstring(status.status);
    if (status_line_->text() != line) {
        status_line_->setText(line);
    }
    const QString check_text = to_qstring(status.checking ? strings::kCheckNowBusy : strings::kCheckNow);
    if (check_now_->text() != check_text) {
        check_now_->setText(check_text);
    }
    const QString note = to_qstring(status.when_found_note);
    if (note_->text() != note) {
        note_->setText(note);
    }
    note_row_->setVisible(!note.isEmpty());
    apply_status_colour();
    update_enabled_states();
    fit_size();
}

QColor SettingsDialog::status_colour(ui::UpdateStatusKind kind)
{
    const style::Theme& theme = style::theme();
    switch (kind) {
    case ui::UpdateStatusKind::up_to_date:
        return theme.success;
    case ui::UpdateStatusKind::failed:
        return theme.failure;
    case ui::UpdateStatusKind::checking:
    case ui::UpdateStatusKind::downloading:
        return theme.text_secondary;
    case ui::UpdateStatusKind::off:
        return theme.text_muted;
    case ui::UpdateStatusKind::not_checked:
    case ui::UpdateStatusKind::available:
    case ui::UpdateStatusKind::ready:
        break;
    }
    return theme.text;
}

void SettingsDialog::apply_status_colour()
{
    const QColor colour = status_colour(status_.kind);
    QPalette palette = status_line_->palette();
    if (palette.color(QPalette::WindowText) != colour) {
        palette.setColor(QPalette::WindowText, colour);
        status_line_->setPalette(palette);
    }
}

void SettingsDialog::update_enabled_states()
{
    // Disabling the focused Check now would pass the focus to the check box, where the
    // next Space (to check again, say) would turn automatic checks off and save that. The
    // focus waits on the status line instead, which reports the check (a screen reader
    // reads it), and comes back when Check now does — unless the user moved it meanwhile.
    if (!status_.check_now_enabled && focusWidget() == check_now_) {
        status_line_->setFocus(Qt::OtherFocusReason);
        check_now_focus_parked_ = true;
    }
    check_now_->setEnabled(status_.check_now_enabled);
    if (status_.check_now_enabled && std::exchange(check_now_focus_parked_, false) && focusWidget() == status_line_) {
        check_now_->setFocus(Qt::OtherFocusReason);
    }
    automatic_->setEnabled(status_.options_enabled);
    frequency_->setEnabled(status_.options_enabled && automatic_->isChecked());
    when_found_group_->setEnabled(status_.when_found_enabled);
}

void SettingsDialog::keyPressEvent(QKeyEvent* event)
{
    // Enter clicks the focused push button, as in the main window (01 §6.3); QDialog would
    // click the default button, Close, for a button that is not an auto-default one.
    const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
    if (enter && (event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
        if (auto* button = qobject_cast<QPushButton*>(focusWidget()); button != nullptr && button->isEnabled()) {
            button->click();
            event->accept();
            return;
        }
    }
    QDialog::keyPressEvent(event);
}

void SettingsDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        QMetaObject::invokeMethod(this, [this] { fit_size(); }, Qt::QueuedConnection);
    }
}

void SettingsDialog::fit_size()
{
    if (fitting_) {
        return;
    }
    const bool was_fitting = std::exchange(fitting_, true);
    // Check now keeps one width whether it reads "Check now" or "Checking…".
    check_now_->setMinimumWidth(std::max(button_width(*check_now_, to_qstring(strings::kCheckNow)),
                                         button_width(*check_now_, to_qstring(strings::kCheckNowBusy))));
    // The status line keeps room for two lines, so the usual statuses never resize the window.
    status_line_->setMinimumHeight(2 * status_line_->fontMetrics().lineSpacing());
    QLayout* const top = layout();
    top->invalidate();
    const int height = top->hasHeightForWidth() ? top->totalHeightForWidth(kClientWidth) : top->totalSizeHint().height();
    setFixedSize(kClientWidth, std::max(height, top->totalMinimumSize().height()));
    fitting_ = was_fitting;
}

} // namespace wizz::app
