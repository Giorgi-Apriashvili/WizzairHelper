// WIZZ_TEXT_RENDERING (ui_style.hpp): each candidate reaches every font the app draws text
// with: the fonts ui_style hands out, the application font and Qt's per-class fonts, every
// visible widget of the main window's pages, of every popup and of every dialog (the combo
// and place lists, the place completer, the calendar, the grid menu, the tooltip, the message
// boxes, the place chooser), and the fonts the item views' models give their cells (the
// grids' semibold cells, resolved against the view's font as QStyledItemDelegate does).

#include "app_test_support.hpp"
#include "controller_dialogs.hpp"
#include "date_picker.hpp"
#include "flight_grid.hpp"
#include "grid_table_view.hpp"
#include "input_panel.hpp"
#include "place_chooser_dialog.hpp"
#include "place_combo.hpp"
#include "round_trip_grid.hpp"
#include "ui_style.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QCalendarWidget>
#include <QComboBox>
#include <QCompleter>
#include <QFont>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QToolTip>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace style = wizz::app::style;
namespace gui = wizz::test::gui;
using style::TextRendering;
using wizz::test::app::WindowRig;

constexpr std::array kCandidates{TextRendering::current, TextRendering::natural, TextRendering::classic,
                                 TextRendering::classic_natural, TextRendering::natural_heavier};

constexpr wizz::Date d(int year, unsigned month, unsigned day)
{
    return wizz::Date::from_ymd(year, month, day);
}

/// Sets WIZZ_TEXT_RENDERING for the test; puts the run's value and its look back afterwards.
/// The app reads the variable once, before its first font: a default-constructed QFont copies
/// the application font, so each candidate starts from Qt's default hinting and no weight axis,
/// as a new process would, not from the previous candidate's.
class TextRenderingGuard {
public:
    TextRenderingGuard()
    {
        if (qEnvironmentVariableIsSet(kVariable)) {
            saved_ = qgetenv(kVariable);
        }
    }
    ~TextRenderingGuard()
    {
        if (saved_) {
            qputenv(kVariable, *saved_);
        } else {
            qunsetenv(kVariable);
        }
        reset_application_font();
        static_cast<void>(style::apply_application_look());
    }
    TextRenderingGuard(const TextRenderingGuard&) = delete;
    TextRenderingGuard& operator=(const TextRenderingGuard&) = delete;

    static void use(TextRendering rendering)
    {
        qputenv(kVariable, style::text_rendering_name(rendering));
        reset_application_font();
    }

    static void reset_application_font()
    {
        QFont plain = QApplication::font();
        plain.setHintingPreference(QFont::PreferDefaultHinting);
        plain.unsetVariableAxis(QFont::Tag("wght"));
        QApplication::setFont(plain);
    }

private:
    static constexpr const char* kVariable = "WIZZ_TEXT_RENDERING";
    std::optional<QByteArray> saved_;
};

[[nodiscard]] bool unhinted(TextRendering rendering)
{
    return rendering == TextRendering::natural || rendering == TextRendering::classic_natural ||
           rendering == TextRendering::natural_heavier;
}

[[nodiscard]] bool classic(TextRendering rendering)
{
    return rendering == TextRendering::classic || rendering == TextRendering::classic_natural;
}

