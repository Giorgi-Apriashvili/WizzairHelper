// GoogleTest entry point that owns the QApplication for widget tests.
#include <QApplication>

#include <gtest/gtest.h>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
