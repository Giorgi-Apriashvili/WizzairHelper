#pragma once
// Taskbar button progress (01 §10.9, DECISIONS D-04): an RAII wrapper over the
// shell's ITaskbarList3 (CLSID 56fdf344-fd6d-11d0-958a-006097c9a090,
// IID ea1afb91-9e28-4b86-90e9-9e9f8a5eefaf), created once and HrInit()-ed.
// Any failure leaves it disabled and every call becomes a no-op; calls are also
// no-ops while the top-level window has no native handle. COM errors of the
// progress calls are ignored.
//
// De-duplication is exactly 01 §10.9 and reuses wizz::ui::TaskbarProgress:
// SetState skips a repeat and forces the next value to be sent; SetValue clamps
// to 0..1000, skips a repeat and implies Normal.
//
// The COM object is injectable (`Factory`) so tests can record the calls.
// UI thread only.

#include "wizz/ui/busy_panel_model.hpp"

#include <QPointer>
#include <QWidget>

#include <functional>
#include <memory>

struct ITaskbarList3;

namespace wizz::app {

/// Releases one COM reference.
struct ComReleaser {
    void operator()(ITaskbarList3* list) const noexcept;
};
/// An owned ITaskbarList3 reference.
using TaskbarListPtr = std::unique_ptr<ITaskbarList3, ComReleaser>;

class TaskbarProgress final {
public:
    /// Creates the (not yet initialised) COM object, or returns null on failure.
    using Factory = std::function<TaskbarListPtr()>;

    /// The shell's TaskbarList through CoCreateInstance (null on failure).
    [[nodiscard]] static TaskbarListPtr create_system_taskbar_list() noexcept;

    /// `anchor` is any widget of the window whose taskbar button shows the progress:
    /// its top-level window's native handle (winId) is looked up at every call. An empty
    /// `factory` uses create_system_taskbar_list. Initialises COM (STA) for this thread
    /// for the wrapper's lifetime and calls HrInit once.
    explicit TaskbarProgress(QWidget* anchor, const Factory& factory = {});
    ~TaskbarProgress();

    TaskbarProgress(const TaskbarProgress&) = delete;
    TaskbarProgress& operator=(const TaskbarProgress&) = delete;

    /// SetProgressState(hwnd, state) unless `state` is the last state sent.
    void set_state(ui::TaskbarState state);
    /// SetProgressValue(hwnd, clamp(permille, 0, 1000), 1000) unless it is the last value sent.
    void set_value(int permille);
    /// Forwards one call recorded by wizz::ui::BusyPanelModel.
    void apply(const ui::TaskbarCall& call);

    /// True when the COM object was created and HrInit succeeded.
    [[nodiscard]] bool available() const noexcept;
    /// The native handle the calls go to, or 0 when the window has none yet.
    [[nodiscard]] WId window_handle() const noexcept;
    [[nodiscard]] ui::TaskbarState last_state() const noexcept;
    [[nodiscard]] int last_value() const noexcept;

private:
    void send_pending();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wizz::app
