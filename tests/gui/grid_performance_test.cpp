// R-03 (05 §7.3; 01 §8.6 UI-1, WF-2): no UI-thread slice may run 100 ms.
// 6000 fares are streamed in batches of 40 (the 150 ms flush) into a visible
// FlightGrid; each slice is the flush plus the layout and paint it causes
// (processEvents), then the final SetResults rebuild. Three shapes: the default
// price sort, a user text sort (collation on every insertion) and a grouped view
// (a full rebuild on every flush).
//
// What every slice must satisfy, so that the test means the same on an idle
// machine and on a busy CI runner (DECISIONS D-50):
//   * its work, counted: one model notification of the right kind (rows
//     inserted into the empty grid, then a layout change per flush into "All
//     fares"; a reset per flush into a grouped view and for the final result),
//     the rows the model formats (only fares it did not show) and compares (a
//     binary insertion per new row; one sort per rebuild), and the cells the
//     view paints or measures, bounded by repaints of the visible rows — not by
//     the 6000 rows;
//   * the UI thread's own CPU time below the budget: 100 ms with optimisation
//     (NDEBUG), a generous 1 s in Debug (iterator debugging, no inlining, Qt's
//     debug DLLs);
//   * its wall time below the same budget unless other processes held the CPUs
//     during it (CpuMeter::Interval::held_by_others): a busy machine stretches
//     a slice without any more work on the UI thread.
// Timings are printed.

#include "cpu_meter.hpp"
#include "flight_grid_samples.hpp"
#include "grid_test_support.hpp"

#include "wizz/ui/strings.hpp"

#include "wizz/core/result_views.hpp"

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QComboBox>
#include <QStyledItemDelegate>

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace wizz;
using namespace wizz::test::gui;
using app::CpuMeter;
using app::FlightGrid;
using ui::FlightColumn;
using Work = ui::FlightGridModel::Work;

#ifdef NDEBUG
constexpr double kBudgetMs = 100.0;
#else
constexpr double kBudgetMs = 1000.0;  // Debug build; the release gate is 100 ms
#endif
constexpr std::size_t kFares = 6000;
constexpr std::size_t kBatch = 40;
/// A slice repaints the visible cells about once (the layout change, the moved cheapest
/// highlight and the current row coalesce into one paint); twice that leaves room for a
/// second pass after the deferred item layout.
constexpr std::size_t kRepaintsPerSlice = 2;

/// 6000 distinct fares: the CT-FSS-01 fares shifted by day and price.
[[nodiscard]] std::vector<FlightResultPtr> many_fares()
{
    const std::vector<FlightResultPtr> base = fss01();
    std::vector<FlightResultPtr> fares;
    fares.reserve(kFares);
    for (std::size_t i = 0; i < kFares; ++i) {
        FlightResult copy = *base[i % base.size()];
        copy.fare.date = copy.fare.date.add_days(static_cast<std::int64_t>(i / base.size()));
        copy.total_converted = *copy.total_converted + Decimal{static_cast<int>(i % 37)};
        fares.push_back(std::make_shared<const FlightResult>(std::move(copy)));
    }
    return fares;
}

/// The most comparisons a binary search (std::upper_bound) over `rows` rows makes, plus one.
[[nodiscard]] std::size_t binary_insertion_bound(std::size_t rows)
{
    return static_cast<std::size_t>(std::bit_width(rows)) + 1;
}

/// Comparisons an O(n log n) sort of `rows` rows stays below (std::sort: about n log2 n).
[[nodiscard]] std::size_t full_sort_bound(std::size_t rows)
{
    return 3 * rows * static_cast<std::size_t>(std::bit_width(rows));
}

/// Counts the cells the view paints or measures, then lets the grid's own delegate do it.
class CountingDelegate final : public QStyledItemDelegate {
public:
    CountingDelegate(QAbstractItemDelegate* inner, QObject* parent) : QStyledItemDelegate(parent), inner_(inner) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        ++cells;
        inner_->paint(painter, option, index);
    }
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        ++cells;
        return inner_->sizeHint(option, index);
    }

    mutable std::size_t cells = 0;

