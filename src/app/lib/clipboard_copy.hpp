#pragma once
// Puts a grid's copied text on the clipboard as "Copy details" does (01 §8.9): one attempt at
// once, then, while another program holds the clipboard, more attempts kClipboardRetryDelay
// apart — kClipboardAttempts in all, on a timer, never sleeping on the UI thread — and then the
// Information box "The clipboard is busy (another program is using it). Try again.". A new copy
// replaces one still waiting. The clipboard and the box are the owner's GridActions, read at
// each attempt (an owner may replace them, tests do). Shared by the Outbound / Return grids and
// the Round trips grid (DECISIONS D-76). UI thread only.

#include "grid_actions.hpp"

#include <QObject>
#include <QString>
#include <QTimer>

namespace wizz::app {

class ClipboardCopy final : public QObject {
public:
    /// `actions` must outlive this object (a member of the owner).
    ClipboardCopy(const GridActions& actions, QObject* parent);

    /// Starts putting `text` on the clipboard.
    void copy(const QString& text);

private:
    void attempt();

    const GridActions* actions_;
    QTimer retry_;
    QString pending_;
    int attempts_left_ = 0;
};

} // namespace wizz::app
