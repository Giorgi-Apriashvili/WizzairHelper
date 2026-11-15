// Placeholder entry point; the desktop experience (01) is wired in the GUI milestone.
#include "app_info.hpp"

#include <QApplication>
#include <QMainWindow>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QMainWindow window;
    window.setWindowTitle(wizz::app::main_window_title());
    window.show();
    return QApplication::exec();
}