/// What keeps `font` from drawing with `rendering`; empty when nothing does.
[[nodiscard]] std::string font_problem(const QFont& font, TextRendering rendering)
{
    std::string problems;
    const auto add = [&problems](const std::string& problem) {
        problems += (problems.empty() ? "" : "; ") + problem;
    };
    const QFont::HintingPreference hinting = unhinted(rendering) ? QFont::PreferNoHinting : QFont::PreferDefaultHinting;
    if (font.hintingPreference() != hinting) {
        add(std::format("hinting {} instead of {}", static_cast<int>(font.hintingPreference()), static_cast<int>(hinting)));
    }
    const QStringList families = font.families().isEmpty() ? QStringList{font.family()} : font.families();
    const auto any = [&families](const QString& prefix) {
        return std::ranges::any_of(families, [&prefix](const QString& family) { return family.startsWith(prefix); });
    };
    const bool ui_face = any(QStringLiteral("Segoe UI"));
    const bool variable = any(QStringLiteral("Segoe UI Variable"));
    if (ui_face && classic(rendering) && variable) {
        add("Segoe UI Variable in a classic candidate");
    }
    if (ui_face && !classic(rendering) && !variable && !style::windows10_fonts()) {
        add("Segoe UI without Segoe UI Variable");
    }
    const QFont::Tag axis("wght");
    if (rendering != TextRendering::natural_heavier) {
        if (font.isVariableAxisSet(axis)) {
            add("a weight axis");
        }
    } else if (variable || families.contains(QStringLiteral("Cascadia Mono"))) {
        if (!font.isVariableAxisSet(axis)) {
            add("no weight axis");
        } else if (font.variableAxisValue(axis) < style::heavier_weight_axis(font.weight())) {
            add(std::format("weight axis {} for weight {}", font.variableAxisValue(axis), static_cast<int>(font.weight())));
        }
    }
    if (!problems.empty()) {
        problems += std::format(" [{}]", font.toString().toStdString());
    }
    return problems;
}

class Collector {
public:
    explicit Collector(TextRendering rendering) : rendering_(rendering) {}

    void font(const QFont& font, const std::string& what)
    {
        ++fonts_;
        if (std::string problem = font_problem(font, rendering_); !problem.empty()) {
            problems_.push_back(what + ": " + problem);
        }
    }

    /// `root` and its visible children, and the cell fonts of the item views among them.
    void widgets(const QWidget& root, const std::string& where)
    {
        if (!root.isVisible()) {
            problems_.push_back(where + ": not shown");
            return;
        }
        std::vector<const QWidget*> all{&root};
        for (const QWidget* child : root.findChildren<QWidget*>()) {
            if (child->isVisible()) {
                all.push_back(child);
            }
        }
        for (const QWidget* widget : all) {
            const std::string name = std::format("{}: {} '{}'", where, widget->metaObject()->className(),
                                                 widget->objectName().toStdString());
            font(widget->font(), name);
            if (const auto* view = qobject_cast<const QAbstractItemView*>(widget); view != nullptr && view->model()) {
                cells(*view, name);
            }
        }
    }

    [[nodiscard]] const std::vector<std::string>& problems() const noexcept { return problems_; }
    [[nodiscard]] int fonts() const noexcept { return fonts_; }

private:
    void cells(const QAbstractItemView& view, const std::string& name)
    {
        const QAbstractItemModel& model = *view.model();
        const int rows = std::min(model.rowCount(view.rootIndex()), 200);
        for (int row = 0; row < rows; ++row) {
            for (int column = 0; column < model.columnCount(view.rootIndex()); ++column) {
                const QVariant value = model.index(row, column, view.rootIndex()).data(Qt::FontRole);
                if (value.isValid() && !value.isNull()) {
                    font(qvariant_cast<QFont>(value).resolve(view.font()), std::format("{} cell {},{}", name, row, column));
                }
            }
        }
    }

    TextRendering rendering_;
    std::vector<std::string> problems_;
    int fonts_ = 0;
};

void pump()
{
    for (int i = 0; i < 4; ++i) {
        QApplication::processEvents();
    }
}

[[nodiscard]] QWidget* visible_tooltip()
{
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->inherits("QTipLabel") && widget->isVisible()) {
            return widget;
        }
    }
    return nullptr;
}

