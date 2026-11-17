// How text is drawn (DECISIONS D-58, ui_style.hpp): by default "natural-heavier", i.e. without
// hinting (DirectWrite natural symmetric) and with every face that has a weight axis 50 units
// heavier than the face Qt picks (regular 450, semibold 650; a bold copy 700, the axis'
// maximum). The tests check that the default reaches every font the app draws text in: the
// fonts ui_style hands out, the application font and Qt's per-class fonts, every visible widget
// of the main window's pages, of every popup and of every dialog, and the fonts the item views'
// models give their cells; that the fonts Qt derives from a widget font (setBold() copies: the
// calendar's month and year buttons, the header's measuring copy, a menu's default action) keep
// the weight they ask for; that ui_style's fonts draw as they were built whatever font they are
// resolved against; and that WIZZ_TEXT_RENDERING=natural|hinted override the default.

#include "app_test_support.hpp"
#include "autotest_diagnostics.hpp"
#include "busy_panel.hpp"
#include "controller_dialogs.hpp"
#include "date_picker.hpp"
#include "elided_label.hpp"
#include "flight_grid.hpp"
#include "grid_table_view.hpp"
#include "input_panel.hpp"
#include "look_test_support.hpp"
#include "modern_style.hpp"
#include "place_chooser_dialog.hpp"
#include "place_combo.hpp"
#include "round_trip_grid.hpp"
#include "ui_style.hpp"

#include "wizz/core/locale.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QCalendarWidget>
#include <QComboBox>
#include <QCompleter>
#include <QFont>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QFontVariableAxis>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QStyleOptionMenuItem>
#include <QTextDocument>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace style = wizz::app::style;
namespace gui = wizz::test::gui;
using style::TextRendering;
using wizz::app::ModernStyle;
using wizz::test::app::WindowRig;
using wizz::test::look::LookGuard;

constexpr std::array kRenderings{TextRendering::natural_heavier, TextRendering::natural, TextRendering::hinted};
const QFont::Tag kWeight("wght");

constexpr wizz::Date d(int year, unsigned month, unsigned day)
{
    return wizz::Date::from_ymd(year, month, day);
}

[[nodiscard]] std::optional<float> axis_of(const QFont& font)
{
    return font.isVariableAxisSet(kWeight) ? std::optional<float>(font.variableAxisValue(kWeight)) : std::nullopt;
}

/// The face Qt picks for `font`'s families and weight (QFontInfo reports an axis as the weight).
[[nodiscard]] QFontInfo face_of(const QFont& font)
{
    QFont plain = font;
    plain.unsetVariableAxis(kWeight);
    return QFontInfo(plain);
}

[[nodiscard]] bool has_weight_axis(const QFontInfo& face)
{
    return std::ranges::any_of(face.variableAxes(), [](const QFontVariableAxis& axis) { return axis.tag() == kWeight; });
}

/// The owner's choice, written out (D-58): what natural-heavier gives the faces the app uses, by
/// the face's family and its own weight (check_autotest.py's CHOSEN_AXES).
[[nodiscard]] std::optional<float> chosen_axis(const QString& family, int weight)
{
    static const std::map<std::pair<QString, int>, float> kChosen{
        {{QStringLiteral("Segoe UI Variable Text"), 400}, 450.0F},
        {{QStringLiteral("Segoe UI Variable Text"), 700}, 700.0F},  // the axis ends at 700
        {{QStringLiteral("Segoe UI Variable Text Semibold"), 600}, 650.0F},
        {{QStringLiteral("Segoe UI Variable Small Semibold"), 600}, 650.0F},
        {{QStringLiteral("Cascadia Mono"), 400}, 450.0F},
        {{QStringLiteral("Cascadia Mono"), 600}, 650.0F},
        {{QStringLiteral("Cascadia Mono"), 700}, 700.0F},
    };
    const auto found = kChosen.find({family, weight});
    return found == kChosen.end() ? std::nullopt : std::optional<float>(found->second);
}

[[nodiscard]] std::optional<float> chosen_axis(const QFontInfo& face)
{
    return chosen_axis(face.family(), face.weight());
}

[[nodiscard]] std::string describe(const QFont& font)
{
    const QFontInfo face = face_of(font);
    return std::format("[{}; face {} {}]", font.toString().toStdString(), face.family().toStdString(), face.weight());
}

