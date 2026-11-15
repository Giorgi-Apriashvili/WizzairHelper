// GoogleTest entry point that owns the QApplication for widget tests. It is the
// app's own Application, so the crash guard in notify() is under test too
// (inactive unless a test installs a CrashReporter).
#include "application.hpp"

#include <gtest/gtest.h>

int main(int argc, char* argv[])
{
    wizz::app::Application app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