void check_candidate(Collector& check)
{
    // The fonts ui_style hands out, the application font and the per-class fonts Qt uses for
    // its own widgets (an empty hash after QApplication::setFont: all are the application font).
    check.font(style::form_font(), "form_font");
    check.font(style::data_font(), "data_font");
    check.font(style::bold_form_font(), "bold_form_font");
    check.font(style::semibold_font(9.0), "semibold_font");
    check.font(style::semibold_face().resolve(style::data_font()), "semibold_face");
    check.font(style::caption_font(), "caption_font");
    check.font(style::hint_font(), "hint_font");
    check.font(style::mono_font(9.5), "mono_font");
    check.font(style::mono_font(10.0, true), "mono_font semibold");
    check.font(QApplication::font(), "application font");
    check.font(QToolTip::font(), "tooltip font");
    for (const char* name : {"QMenu", "QMenuBar", "QMessageBoxLabel", "QTipLabel", "QAbstractItemView", "QListView",
                             "QHeaderView", "QComboBox", "QComboMenuItem", "QLineEdit", "QLabel", "QPushButton",
                             "QCalendarWidget", "QToolButton", "QTabBar", "QCheckBox", "QDialog"}) {
        check.font(QApplication::font(name), std::string{"QApplication::font(\""} + name + "\")");
    }

    WindowRig rig;
    rig.start();
    rig.input().set_from_radius_km(200);
    rig.input().set_to_radius_km(30);
    rig.input().apply_autotest_dates(wizz::ui::DateWindow{.start = d(2026, 11, 10), .end = d(2026, 11, 20)},
                                     wizz::ui::DateWindow{.start = d(2026, 11, 12), .end = d(2026, 11, 20)});
    rig.run(rig.controller().search_outbound());
    rig.run(rig.controller().search_return());
    QWidget& window = rig.window();
    for (const auto page : {wizz::app::MainWindow::Page::outbound, wizz::app::MainWindow::Page::inbound,
                            wizz::app::MainWindow::Page::round_trips, wizz::app::MainWindow::Page::log}) {
        rig.window().select_page(page);
        pump();
        check.widgets(window, std::format("main window page {}", static_cast<int>(page)));
    }

    wizz::app::InputPanel& input = rig.input();
    // The fare list and the From place list (the names around "Stockholm", as in the report).
    QComboBox* fare = input.fare_box();
    fare->showPopup();
    pump();
    check.widgets(*fare->view()->window(), "fare list");
    fare->hidePopup();
    wizz::app::PlaceCombo* from = input.from_box();
    const QString typed = from->currentText();
    from->setCurrentIndex(std::max(0, from->findText(QStringLiteral("Stockholm"))));
    from->showPopup();
    pump();
    check.widgets(*from->view()->window(), "place list");
    from->hidePopup();
    input.set_from_text(typed.toStdString());

    // The place completer.
    QLineEdit* edit = from->lineEdit();
    edit->setFocus(Qt::OtherFocusReason);
    edit->clear();
    gui::type_text(edit, QStringLiteral("Bu"));
    pump();
    check.widgets(*from->completer()->popup(), "completer");
    from->completer()->popup()->hide();
    input.set_from_text(typed.toStdString());

    // The calendar drop-down (its month and year buttons are Qt's setBold() copies).
    wizz::app::DatePicker* picker = input.out_from_picker();
    picker->setFocus(Qt::TabFocusReason);
    const QPoint arrow(picker->width() - 12, picker->height() / 2);
    gui::send_mouse(picker, QEvent::MouseButtonPress, arrow, Qt::LeftButton);
    gui::send_mouse(picker, QEvent::MouseButtonRelease, arrow, Qt::LeftButton);
    pump();
    check.widgets(*picker->calendarWidget()->window(), "calendar");
    picker->calendarWidget()->window()->hide();

    // The grid's context menu.
    rig.window().select_page(wizz::app::MainWindow::Page::outbound);
    pump();
    wizz::app::GridTableView* table = rig.outbound().table();
    gui::context_menu_at(*table, gui::cell_center(*table, 1, 2));
    pump();
    if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()); menu != nullptr) {
        check.widgets(*menu, "context menu");
        menu->hide();
    } else {
        ADD_FAILURE() << "the grid's context menu did not open";
    }

    // The From box's tooltip.
    QToolTip::showText(from->mapToGlobal(QPoint(40, from->height())), QStringLiteral("City, airport or IATA code"), from);
    pump();
    if (QWidget* tip = visible_tooltip(); tip != nullptr) {
        check.widgets(*tip, "tooltip");
    } else if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        ADD_FAILURE() << "the tooltip did not show";
    }  // the minimal platform shows none: QToolTip::font() is checked above
    QToolTip::hideText();

    // The message boxes and the place chooser.
    {
        const auto box = wizz::app::make_big_search_box(QStringLiteral("This search sends 612 requests."), &window);
        box->open();
        pump();
        check.widgets(*box, "question box");
        box->done(QMessageBox::No);
    }
    {
        const auto box = wizz::app::make_error_box(QStringLiteral("Could not find “Atlantis”."), &window);
        box->open();
        pump();
        check.widgets(*box, "error box");
        box->done(QMessageBox::Ok);
    }
    {
        std::vector<wizz::Place> candidates;
        for (const char* place : {"London, United Kingdom", "London, Ontario, Canada"}) {
            candidates.push_back(wizz::Place{.name = "London", .description = place});
        }
        wizz::app::PlaceChooserDialog chooser("London", "destination", candidates, std::nullopt, &window);
        chooser.show();
        pump();
        check.widgets(chooser, "place chooser");
        chooser.reject();
    }
}