private:
    QAbstractItemDelegate* inner_;
};

/// The model notifications of a slice.
struct Notifications {
    int resets = 0;
    int layouts = 0;
    int inserts = 0;
};

/// A visible grid of the default window size, after Clear, with its counters attached.
class Rig {
public:
    Rig()
    {
        grid.resize(1320, 700);
        grid.show();
        grid.clear("EUR", false, ui::strings::searching_routes(150, 3));
        settle();
        delegate = new CountingDelegate(grid.table()->itemDelegate(), grid.table());
        grid.table()->setItemDelegate(delegate);
        QAbstractItemModel* model = grid.model();
        QObject::connect(model, &QAbstractItemModel::modelReset, [this] { ++seen.resets; });
        QObject::connect(model, &QAbstractItemModel::layoutChanged, [this] { ++seen.layouts; });
        QObject::connect(model, &QAbstractItemModel::rowsInserted, [this] { ++seen.inserts; });
    }

    /// Cells of one full repaint of the viewport (call with the viewport full of rows).
    [[nodiscard]] std::size_t visible_cells()
    {
        delegate->cells = 0;
        grid.table()->viewport()->repaint();
        return delegate->cells;
    }

    FlightGrid grid{Locale::en_us(), true};
    CountingDelegate* delegate = nullptr;
    Notifications seen;
};

/// What one slice did and took.
struct Slice {
    std::size_t index = 0;  ///< flush number; kFares / kBatch for the final rebuild
    Notifications notified;
    Work work;              ///< the model's work in the slice
    std::size_t rows_after = 0;
    std::size_t cells = 0;  ///< cells painted or measured
    CpuMeter::Interval cpu;
};

/// Runs `change` plus the layout and paint it causes as one measured slice.
[[nodiscard]] Slice measure(Rig& rig, std::size_t index, const std::function<void()>& change)
{
    const Notifications seen_before = rig.seen;
    const Work work_before = rig.grid.model()->state().work();
    rig.delegate->cells = 0;
    const CpuMeter::Sample begin = CpuMeter::sample();
    change();
    QApplication::processEvents();  // the layout and paint this change causes
    const CpuMeter::Sample end = CpuMeter::sample();
    const Work& work_after = rig.grid.model()->state().work();
    return Slice{
        .index = index,
        .notified = Notifications{.resets = rig.seen.resets - seen_before.resets,
                                  .layouts = rig.seen.layouts - seen_before.layouts,
                                  .inserts = rig.seen.inserts - seen_before.inserts},
        .work = Work{.rows_built = work_after.rows_built - work_before.rows_built,
                     .comparisons = work_after.comparisons - work_before.comparisons,
                     .full_sorts = work_after.full_sorts - work_before.full_sorts},
        .rows_after = rig.grid.row_count(),
        .cells = rig.delegate->cells,
        .cpu = CpuMeter::between(begin, end),
    };
}

/// Streams `fares` into the rig like the controller's flush, then delivers the final result.
[[nodiscard]] std::vector<Slice> stream(Rig& rig, const std::vector<FlightResultPtr>& fares)
{
    std::vector<Slice> slices;
    const std::span<const FlightResultPtr> all{fares};
    for (std::size_t start = 0; start < all.size(); start += kBatch) {
        slices.push_back(measure(rig, start / kBatch, [&] {
            rig.grid.append(all.subspan(start, std::min(kBatch, all.size() - start)));
            rig.grid.set_summary(ui::strings::fares_so_far(rig.grid.all_results().size()));
        }));
    }
    const std::vector<FlightResultPtr> ranked = result_views::sort_by_price(fares);
    slices.push_back(
        measure(rig, slices.size(), [&] { rig.grid.set_results(ranked, "EUR", std::to_string(kFares) + " fares"); }));
    return slices;
}

