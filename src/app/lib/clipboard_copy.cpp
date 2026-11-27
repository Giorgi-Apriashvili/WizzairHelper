#include "clipboard_copy.hpp"

#include "grid_top_bar.hpp"

#include "wizz/ui/strings.hpp"

namespace wizz::app {

ClipboardCopy::ClipboardCopy(const GridActions& actions, QObject* parent) : QObject(parent), actions_(&actions)
{
    retry_.setSingleShot(true);
    retry_.setInterval(kClipboardRetryDelay);
    connect(&retry_, &QTimer::timeout, this, &ClipboardCopy::attempt);
}

void ClipboardCopy::copy(const QString& text)
{
    retry_.stop();
    pending_ = text;
    attempts_left_ = kClipboardAttempts;
    attempt();
}

void ClipboardCopy::attempt()
{
    if (attempts_left_ <= 0) {
        return;
    }
    --attempts_left_;
    if (!actions_->try_set_clipboard || actions_->try_set_clipboard(pending_)) {
        attempts_left_ = 0;
        return;
    }
    if (attempts_left_ > 0) {
        retry_.start();  // another program holds the clipboard: try again shortly
        return;
    }
    if (actions_->show_message) {
        actions_->show_message(QMessageBox::Information, grid_bar::text(ui::strings::kClipboardBusy));
    }
}

} // namespace wizz::app
