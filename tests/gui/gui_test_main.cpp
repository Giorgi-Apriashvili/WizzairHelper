// GoogleTest entry point that owns the QApplication for widget tests. It is the
// app's own Application, so the crash guard in notify() is under test too
// (inactive unless a test installs a CrashReporter). Every test runs under the
// app's own look, installed here as WizzairHelper.exe installs it at start-up
// (look_test_support.hpp).
#include "application.hpp"
#include "look_test_support.hpp"

#include <gtest/gtest.h>

int main(int argc, char* argv[])
{
    wizz::app::Application app(argc, argv);
    wizz::test::look::apply_suite_look();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