/// Every slice: CPU time within the budget, wall time too unless other processes held the CPUs.
void expect_within_budget(const std::vector<Slice>& slices)
{
    for (const Slice& slice : slices) {
        EXPECT_LT(slice.cpu.thread_ms, kBudgetMs) << "slice " << slice.index << ": the UI thread's own CPU time";
        if (!slice.cpu.held_by_others()) {
            EXPECT_LT(slice.cpu.wall_ms, kBudgetMs)
                << "slice " << slice.index << " took " << slice.cpu.thread_ms << " ms of CPU; idle CPUs "
                << slice.cpu.idle_cpus << ", other processes " << slice.cpu.other_cpus;
        }
    }
}

/// Every slice: cells painted or measured within kRepaintsPerSlice repaints of the visible rows.
void expect_visible_cells_only(const std::vector<Slice>& slices, std::size_t visible_cells)
{
    ASSERT_GT(visible_cells, 0u) << "the view paints (the bound would mean nothing)";
    for (const Slice& slice : slices) {
        EXPECT_LE(slice.cells, kRepaintsPerSlice * visible_cells)
            << "slice " << slice.index << " with " << slice.rows_after << " rows";
    }
}

void report(const char* name, const std::vector<Slice>& slices, std::size_t visible_cells)
{
    const std::span<const Slice> flushes{slices.data(), slices.size() - 1};
    const Slice& final_rebuild = slices.back();
    double slowest_cpu = 0;
    double slowest_wall = 0;
    double all_cpu = 0;
    std::size_t most_cells = 0;
    int held = 0;
    for (const Slice& slice : flushes) {
        slowest_cpu = std::max(slowest_cpu, slice.cpu.thread_ms);
        slowest_wall = std::max(slowest_wall, slice.cpu.wall_ms);
        all_cpu += slice.cpu.thread_ms;
        most_cells = std::max(most_cells, slice.cells);
        held += slice.cpu.held_by_others() ? 1 : 0;
    }
    std::printf("[   R-03   ] %s: %zu flushes of %zu: CPU slowest %.2f ms, mean %.3f ms; wall slowest %.2f ms; "
                "%d held by other processes; cells per flush at most %zu (a repaint: %zu); final rebuild CPU %.2f ms, "
                "wall %.2f ms, %zu rows formatted, %zu comparisons\n",
                name, flushes.size(), kBatch, slowest_cpu, all_cpu / static_cast<double>(flushes.size()), slowest_wall,
                held, most_cells, visible_cells, final_rebuild.cpu.thread_ms, final_rebuild.cpu.wall_ms,
                final_rebuild.work.rows_built, final_rebuild.work.comparisons);
    testing::Test::RecordProperty("slowest_flush_cpu_ms", std::to_string(slowest_cpu));
    testing::Test::RecordProperty("slowest_flush_wall_ms", std::to_string(slowest_wall));
    testing::Test::RecordProperty("final_rebuild_cpu_ms", std::to_string(final_rebuild.cpu.thread_ms));
    testing::Test::RecordProperty("final_rebuild_wall_ms", std::to_string(final_rebuild.cpu.wall_ms));
}

/// "All fares": rows inserted into the empty grid, then one layout change per flush; only
/// the batch is formatted and each row finds its place by binary search; the final result
/// is one reset that formats nothing (the streamed fares) and sorts once.
void expect_all_fares_work(const std::vector<Slice>& slices)
{
    for (const Slice& slice : std::span<const Slice>{slices.data(), slices.size() - 1}) {
        const bool first = slice.index == 0;
        EXPECT_EQ(slice.notified.inserts, first ? 1 : 0) << "flush " << slice.index;
        EXPECT_EQ(slice.notified.layouts, first ? 0 : 1) << "flush " << slice.index;
        EXPECT_EQ(slice.notified.resets, 0) << "flush " << slice.index;
        EXPECT_EQ(slice.work.rows_built, kBatch) << "flush " << slice.index;
        EXPECT_EQ(slice.work.full_sorts, 0u) << "flush " << slice.index;
        EXPECT_LE(slice.work.comparisons, kBatch * binary_insertion_bound(slice.rows_after)) << "flush " << slice.index;
    }
    const Slice& final_rebuild = slices.back();
    EXPECT_EQ(final_rebuild.notified.resets, 1);
    EXPECT_EQ(final_rebuild.notified.layouts, 0);
    EXPECT_EQ(final_rebuild.work.rows_built, 0u) << "the final result holds the streamed fares";
    EXPECT_EQ(final_rebuild.work.full_sorts, 1u);
    EXPECT_LE(final_rebuild.work.comparisons, full_sort_bound(kFares));
}

