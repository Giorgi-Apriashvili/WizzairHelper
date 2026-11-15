// R-03 (05 §7.3; 01 §8.6 UI-1, WF-2): no UI-thread slice may run 100 ms.
// 6000 fares are streamed in batches of 40 (the 150 ms flush) into a visible
// FlightGrid; each slice is the flush plus the layout and paint it causes
// (processEvents), then the final SetResults rebuild. Timings are printed; the
// bound is 100 ms with optimisation (NDEBUG) and a generous 1 s in Debug
// (iterator debugging, no inlining). Three shapes: the default price sort,
// a user text sort (collation on every insertion) and a grouped view (a full
// rebuild on every flush).

#include "flight_grid_samples.hpp"
#include "grid_test_support.hpp"

#include "wizz/ui/strings.hpp"

#include "wizz/core/result_views.hpp"

#include <QApplication>
#include <QComboBox>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace wizz;
using namespace wizz::test::gui;
using app::FlightGrid;
using ui::FlightColumn;
using Milliseconds = std::chrono::duration<double, std::milli>;

#ifdef NDEBUG
constexpr Milliseconds kBudget{100};
#else
constexpr Milliseconds kBudget{1000};  // Debug build; the release gate is 100 ms
#endif
constexpr std::size_t kFares = 6000;
constexpr std::size_t kBatch = 40;

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

struct Timings {
    Milliseconds slowest_flush{};
    Milliseconds all_flushes{};
    Milliseconds all_grid_calls{};  ///< append + set_summary, without the layout and paint
    std::size_t flushes = 0;
    Milliseconds final_rebuild{};
};

/// Streams `fares` into `grid` like the controller's flush, then delivers the final result.
[[nodiscard]] Timings stream(FlightGrid& grid, const std::vector<FlightResultPtr>& fares)
{
    using Clock = std::chrono::steady_clock;
    Timings timings;
    const std::span<const FlightResultPtr> all{fares};
    for (std::size_t start = 0; start < all.size(); start += kBatch) {
        const auto begin = Clock::now();
        grid.append(all.subspan(start, std::min(kBatch, all.size() - start)));
        grid.set_summary(ui::strings::fares_so_far(grid.all_results().size()));
        timings.all_grid_calls += Clock::now() - begin;
        QApplication::processEvents();  // the layout and paint this flush causes
        const Milliseconds spent = Clock::now() - begin;
        timings.slowest_flush = std::max(timings.slowest_flush, spent);
        timings.all_flushes += spent;
        ++timings.flushes;
    }
    const std::vector<FlightResultPtr> ranked = result_views::sort_by_price(fares);
    const auto begin = Clock::now();
    grid.set_results(ranked, "EUR", "6000 fares");
    QApplication::processEvents();
    timings.final_rebuild = Clock::now() - begin;
    return timings;
}

void report(const char* name, const Timings& timings)
{
    const auto flushes = static_cast<double>(timings.flushes);
    std::printf("[   R-03   ] %s: %zu flushes of %zu, slowest %.2f ms, mean %.3f ms (grid calls %.3f ms, layout and "
                "paint %.3f ms); final rebuild %.2f ms\n",
                name, timings.flushes, kBatch, timings.slowest_flush.count(), timings.all_flushes.count() / flushes,
                timings.all_grid_calls.count() / flushes,
                (timings.all_flushes - timings.all_grid_calls).count() / flushes, timings.final_rebuild.count());
    testing::Test::RecordProperty("slowest_flush_ms", std::to_string(timings.slowest_flush.count()));
    testing::Test::RecordProperty("final_rebuild_ms", std::to_string(timings.final_rebuild.count()));
}

/// A visible grid of the default window size, after Clear.
void prepare(FlightGrid& grid)
{
    grid.resize(1320, 700);
    grid.show();
    grid.clear("EUR", false, ui::strings::searching_routes(150, 3));
    settle();
}

TEST(FlightGridPerformance, R_03_Streaming_6000_fares_in_batches_of_40_then_the_final_rebuild)
{
    const std::vector<FlightResultPtr> fares = many_fares();
    FlightGrid grid{Locale::en_us(), true};
    prepare(grid);
    const Timings timings = stream(grid, fares);
    report("default sort", timings);
    EXPECT_LT(timings.slowest_flush, kBudget);
    EXPECT_LT(timings.final_rebuild, kBudget);

    ASSERT_EQ(grid.row_count(), kFares);
    EXPECT_EQ(grid.table()->currentIndex().row(), 0);  // no pick: row 0 (the cheapest) and the top
    for (std::size_t row = 1; row < grid.row_count(); ++row) {
        ASSERT_LE(grid.model()->state().row(row - 1).price_sort, grid.model()->state().row(row).price_sort);
    }
}

TEST(FlightGridPerformance, R_03_Streaming_under_a_user_text_sort)
{
    const std::vector<FlightResultPtr> fares = many_fares();
    FlightGrid grid{Locale::en_us(), true};
    prepare(grid);
    grid.model()->sort_by_column(FlightColumn::all_departures);  // collation on every insertion
    const Timings timings = stream(grid, fares);
    report("text sort", timings);
    EXPECT_LT(timings.slowest_flush, kBudget);
    EXPECT_LT(timings.final_rebuild, kBudget);
    EXPECT_EQ(grid.row_count(), kFares);
}

TEST(FlightGridPerformance, R_03_Streaming_into_a_grouped_view_rebuilds_every_flush)
{
    const std::vector<FlightResultPtr> fares = many_fares();
    FlightGrid grid{Locale::en_us(), true};
    prepare(grid);
    send_key(grid.view_combo(), QEvent::KeyPress, Qt::Key_Down);  // the user picks "Cheapest per day"
    ASSERT_EQ(grid.mode(), ui::ViewMode::cheapest_per_day);
    const Timings timings = stream(grid, fares);
    report("cheapest per day", timings);
    EXPECT_LT(timings.slowest_flush, kBudget);
    EXPECT_LT(timings.final_rebuild, kBudget);
    EXPECT_EQ(grid.all_results().size(), kFares);
    EXPECT_EQ(grid.row_count(), ui::view_list(grid.all_results(), ui::ViewMode::cheapest_per_day).size());
}

} // namespace