/// What keeps `font` from drawing with `rendering`; empty when nothing does.
[[nodiscard]] std::string font_problem(const QFont& font, TextRendering rendering)
{
    std::string problems;
    const auto add = [&problems](const std::string& problem) { problems += (problems.empty() ? "" : "; ") + problem; };
    const QFont::HintingPreference hinting =
        rendering == TextRendering::hinted ? QFont::PreferDefaultHinting : QFont::PreferNoHinting;
    if (font.hintingPreference() != hinting) {
        add(std::format("hinting {} instead of {}", static_cast<int>(font.hintingPreference()), static_cast<int>(hinting)));
    }
    const QStringList families = font.families().isEmpty() ? QStringList{font.family()} : font.families();
    const bool ui_face = std::ranges::any_of(families, [](const QString& f) { return f.startsWith(QStringLiteral("Segoe UI")); });
    const bool variable = std::ranges::any_of(families, [](const QString& f) { return f.startsWith(QStringLiteral("Segoe UI Variable")); });
    if (ui_face && !variable && !style::windows10_fonts()) {
        add("Segoe UI without Segoe UI Variable (a per-class font of Qt's?)");
    }
    const std::optional<float> axis = axis_of(font);
    const QFontInfo face = face_of(font);
    if (rendering != TextRendering::natural_heavier || !has_weight_axis(face)) {
        if (axis) {
            add(std::format("a weight axis {} where none belongs", *axis));
        }
    } else if (!axis) {
        add("no weight axis");
    } else {
        if (const std::optional<float> wanted = style::weight_axis_for(font, rendering); axis != wanted) {
            add(std::format("weight axis {} where the face asks for {}", *axis, wanted.value_or(-1.0F)));
        }
        if (const std::optional<float> chosen = chosen_axis(face); chosen && axis != chosen) {
            add(std::format("weight axis {} where D-58 chose {}", *axis, *chosen));
        }
    }
    if (!problems.empty()) {
        problems += " " + describe(font);
    }
    return problems;
}

class Collector {
public:
    explicit Collector(TextRendering rendering) : rendering_(rendering) {}

    void font(const QFont& font, const std::string& what)
    {
        ++fonts_;
        if (axis_of(font).has_value()) {
            ++with_axis_;
        }
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
            // Rich text takes the weight of <b> and font-weight from the label's font in a text
            // format of its own, which nothing fits (D-58): it would draw "bold" at the regular axis.
            if (const auto* label = qobject_cast<const QLabel*>(widget); label != nullptr) {
                const Qt::TextFormat format = label->textFormat();
                if (format == Qt::RichText || format == Qt::MarkdownText ||
                    (format == Qt::AutoText && Qt::mightBeRichText(label->text()))) {
                    problems_.push_back(name + ": rich text '" + label->text().toStdString() + "'");
                }
            }
            if (const auto* view = qobject_cast<const QAbstractItemView*>(widget); view != nullptr && view->model()) {
                cells(*view, name);
            }
        }
    }

    [[nodiscard]] const std::vector<std::string>& problems() const noexcept { return problems_; }
    [[nodiscard]] int fonts() const noexcept { return fonts_; }
    [[nodiscard]] int fonts_with_axis() const noexcept { return with_axis_; }

