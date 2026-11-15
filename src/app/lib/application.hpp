#pragma once
// The QApplication of WizzairHelper.exe. Qt does not let exceptions travel
// through its event dispatch, so every UI-thread entry point (input, paint,
// timers, queued slot calls and posted tasks) is guarded in notify(): an
// exception that escapes an event handler goes to the installed CrashReporter
// (errors.log + crash box, 01 §3.2) and the application keeps running. Without
// an installed reporter the exception propagates as before.

#include <QApplication>

namespace wizz::app {

class Application final : public QApplication {
    Q_OBJECT

public:
    Application(int& argc, char** argv);

    bool notify(QObject* receiver, QEvent* event) override;
};

} // namespace wizz::app
