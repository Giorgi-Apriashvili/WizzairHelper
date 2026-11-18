#pragma once
// Whether Windows animates a window in when it is first shown (DECISIONS D-65).
//
// The desktop window manager plays an open transition of about 300 ms when a top-level
// window appears; while it plays, a GDI flush of the window can block the UI thread for
// over 100 ms, so the main window starts its first work after it (MainWindow::kStartDelay).
// It plays only for a native Win32 window, and the window is taken to animate only while
// every one of these allows it:
//   * "Animation effects" (Settings > Accessibility > Visual effects; Windows 10's "Show
//     animations in Windows", the Ease of Access Center's "Turn off all unnecessary
//     animations"): SPI_GETCLIENTAREAANIMATION. GitHub's Windows Server images have it off.
//   * "Animate windows when minimizing and maximizing" (System Properties > Performance
//     Options): SPI_GETANIMATION's iMinAnimate.
//   * The policy "Do not allow window animations" (DisallowAnimations under
//     Software\Policies\Microsoft\Windows\DWM, per user or per machine): not set.
// Nothing tells whether DWM will animate one particular window (its transitions-off
// attribute can be set but not read), so the settings are read when the window is shown.
// Should one of them not stop the transition after all, such a start is 2.1.0's.

class QWidget;

namespace wizz::app {

/// The settings of this session that decide whether windows animate.
struct WindowAnimationSettings {
    bool client_area_animation = false;  ///< SPI_GETCLIENTAREAANIMATION: "Animation effects"
    bool minimize_animation = false;     ///< SPI_GETANIMATION's iMinAnimate: "Animate windows when …"
    bool policy_disallows = false;       ///< DisallowAnimations (user or machine policy) is set

    /// Windows animates windows: every one of them allows it.
    [[nodiscard]] bool animate() const noexcept
    {
        return client_area_animation && minimize_animation && !policy_disallows;
    }
};

/// The current settings (a setting that cannot be read counts as on, a policy as not set).
[[nodiscard]] WindowAnimationSettings read_window_animation_settings();

/// Whether Windows will animate `window` in when it is shown: it has a native Win32 window
/// (not on the tests' "minimal" platform) and read_window_animation_settings().animate().
[[nodiscard]] bool opens_with_animation(const QWidget& window);

} // namespace wizz::app