TEST(FlightGridPerformance, R_03_Streaming_6000_fares_in_batches_of_40_then_the_final_rebuild)
{
    const std::vector<FlightResultPtr> fares = many_fares();
    Rig rig;
    std::vector<Slice> slices = stream(rig, fares);
    // Calibrated now (the viewport is full of rows) and checked for every slice.
    const std::size_t visible_cells = rig.visible_cells();
    report("default sort", slices, visible_cells);
    expect_all_fares_work(slices);
    expect_visible_cells_only(slices, visible_cells);
    expect_within_budget(slices);

    ASSERT_EQ(rig.grid.row_count(), kFares);
    EXPECT_EQ(rig.grid.table()->currentIndex().row(), 0);  // no pick: row 0 (the cheapest) and the top
    for (std::size_t row = 1; row < rig.grid.row_count(); ++row) {
        ASSERT_LE(rig.grid.model()->state().row(row - 1).price_sort, rig.grid.model()->state().row(row).price_sort);
    }
}

TEST(FlightGridPerformance, R_03_Streaming_under_a_user_text_sort)
{
    const std::vector<FlightResultPtr> fares = many_fares();
    Rig rig;
    rig.grid.model()->sort_by_column(FlightColumn::all_departures);  // collation on every insertion
    std::vector<Slice> slices = stream(rig, fares);
    const std::size_t visible_cells = rig.visible_cells();
    report("text sort", slices, visible_cells);
    expect_all_fares_work(slices);
    expect_visible_cells_only(slices, visible_cells);
    expect_within_budget(slices);
    EXPECT_EQ(rig.grid.row_count(), kFares);
}

TEST(FlightGridPerformance, R_03_Streaming_into_a_grouped_view_rebuilds_every_flush)
{
    const std::vector<FlightResultPtr> fares = many_fares();
    Rig rig;
    send_key(rig.grid.view_combo(), QEvent::KeyPress, Qt::Key_Down);  // the user picks "Cheapest per day"
    ASSERT_EQ(rig.grid.mode(), ui::ViewMode::cheapest_per_day);
    std::vector<Slice> slices = stream(rig, fares);
    const std::size_t visible_cells = rig.visible_cells();
    report("cheapest per day", slices, visible_cells);
    // Every flush and the final result: one reset and one sort of the view; rows only for
    // the days whose cheapest fare is new (at most the batch), not for every day.
    for (const Slice& slice : slices) {
        EXPECT_EQ(slice.notified.resets, 1) << "slice " << slice.index;
        EXPECT_EQ(slice.notified.layouts + slice.notified.inserts, 0) << "slice " << slice.index;
        EXPECT_LE(slice.work.rows_built, kBatch) << "slice " << slice.index;
        EXPECT_EQ(slice.work.full_sorts, 1u) << "slice " << slice.index;
        EXPECT_LE(slice.work.comparisons, full_sort_bound(slice.rows_after)) << "slice " << slice.index;
    }
    expect_visible_cells_only(slices, visible_cells);
    expect_within_budget(slices);
    EXPECT_EQ(rig.grid.all_results().size(), kFares);
    EXPECT_EQ(rig.grid.row_count(), ui::view_list(rig.grid.all_results(), ui::ViewMode::cheapest_per_day).size());
}

} // namespace