TEST(TextRendering, Names_round_trip_and_unknown_values_are_the_released_rendering)
{
    for (const TextRendering rendering : kCandidates) {
        EXPECT_EQ(style::parse_text_rendering(style::text_rendering_name(rendering)), rendering);
    }
    EXPECT_EQ(style::parse_text_rendering(" Natural-Heavier \n"), TextRendering::natural_heavier);
    EXPECT_EQ(style::parse_text_rendering("grayscale"), std::nullopt);
    EXPECT_EQ(style::parse_text_rendering(""), std::nullopt);
    const TextRenderingGuard guard;
    qputenv("WIZZ_TEXT_RENDERING", "nonsense");
    EXPECT_EQ(style::text_rendering(), TextRendering::current);
    qunsetenv("WIZZ_TEXT_RENDERING");
    EXPECT_EQ(style::text_rendering(), TextRendering::current);
}

TEST(TextRendering, The_released_rendering_leaves_every_font_as_it_was)
{
    const TextRenderingGuard guard;
    qunsetenv("WIZZ_TEXT_RENDERING");
    TextRenderingGuard::reset_application_font();  // a run under another candidate set its axis there
    for (const QFont& font : {style::form_font(), style::data_font(), style::bold_form_font(), style::caption_font(),
                              style::hint_font(), style::mono_font(9.5), style::semibold_face()}) {
        EXPECT_FALSE(font.resolveMask() & QFont::HintingPreferenceResolved) << font.toString().toStdString();
        EXPECT_FALSE(font.isVariableAxisSet(QFont::Tag("wght"))) << font.toString().toStdString();
    }
}

/// Popups appear at once (no roll-out or fade, which the windows platform runs), so they
/// are visible when checked; the effects are put back afterwards.
class NoPopupEffects {
public:
    NoPopupEffects()
    {
        for (const auto& [effect, enabled] : saved_) {
            QApplication::setEffectEnabled(effect, false);
        }
    }
    ~NoPopupEffects()
    {
        for (const auto& [effect, enabled] : saved_) {
            QApplication::setEffectEnabled(effect, enabled);
        }
    }
    NoPopupEffects(const NoPopupEffects&) = delete;
    NoPopupEffects& operator=(const NoPopupEffects&) = delete;

private:
    static std::pair<Qt::UIEffect, bool> state(Qt::UIEffect effect) { return {effect, QApplication::isEffectEnabled(effect)}; }
    std::array<std::pair<Qt::UIEffect, bool>, 5> saved_{state(Qt::UI_AnimateCombo), state(Qt::UI_AnimateMenu),
                                                        state(Qt::UI_FadeMenu), state(Qt::UI_AnimateTooltip),
                                                        state(Qt::UI_FadeTooltip)};
};

TEST(TextRendering, Every_candidate_reaches_every_font_widget_popup_and_dialog)
{
    const NoPopupEffects effects;
    const TextRenderingGuard guard;
    for (const TextRendering rendering : kCandidates) {
        SCOPED_TRACE(style::text_rendering_name(rendering));
        TextRenderingGuard::use(rendering);
        ASSERT_EQ(style::text_rendering(), rendering);
        ASSERT_TRUE(style::apply_application_look());
        Collector check{rendering};
        check_candidate(check);
        EXPECT_GT(check.fonts(), 100);
        EXPECT_EQ(check.problems(), std::vector<std::string>{});
    }
}

} // namespace
