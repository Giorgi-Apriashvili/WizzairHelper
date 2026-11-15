// TaskbarProgress (01 §10.9, D-04): the ITaskbarList3 wrapper's lifecycle, window
// handle and state/value de-duplication, against a recording COM mock.

#include "mock_taskbar_list.hpp"
#include "taskbar_progress.hpp"

#include <QWidget>

#include <gtest/gtest.h>

#include <memory>

namespace {

using wizz::app::TaskbarListPtr;
using wizz::app::TaskbarProgress;
using wizz::test::is_state_call;
using wizz::test::is_value_call;
using wizz::test::MockTaskbarList;
using wizz::ui::TaskbarCall;
using wizz::ui::TaskbarState;

class TaskbarProgressTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        window_ = std::make_unique<QWidget>();
        child_ = new QWidget(window_.get());
        static_cast<void>(window_->winId());  // the top-level window gets its native handle
    }

    [[nodiscard]] HWND hwnd() const { return reinterpret_cast<HWND>(window_->winId()); }

    MockTaskbarList mock_;
    std::unique_ptr<QWidget> window_;
    QWidget* child_ = nullptr;
};

TEST_F(TaskbarProgressTest, HrInit_is_called_once_and_the_object_is_released)
{
    {
        const TaskbarProgress progress(child_, mock_.factory());
        EXPECT_TRUE(progress.available());
        EXPECT_EQ(mock_.init_calls, 1);
        EXPECT_EQ(mock_.references(), 2U);
    }
    EXPECT_EQ(mock_.references(), 1U);
}

TEST_F(TaskbarProgressTest, Calls_go_to_the_top_level_window_without_making_the_child_native)
{
    TaskbarProgress progress(child_, mock_.factory());
    EXPECT_EQ(progress.window_handle(), window_->winId());

    progress.set_state(TaskbarState::indeterminate);

    ASSERT_EQ(mock_.calls.size(), 1U);
    EXPECT_TRUE(is_state_call(mock_.calls[0], TBPF_INDETERMINATE));
    EXPECT_EQ(mock_.calls[0].window, hwnd());
    EXPECT_EQ(child_->internalWinId(), 0U);
    EXPECT_FALSE(child_->testAttribute(Qt::WA_NativeWindow));
}

TEST_F(TaskbarProgressTest, State_and_value_are_deduplicated_as_01_10_9)
{
    TaskbarProgress progress(child_, mock_.factory());

    progress.set_state(TaskbarState::indeterminate);  // sent
    progress.set_state(TaskbarState::indeterminate);  // repeat: skipped
    progress.set_value(150);                          // sent, implies Normal
    progress.set_value(150);                          // repeat: skipped
    progress.set_state(TaskbarState::normal);         // already Normal: skipped
    progress.set_value(255);                          // sent
    progress.set_state(TaskbarState::error);          // sent, forgets the value
    progress.set_value(255);                          // sent again after a state change
    progress.set_state(TaskbarState::none);           // sent

    ASSERT_EQ(mock_.calls.size(), 6U);
    EXPECT_TRUE(is_state_call(mock_.calls[0], TBPF_INDETERMINATE));
    EXPECT_TRUE(is_value_call(mock_.calls[1], 150));
    EXPECT_TRUE(is_value_call(mock_.calls[2], 255));
    EXPECT_TRUE(is_state_call(mock_.calls[3], TBPF_ERROR));
    EXPECT_TRUE(is_value_call(mock_.calls[4], 255));
    EXPECT_TRUE(is_state_call(mock_.calls[5], TBPF_NOPROGRESS));
    EXPECT_EQ(progress.last_state(), TaskbarState::none);
    EXPECT_EQ(progress.last_value(), wizz::ui::TaskbarProgress::kNoValue);
}

TEST_F(TaskbarProgressTest, The_initial_state_is_none_so_none_is_not_sent_first)
{
    TaskbarProgress progress(child_, mock_.factory());
    progress.set_state(TaskbarState::none);
    EXPECT_TRUE(mock_.calls.empty());
}

