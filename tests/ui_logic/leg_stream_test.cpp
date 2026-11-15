// Streaming flush (01 §11.9, §14; 06 §2; 05 §7.2 U-10, UI-2): batches after
// `finished` are ignored, stale job events are dropped, and the final
// SetResults replaces the streamed rows, so a cached re-run never shows a row twice.

#include "wizz/ui/leg_stream.hpp"

#include "ui_logic/ui_samples.hpp"

#include <gtest/gtest.h>

#include <set>
#include <vector>

namespace {

using namespace wizz;
using namespace wizz::ui;

[[nodiscard]] std::vector<FlightResultPtr> fares()
{
    return test::samples::section11_flights();
}

void expect_distinct(const FlightGridModel& grid)
{
    std::set<const FlightResult*> seen;
    for (std::size_t row = 0; row < grid.row_count(); ++row) {
        EXPECT_TRUE(seen.insert(grid.row(row).fare.get()).second) << "row " << row << " appears twice";
    }
}

TEST(LegStream, Flush_appends_pending_rows_with_the_running_count)
{
    const std::vector<FlightResultPtr> all = fares();
    FlightGridModel grid{Locale::en_us()};
    grid.clear("EUR", false, "Searching 3 route(s) from 3 airport(s)\u2026");
    LegStream stream{1};
    EXPECT_EQ(LegStream::kFlushInterval, std::chrono::milliseconds{150});
    EXPECT_FALSE(stream.flush(grid));  // nothing pending
    EXPECT_EQ(grid.summary(), "Searching 3 route(s) from 3 airport(s)\u2026");

    stream.on_partial(std::span<const FlightResultPtr>{all}.first(11));
    EXPECT_EQ(stream.pending_count(), 11u);
    EXPECT_TRUE(stream.flush(grid));
    EXPECT_EQ(stream.pending_count(), 0u);
    EXPECT_EQ(grid.row_count(), 11u);
    EXPECT_EQ(grid.summary(), "11 fares so far\u2026");

    stream.on_partial(std::span<const FlightResultPtr>{all}.subspan(11));
    EXPECT_TRUE(stream.flush(grid));
    EXPECT_EQ(grid.summary(), "12 fares so far\u2026");
    EXPECT_EQ(grid.all_results().size(), 12u);
}

TEST(LegStream, U_10_Late_batches_are_ignored_and_the_final_result_replaces_the_rows)
{
    const std::vector<FlightResultPtr> all = fares();
    FlightGridModel grid{Locale::en_us()};

    // Run 1: streamed, then the final ranked list.
    grid.clear("EUR", false);
    LegStream first{1};
    first.on_partial(all);
    (void)first.flush(grid);
    first.finish();
    grid.set_results(all, "EUR", "12 fares \u00B7 \u2026");
    EXPECT_EQ(grid.row_count(), 12u);

    // Run 2, fully cached: the whole batch arrives, but the result comes before the flush timer.
    grid.clear("EUR", false);
    LegStream second{2};
    second.on_partial(all);
    second.finish();  // flushTimer.stop(); finished = true
    EXPECT_FALSE(second.flush(grid));
    second.on_partial(all);  // a late batch
    EXPECT_EQ(second.pending_count(), 0u);
    EXPECT_FALSE(second.flush(grid));
    EXPECT_EQ(grid.row_count(), 0u);
    grid.set_results(all, "EUR", "12 fares \u00B7 \u2026");
    EXPECT_EQ(grid.row_count(), 12u);
    expect_distinct(grid);
    EXPECT_EQ(grid.summary(), "12 fares \u00B7 \u2026");
}

TEST(LegStream, Streamed_rows_are_replaced_not_added_to)
{
    const std::vector<FlightResultPtr> all = fares();
    FlightGridModel grid{Locale::en_us()};
    grid.clear("EUR", false);
    LegStream stream{7};
    stream.on_partial(all);
    (void)stream.flush(grid);
    stream.finish();
    grid.set_results(all, "EUR", "final");
    EXPECT_EQ(grid.row_count(), all.size());
    EXPECT_EQ(grid.all_results().size(), all.size());
    expect_distinct(grid);
}

TEST(LegStream, Events_of_another_job_are_dropped)
{
    const std::vector<FlightResultPtr> all = fares();
    LegStream stream{2};
    EXPECT_TRUE(stream.accepts(2));
    EXPECT_FALSE(stream.accepts(1));
    stream.on_partial(1, all);  // stale job
    EXPECT_EQ(stream.pending_count(), 0u);
    stream.on_partial(2, all);
    EXPECT_EQ(stream.pending_count(), all.size());
    stream.finish();
    EXPECT_FALSE(stream.accepts(2));
    EXPECT_TRUE(stream.finished());
}

} // namespace
