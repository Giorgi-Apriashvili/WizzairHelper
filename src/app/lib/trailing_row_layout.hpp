#pragma once
// One row of a section card: leading widgets left to right, vertically centred,
// and an optional trailing widget (a search button) aligned to the right edge.
// When the row is too narrow for both, the trailing widget moves to a second
// line, still right-aligned, so nothing is clipped at the minimum window size.
// Reports heightForWidth, so the card grows by that line only when it is needed.

#include <QLayout>

#include <memory>
#include <vector>

namespace wizz::app {

class TrailingRowLayout final : public QLayout {
public:
    explicit TrailingRowLayout(QWidget* parent = nullptr);
    ~TrailingRowLayout() override;
    TrailingRowLayout(const TrailingRowLayout&) = delete;
    TrailingRowLayout& operator=(const TrailingRowLayout&) = delete;

    /// Space between leading items, and between the two lines.
    static constexpr int kSpacing = 8;

    void add_widget(QWidget* widget);
    /// Extra space before the next leading item.
    void add_gap(int width);
    void set_trailing(QWidget* widget);
    /// Whether the trailing widget needs a line of its own at `width`.
    [[nodiscard]] bool wraps_at(int width) const;

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
        int gap_before = 0;
    };
    [[nodiscard]] int leading_width() const;
    [[nodiscard]] int leading_height() const;
    [[nodiscard]] QSize trailing_size() const;

    std::vector<Entry> leading_;
    std::unique_ptr<QLayoutItem> trailing_;
    int pending_gap_ = 0;
};

} // namespace wizz::app