private:
    void cells(const QAbstractItemView& view, const std::string& name)
    {
        // As QStyledItemDelegate draws them: the model's font resolved against the view's.
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
    int with_axis_ = 0;
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

/// The fonts ui_style hands out, the application font and Qt's per-class fonts, then every
/// surface of the app with a search's results in its grids.
void check_everything(Collector& check)
{
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
    check.font(QFont{}, "a default-constructed QFont");
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
    // The busy panel's texts, shown or not: the wait note (semibold), the elapsed time (mono
    // semibold) and the status line's compact font (1 pt smaller).
    wizz::app::BusyPanel& busy = rig.busy();
    check.font(busy.wait_label()->font(), "busy panel wait note");
    check.font(busy.elapsed_label()->font(), "busy panel elapsed time");
    check.font(busy.status_label()->font(), "busy panel status");
    if (const std::optional<QFont>& compact = busy.status_label()->compact_font(); compact) {
        check.font(*compact, "busy panel status, compact");
    } else {
        ADD_FAILURE() << "the status line has no compact font";
    }

    wizz::app::InputPanel& input = rig.input();
    // The fare list and the From place list (the names around "Stockholm").
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

    // The calendar drop-down: its month and year buttons are Qt's setBold() copies of the
    // calendar's font, its day names and days the model's fonts.
    wizz::app::DatePicker* picker = input.out_from_picker();
    picker->setFocus(Qt::TabFocusReason);
    const QPoint arrow(picker->width() - 12, picker->height() / 2);
    gui::send_mouse(picker, QEvent::MouseButtonPress, arrow, Qt::LeftButton);
    gui::send_mouse(picker, QEvent::MouseButtonRelease, arrow, Qt::LeftButton);
    pump();
    check.widgets(*picker->calendarWidget()->window(), "calendar");
    picker->calendarWidget()->window()->hide();

    // The grid's context menu, its items as the style draws them.
    rig.window().select_page(wizz::app::MainWindow::Page::outbound);
    pump();
    wizz::app::GridTableView* table = rig.outbound().table();
    gui::context_menu_at(*table, gui::cell_center(*table, 1, 2));
    pump();
    if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()); menu != nullptr) {
        check.widgets(*menu, "context menu");
        for (const QAction* action : menu->actions()) {
            QStyleOptionMenuItem item;
            item.initFrom(menu);
            item.font = action->font().resolve(menu->font());
            item.menuItemType = action == menu->defaultAction() ? QStyleOptionMenuItem::DefaultItem
                                                                : QStyleOptionMenuItem::Normal;
            check.font(ModernStyle::menu_item_font(item), "context menu item " + action->text().toStdString());
        }
        // High Contrast's plain style draws a default action with a setBold() copy of its own,
        // which nothing fits; the app's menus have none (D-58).
        EXPECT_EQ(menu->defaultAction(), nullptr);
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

/// Whether this machine has the variable UI face (Windows 11; the GitHub runner's Windows
/// Server 2025 too). Without it every check of an axis value is moot.
[[nodiscard]] bool variable_ui_face()
{
    return has_weight_axis(face_of(style::form_font()));
}

/// The fonts ui_style hands out, by name: the first five name their weight (the regular ones and the
/// monospace semibold), the semibold families leave it open (they have one face).
[[nodiscard]] std::vector<std::pair<std::string, QFont>> look_fonts()
{
    return {{"form_font", style::form_font()},
            {"data_font", style::data_font()},
            {"hint_font", style::hint_font()},
            {"mono_font", style::mono_font(9.5)},
            {"mono_font semibold", style::mono_font(10.0, true)},
            {"bold_form_font", style::bold_form_font()},
            {"semibold_font", style::semibold_font(9.0)},
            {"semibold_face", style::semibold_face()},
            {"caption_font", style::caption_font()}};
}

TEST(TextRendering, Names_round_trip_and_the_default_is_natural_heavier)
{
    for (const TextRendering rendering : kRenderings) {
        EXPECT_EQ(style::parse_text_rendering(style::text_rendering_name(rendering)), rendering);
    }
    EXPECT_STREQ(style::text_rendering_name(TextRendering::natural_heavier), "natural-heavier");
    EXPECT_STREQ(style::text_rendering_name(TextRendering::natural), "natural");
    EXPECT_STREQ(style::text_rendering_name(TextRendering::hinted), "hinted");
    EXPECT_EQ(style::parse_text_rendering(" Natural-Heavier \n"), TextRendering::natural_heavier);
    EXPECT_EQ(style::parse_text_rendering("HINTED"), TextRendering::hinted);
    // The prototype's candidates are gone; anything unknown is the default.
    for (const char* gone : {"current", "classic", "classic-natural", "grayscale", ""}) {
        EXPECT_EQ(style::parse_text_rendering(gone), std::nullopt) << gone;
    }
    const LookGuard guard;  // clears WIZZ_TEXT_RENDERING for the test
    EXPECT_EQ(style::text_rendering(), TextRendering::natural_heavier) << "unset";
    qputenv("WIZZ_TEXT_RENDERING", "nonsense");
    EXPECT_EQ(style::text_rendering(), TextRendering::natural_heavier) << "unknown";
    EXPECT_EQ(style::kHeavierBy, 50);
}

TEST(TextRendering, The_look_fonts_draw_natural_and_their_variable_faces_50_units_heavier)
{
    const LookGuard guard;
    ASSERT_TRUE(style::apply_application_look());  // the default, whatever the suite runs with
    for (const QFont& font : {style::form_font(), style::data_font(), style::hint_font(), style::mono_font(9.5)}) {
        EXPECT_EQ(font.hintingPreference(), QFont::PreferNoHinting) << describe(font);
        EXPECT_TRUE(font.resolveMask() & QFont::HintingPreferenceResolved) << "set, so it survives resolve()";
        EXPECT_EQ(axis_of(font), has_weight_axis(face_of(font)) ? std::optional(450.0F) : std::nullopt) << describe(font);
    }
    for (const QFont& font : {style::bold_form_font(), style::semibold_font(9.0), style::semibold_face()}) {
        EXPECT_EQ(font.hintingPreference(), QFont::PreferNoHinting) << describe(font);
        EXPECT_EQ(axis_of(font), has_weight_axis(face_of(font)) ? std::optional(650.0F) : std::nullopt)
            << "the semibold family, whatever QFont::weight() says " << describe(font);
    }
    // The caption face (Segoe UI Variable Small Semibold) and Cascadia Mono's semibold are not in
    // the minimal platform's font list as the app asks for them (D-57): the face decides.
    for (const QFont& font : {style::caption_font(), style::mono_font(10.0, true)}) {
        const QFontInfo face = face_of(font);
        const std::optional<float> wanted =
            has_weight_axis(face) ? std::optional(std::min(face.weight() + 50.0F, 700.0F)) : std::nullopt;
        EXPECT_EQ(axis_of(font), wanted) << describe(font);
    }
    if (variable_ui_face()) {
        EXPECT_EQ(QApplication::font(), style::form_font()) << "the application font";
        EXPECT_EQ(axis_of(QApplication::font()), 450.0F);
        EXPECT_EQ(axis_of(QFont{}), 450.0F) << "a default-constructed QFont copies the application font";
    } else {
        GTEST_SKIP() << "no Segoe UI Variable on this machine: plain natural rendering";
    }
}

TEST(TextRendering, A_look_font_draws_as_built_whatever_font_it_is_resolved_against)
{
    // A model's Qt::FontRole, QPainter::setFont and a widget's own font are resolved against another
    // font, and resolve() takes every attribute a font only copied from that font: a font that
    // names its face (its families, and its weight where the family has more than one face) must
    // have its hinting and its weight axis as its own too.
    const LookGuard guard;
    for (const TextRendering rendering : kRenderings) {
        for (const bool windows10 : {false, true}) {
            SCOPED_TRACE(std::format("{}{}", style::text_rendering_name(rendering), windows10 ? ", Windows 10 fonts" : ""));
            if (rendering == TextRendering::natural_heavier) {
                qunsetenv("WIZZ_TEXT_RENDERING");
            } else {
                qputenv("WIZZ_TEXT_RENDERING", style::text_rendering_name(rendering));
            }
            if (windows10) {
                qputenv("WIZZ_FONTS", "windows10");
            } else {
                qunsetenv("WIZZ_FONTS");
            }
            ASSERT_TRUE(style::apply_application_look());
            const std::vector<std::pair<std::string, QFont>> fonts = look_fonts();
            for (std::size_t i = 0; i < fonts.size(); ++i) {
                const auto& [name, font] = fonts[i];
                EXPECT_TRUE(font.resolveMask() & QFont::HintingPreferenceResolved) << name;
                EXPECT_TRUE(font.resolveMask() & QFont::VariableAxesResolved) << name << " " << describe(font);
                EXPECT_EQ(i < 5, (font.resolveMask() & QFont::WeightResolved) != 0) << name << ": a weight of its own";
                EXPECT_EQ(font_problem(font, rendering), "") << name;
            }
            // A font the app builds for painting alone, fitted as GUI_DESIGN asks: one that names
            // its families gets its own axis, one that takes them from elsewhere takes the axis along.
            QFont named;
            named.setFamilies(style::form_font().families());
            named.setWeight(QFont::Normal);
            style::fit_text_rendering(named);
            EXPECT_TRUE(named.resolveMask() & QFont::HintingPreferenceResolved);
            EXPECT_TRUE(named.resolveMask() & QFont::VariableAxesResolved);
            EXPECT_FALSE(style::fit_text_rendering(named)) << "fitted";
            QFont derived;
            derived.setPointSizeF(8.0);
            EXPECT_FALSE(style::fit_text_rendering(derived)) << "the application font's values are right";
            EXPECT_TRUE(derived.resolveMask() & QFont::HintingPreferenceResolved);
            EXPECT_FALSE(derived.resolveMask() & QFont::VariableAxesResolved) << "its families are not its own";
        }
    }
    qunsetenv("WIZZ_TEXT_RENDERING");
    qunsetenv("WIZZ_FONTS");
    ASSERT_TRUE(style::apply_application_look());
    if (!variable_ui_face()) {
        GTEST_SKIP() << "no Segoe UI Variable on this machine";
    }
    QFont bold = style::form_font();
    bold.setBold(true);
    style::fit_text_rendering(bold);
    ASSERT_EQ(axis_of(bold), 700.0F);
    // A regular font resolved against a semibold family (its axis 650) or a bold font (Bold, 700)
    // stays regular: the face it asks for and the axis it draws with are its own.
    const std::vector<std::pair<std::string, QFont>> bases{
        {"bold_form_font", style::bold_form_font()}, {"caption_font", style::caption_font()}, {"a bold copy", bold}};
    for (const auto& [name, regular] : {std::pair<std::string, QFont>{"form_font", style::form_font()},
                                        {"data_font", style::data_font()}, {"hint_font", style::hint_font()}}) {
        for (const auto& [base_name, base] : bases) {
            const QFont resolved = regular.resolve(base);
            EXPECT_EQ(resolved.weight(), QFont::Normal) << name << " against " << base_name;
            EXPECT_EQ(axis_of(resolved), 450.0F) << name << " against " << base_name << " " << describe(resolved);
            EXPECT_EQ(font_problem(resolved, TextRendering::natural_heavier), "") << name << " against " << base_name;
        }
    }
    // The semibold family has one face: against a bold font it stays semibold.
    EXPECT_EQ(axis_of(style::semibold_face().resolve(bold)), 650.0F);
    // A fitted font that takes its families from the font it is resolved against (QPainter::setFont
    // in a semibold widget) takes that font's axis with them.
    QFont derived;
    derived.setPointSizeF(8.0);
    style::fit_text_rendering(derived);
    const QFont in_semibold = derived.resolve(style::bold_form_font());
    EXPECT_EQ(axis_of(in_semibold), 650.0F) << describe(in_semibold);
    EXPECT_EQ(font_problem(in_semibold, TextRendering::natural_heavier), "");
}

TEST(TextRendering, A_bold_copy_draws_bold_and_a_semibold_family_semibold)
{
    const LookGuard guard;
    ASSERT_TRUE(style::apply_application_look());
    if (!variable_ui_face()) {
        GTEST_SKIP() << "no Segoe UI Variable on this machine";
    }
    // A setBold() copy of the UI font asks for the regular family's Bold (700): the copied
    // axis (450) would draw it regular.
    QFont bold = style::form_font();
    bold.setBold(true);
    EXPECT_EQ(axis_of(bold), 450.0F) << "the copy before it is fitted";
    EXPECT_TRUE(style::fit_text_rendering(bold));
    EXPECT_EQ(axis_of(bold), 700.0F) << "Bold, the axis' maximum";
    EXPECT_FALSE(style::fit_text_rendering(bold)) << "fitting twice changes nothing";
    // A weight of 600 on the regular family gives its Bold too (D-53).
    QFont demi = style::form_font();
    demi.setWeight(QFont::DemiBold);
    style::fit_text_rendering(demi);
    EXPECT_EQ(axis_of(demi), 700.0F);
    // The semibold family has one face: a bold copy of it (the header's measuring copy) stays
    // semibold, 650, not the 750 the weight alone would suggest.
    QFont semibold_bold = style::bold_form_font();
    semibold_bold.setBold(true);
    EXPECT_FALSE(style::fit_text_rendering(semibold_bold));
    EXPECT_EQ(axis_of(semibold_bold), 650.0F);
    // A font built without ui_style: a default-constructed QFont copies the application font's
    // axis (450), which the semibold family would draw lighter than semibold.
    QFont built;
    built.setFamilies({QStringLiteral("Segoe UI Variable Text Semibold"), QStringLiteral("Segoe UI Semibold")});
    EXPECT_EQ(axis_of(built), 450.0F);
    EXPECT_TRUE(style::fit_text_rendering(built));
    EXPECT_EQ(axis_of(built), 650.0F);
    QFont light = style::form_font();
    light.setBold(true);
    style::fit_text_rendering(light);
    light.setBold(false);
    EXPECT_TRUE(style::fit_text_rendering(light)) << "back to the regular face";
    EXPECT_EQ(axis_of(light), 450.0F);
    // A face without a weight axis carries none.
    QFont classic(QStringLiteral("Segoe UI"));
    classic.setVariableAxis(kWeight, 450.0F);
    EXPECT_TRUE(style::fit_text_rendering(classic));
    EXPECT_EQ(axis_of(classic), std::nullopt);
    // A menu's default action: bold, with the Bold face's axis.
    QMenu menu;
    QStyleOptionMenuItem item;
    item.initFrom(&menu);
    item.font = menu.font();
    item.menuItemType = QStyleOptionMenuItem::DefaultItem;
    const QFont default_item = ModernStyle::menu_item_font(item);
    EXPECT_EQ(default_item.weight(), QFont::Bold);
    EXPECT_EQ(axis_of(default_item), 700.0F);
    item.menuItemType = QStyleOptionMenuItem::Normal;
    EXPECT_EQ(axis_of(ModernStyle::menu_item_font(item)), 450.0F);
}

/// The calendar's month and year buttons: Qt sets a setBold() copy of the calendar's font on
/// them when it builds the calendar.
void expect_bold_calendar_buttons(const char* look)
{
    SCOPED_TRACE(look);
    QCalendarWidget calendar;
    calendar.show();
    pump();
    for (const char* name : {"qt_calendar_monthbutton", "qt_calendar_yearbutton"}) {
        const auto* button = calendar.findChild<QToolButton*>(QString::fromLatin1(name));
        ASSERT_NE(button, nullptr) << name;
        EXPECT_EQ(button->font().weight(), QFont::Bold) << name;
        EXPECT_EQ(axis_of(button->font()), 700.0F) << name << " " << describe(button->font());
    }
}

TEST(TextRendering, Qt_bold_copies_of_widget_fonts_keep_the_weight_they_ask_for)
{
    const LookGuard guard;
    ASSERT_TRUE(style::apply_application_look());
    if (!variable_ui_face()) {
        GTEST_SKIP() << "no Segoe UI Variable on this machine";
    }
    expect_bold_calendar_buttons("light");

    // Any widget given a bold copy of its font, and its children through it.
    QWidget box;
    auto* layout = new QVBoxLayout(&box);
    auto* label = new QLabel(QStringLiteral("Total"));
    layout->addWidget(label);
    box.show();
    pump();
    EXPECT_EQ(axis_of(label->font()), 450.0F);
    QFont bold = box.font();
    bold.setBold(true);
    box.setFont(bold);
    EXPECT_EQ(axis_of(box.font()), 700.0F);
    EXPECT_EQ(axis_of(label->font()), 700.0F) << "the child inherits the bold font";
    QFont regular = box.font();
    regular.setBold(false);
    box.setFont(regular);
    EXPECT_EQ(axis_of(box.font()), 450.0F);
    EXPECT_EQ(axis_of(label->font()), 450.0F) << "and goes back with it";

    // A font the app might build without ui_style: the semibold family on a default-constructed
    // QFont (which copied the application font's axis, 450).
    QFont built;
    built.setFamilies({QStringLiteral("Segoe UI Variable Text Semibold"), QStringLiteral("Segoe UI Semibold")});
    label->setFont(built);
    EXPECT_EQ(axis_of(label->font()), 650.0F) << describe(label->font());

    // High Contrast's plain style (D-55) keeps the app's fonts and their weights.
    style::set_high_contrast_probe([] { return true; });
    ASSERT_TRUE(style::refresh_appearance());
    ASSERT_FALSE(ModernStyle::is_active());
    expect_bold_calendar_buttons("High Contrast");
}

TEST(TextRendering, The_grid_header_measures_with_the_weight_it_draws)
{
    // QHeaderView sizes a section with a setBold() copy of its font; the semibold header font
    // has no bolder face, so the copy measures and draws exactly as the header does.
    const LookGuard guard;
    ASSERT_TRUE(style::apply_application_look());
    wizz::app::GridTableView view;
    const QFont drawn = view.horizontalHeader()->font();
    QFont measured = drawn;
    measured.setBold(true);
    EXPECT_EQ(axis_of(measured), axis_of(drawn));
    EXPECT_EQ(style::weight_axis_for(measured, TextRendering::natural_heavier),
              style::weight_axis_for(drawn, TextRendering::natural_heavier));
    const QString text = QStringLiteral("All departures from the place you start from");
    EXPECT_EQ(QFontMetricsF(measured).horizontalAdvance(text), QFontMetricsF(drawn).horizontalAdvance(text));
    EXPECT_EQ(QFontMetricsF(measured).height(), QFontMetricsF(drawn).height());
}

TEST(TextRendering, Every_rendering_reaches_every_font_widget_popup_and_dialog)
{
    const NoPopupEffects effects;
    const LookGuard guard;
    struct Case {
        const char* name;
        TextRendering rendering;
        bool high_contrast = false;
        bool windows10 = false;
    };
    for (const Case& c : {Case{"default", TextRendering::natural_heavier}, Case{"natural", TextRendering::natural},
                          Case{"hinted", TextRendering::hinted},
                          Case{"default, High Contrast", TextRendering::natural_heavier, true},
                          Case{"default, Windows 10 fonts", TextRendering::natural_heavier, false, true}}) {
        SCOPED_TRACE(c.name);
        if (c.rendering == TextRendering::natural_heavier) {
            qunsetenv("WIZZ_TEXT_RENDERING");  // the default
        } else {
            qputenv("WIZZ_TEXT_RENDERING", style::text_rendering_name(c.rendering));
        }
        if (c.windows10) {
            qputenv("WIZZ_FONTS", "windows10");
        } else {
            qunsetenv("WIZZ_FONTS");
        }
        style::set_high_contrast_probe([hc = c.high_contrast] { return hc; });
        ASSERT_TRUE(style::apply_application_look());
        ASSERT_EQ(style::text_rendering(), c.rendering);
        ASSERT_EQ(ModernStyle::is_active(), !c.high_contrast);

        // What diagnostics.json reports end to end.
        const wizz::app::EnvironmentDiagnostics environment = wizz::app::describe_environment(*wizz::Locale::en_us());
        EXPECT_EQ(environment.text_rendering, style::text_rendering_name(c.rendering));
        EXPECT_EQ(environment.font_hinting, c.rendering == TextRendering::hinted ? "default" : "none");
        const bool heavier = c.rendering == TextRendering::natural_heavier && !c.windows10 && variable_ui_face();
        EXPECT_EQ(environment.font_weight_axis, heavier ? std::optional(450.0) : std::nullopt);
        const std::vector<std::string> roles{"ui", "semibold", "caption", "mono", "monoSemibold"};
        ASSERT_EQ(environment.fonts.size(), roles.size());
        for (std::size_t i = 0; i < roles.size(); ++i) {
            const wizz::app::FontDiagnostics& font = environment.fonts[i];
            EXPECT_EQ(font.role, roles[i]);
            const std::optional<float> chosen = c.rendering == TextRendering::natural_heavier
                                                    ? chosen_axis(QString::fromStdString(font.face), font.weight)
                                                    : std::nullopt;
            EXPECT_EQ(font.weight_axis, chosen ? std::optional<double>(*chosen) : std::nullopt)
                << font.role << ": " << font.face << " " << font.weight;
        }

        Collector check{c.rendering};
        check_everything(check);
        EXPECT_GT(check.fonts(), 150);
        EXPECT_EQ(check.problems(), std::vector<std::string>{});
        if (c.rendering != TextRendering::natural_heavier || c.windows10) {
            EXPECT_EQ(check.fonts_with_axis(), 0) << "no weight axis anywhere";
        } else if (heavier) {
            EXPECT_GT(check.fonts_with_axis(), check.fonts() * 9 / 10) << "nearly every font is a variable face";
        }  // else no Segoe UI Variable here: Cascadia Mono may still have an axis, which font_problem checks
    }
}

} // namespace
