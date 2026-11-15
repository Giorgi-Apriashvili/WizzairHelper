// Placeholder entry point; the desktop experience (01) is implemented in a later milestone.
#include <QApplication>
#include <QMainWindow>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QMainWindow window;
    window.setWindowTitle(QStringLiteral("WizzairHelper — cheapest Wizz Air flights from nearby airports"));
    window.show();
    return QApplication::exec();
}
