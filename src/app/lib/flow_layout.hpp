#pragma once
// A wrapping row layout with the semantics of a WinForms FlowLayoutPanel
// (FlowDirection LeftToRight, WrapContents = true), as the input rows of
// 01 §4.3 use it:
//   * items are placed left to right; an item that does not fit on the current
//     line (its outer width included) starts a new line, unless it is the first
//     item of the line;
//   * every item carries its own outer margins (WinForms Control.Margin; the
//     default is 3 px on every side) and there is no extra spacing;
//   * items are top-aligned in their line at `line top + margin.top`; the line
//     height is the largest outer height of its items;
//   * hidden widgets take no space;
//   * there is no minimum width: an item wider than the row overflows its line.
// The layout reports heightForWidth, so a parent box layout gives the row the
// height its wrapped lines need (narrow windows wrap a row onto more lines).

#include <QLayout>
#include <QMargins>

#include <memory>
#include <vector>

namespace wizz::app {

class FlowLayout final : public QLayout {
public:
    /// WinForms Control.DefaultMargin, used by `addItem` / `addWidget`.
    static constexpr QMargins kDefaultItemMargins{3, 3, 3, 3};

    explicit FlowLayout(QWidget* parent = nullptr);
    ~FlowLayout() override;
    FlowLayout(const FlowLayout&) = delete;
    FlowLayout& operator=(const FlowLayout&) = delete;

    /// Adds `widget` with its own outer margins.
    void add_widget(QWidget* widget, QMargins margins = kDefaultItemMargins);
    /// Adds a horizontal gap of `width` × 1 px without margins (01 §4.3 `Gap(w)`).
    void add_gap(int width);

    /// The outer margins of item `index` (empty margins for an invalid index).
    [[nodiscard]] QMargins item_margins(int index) const;
    /// How many lines the visible items need at `width` (the layout's own width,
    /// contents margins included).
    [[nodiscard]] int line_count(int width) const;

    // QLayout
    void addItem(QLayoutItem* item) override;
    [[nodiscard]] int count() const override;
    [[nodiscard]] QLayoutItem* itemAt(int index) const override;
    [[nodiscard]] QLayoutItem* takeAt(int index) override;
    [[nodiscard]] Qt::Orientations expandingDirections() const override;
    [[nodiscard]] bool hasHeightForWidth() const override;
    [[nodiscard]] int heightForWidth(int width) const override;
    [[nodiscard]] QSize minimumSize() const override;
    [[nodiscard]] QSize sizeHint() const override;
    void setGeometry(const QRect& rect) override;

private:
    struct Entry {
        std::unique_ptr<QLayoutItem> item;
        QMargins margins;
    };
    struct Pass {
        int height = 0;  ///< used height including the contents margins
        int lines = 0;
    };

    /// Places the items into `rect` (or only measures when `apply` is false).
    Pass run(const QRect& rect, bool apply) const;

    std::vector<Entry> entries_;
};

} // namespace wizz::app