TEST_F(TaskbarProgressTest, Values_are_clamped_to_permille)
{
    TaskbarProgress progress(child_, mock_.factory());
    progress.set_value(-5);
    progress.set_value(5000);
    progress.set_value(1000);  // equal to the clamped 5000: skipped

    ASSERT_EQ(mock_.calls.size(), 2U);
    EXPECT_TRUE(is_value_call(mock_.calls[0], 0));
    EXPECT_TRUE(is_value_call(mock_.calls[1], 1000));
    EXPECT_EQ(progress.last_state(), TaskbarState::normal);
}

TEST_F(TaskbarProgressTest, Apply_forwards_model_calls)
{
    TaskbarProgress progress(child_, mock_.factory());
    progress.apply(TaskbarCall{.kind = TaskbarCall::Kind::set_state, .state = TaskbarState::paused, .value = 0});
    progress.apply(TaskbarCall{.kind = TaskbarCall::Kind::set_value, .state = TaskbarState::normal, .value = 42});

    ASSERT_EQ(mock_.calls.size(), 2U);
    EXPECT_TRUE(is_state_call(mock_.calls[0], TBPF_PAUSED));
    EXPECT_TRUE(is_value_call(mock_.calls[1], 42));
}

TEST_F(TaskbarProgressTest, Com_errors_are_ignored_and_the_state_moves_on)
{
    mock_.call_result = E_FAIL;
    TaskbarProgress progress(child_, mock_.factory());
    progress.set_state(TaskbarState::error);
    progress.set_state(TaskbarState::error);

    ASSERT_EQ(mock_.calls.size(), 1U);
    EXPECT_EQ(progress.last_state(), TaskbarState::error);
}

TEST_F(TaskbarProgressTest, No_native_window_means_no_calls_and_no_state_change)
{
    QWidget unshown;
    QWidget* inner = new QWidget(&unshown);
    TaskbarProgress progress(inner, mock_.factory());
    EXPECT_TRUE(progress.available());
    EXPECT_EQ(progress.window_handle(), 0U);

    progress.set_state(TaskbarState::indeterminate);
    progress.set_value(300);
    EXPECT_TRUE(mock_.calls.empty());
    EXPECT_EQ(progress.last_state(), TaskbarState::none);
    EXPECT_EQ(unshown.internalWinId(), 0U);  // looking did not create it

    static_cast<void>(unshown.winId());
    progress.set_state(TaskbarState::indeterminate);
    ASSERT_EQ(mock_.calls.size(), 1U);
    EXPECT_TRUE(is_state_call(mock_.calls[0], TBPF_INDETERMINATE));
}

TEST_F(TaskbarProgressTest, A_destroyed_anchor_disables_the_calls)
{
    auto anchor = std::make_unique<QWidget>();
    static_cast<void>(anchor->winId());
    TaskbarProgress progress(anchor.get(), mock_.factory());
    anchor.reset();

    progress.set_state(TaskbarState::indeterminate);
    EXPECT_TRUE(mock_.calls.empty());
}

TEST_F(TaskbarProgressTest, HrInit_failure_disables_every_call)
{
    mock_.init_result = E_FAIL;
    TaskbarProgress progress(child_, mock_.factory());
    EXPECT_FALSE(progress.available());
    EXPECT_EQ(mock_.references(), 1U);  // released at once

    progress.set_state(TaskbarState::indeterminate);
    progress.set_value(500);
    EXPECT_TRUE(mock_.calls.empty());
}

TEST_F(TaskbarProgressTest, A_failed_creation_disables_every_call)
{
    TaskbarProgress progress(child_, [] { return TaskbarListPtr{}; });
    EXPECT_FALSE(progress.available());
    progress.set_state(TaskbarState::error);
    progress.set_value(10);
    EXPECT_EQ(progress.last_state(), TaskbarState::none);
}

TEST_F(TaskbarProgressTest, The_shell_object_can_be_used_or_is_a_safe_no_op)
{
    // Whatever the machine offers (a real taskbar, or none in a service session), the
    // default wrapper must accept every call without failing.
    TaskbarProgress progress(child_);
    progress.set_state(TaskbarState::indeterminate);
    progress.set_value(500);
    progress.set_state(TaskbarState::none);
    SUCCEED();
}

} // namespace
