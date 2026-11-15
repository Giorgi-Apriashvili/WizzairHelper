#include "taskbar_progress.hpp"

#include <objbase.h>
#include <shobjidl.h>

namespace wizz::app {

namespace {

/// CoInitializeEx / CoUninitialize for the owning (UI) thread. Qt's Windows platform
/// plugin has normally initialised OLE already (S_FALSE, still balanced here); a thread
/// in the multi-threaded apartment (RPC_E_CHANGED_MODE) can use the object as well.
class ComApartment {
public:
    ComApartment() noexcept : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComApartment()
    {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    HRESULT result_;
};

[[nodiscard]] TBPFLAG to_tbpf(ui::TaskbarState state) noexcept
{
    return static_cast<TBPFLAG>(static_cast<int>(state));
}

// TBPF_* and ui::TaskbarState share their values (01 §10.9).
static_assert(static_cast<int>(ui::TaskbarState::none) == TBPF_NOPROGRESS);
static_assert(static_cast<int>(ui::TaskbarState::indeterminate) == TBPF_INDETERMINATE);
static_assert(static_cast<int>(ui::TaskbarState::normal) == TBPF_NORMAL);
static_assert(static_cast<int>(ui::TaskbarState::error) == TBPF_ERROR);
static_assert(static_cast<int>(ui::TaskbarState::paused) == TBPF_PAUSED);

} // namespace

void ComReleaser::operator()(ITaskbarList3* list) const noexcept
{
    if (list != nullptr) {
        list->Release();
    }
}

struct TaskbarProgress::Impl {
    ComApartment apartment;  // declared first: released last, after `list`
    TaskbarListPtr list;
    QPointer<QWidget> anchor;
    ui::TaskbarProgress dedup;
};

TaskbarListPtr TaskbarProgress::create_system_taskbar_list() noexcept
{
    ITaskbarList3* list = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(TaskbarList), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list)))) {
        return {};
    }
    return TaskbarListPtr{list};
}

TaskbarProgress::TaskbarProgress(QWidget* anchor, const Factory& factory) : impl_(std::make_unique<Impl>())
{
    impl_->anchor = anchor;
    impl_->list = factory ? factory() : create_system_taskbar_list();
    if (impl_->list && FAILED(impl_->list->HrInit())) {
        impl_->list.reset();
    }
}

TaskbarProgress::~TaskbarProgress() = default;

bool TaskbarProgress::available() const noexcept
{
    return impl_->list != nullptr;
}

WId TaskbarProgress::window_handle() const noexcept
{
    const QWidget* anchor = impl_->anchor.data();
    if (anchor == nullptr) {
        return 0;
    }
    // internalWinId() never creates the native window (winId() would, and on a child
    // widget it would even turn that child into a native window).
    return anchor->window()->internalWinId();
}

ui::TaskbarState TaskbarProgress::last_state() const noexcept
{
    return impl_->dedup.last_state();
}

int TaskbarProgress::last_value() const noexcept
{
    return impl_->dedup.last_value();
}

void TaskbarProgress::set_state(ui::TaskbarState state)
{
    if (!available() || window_handle() == 0) {
        return;
    }
    impl_->dedup.set_state(state);
    send_pending();
}

void TaskbarProgress::set_value(int permille)
{
    if (!available() || window_handle() == 0) {
        return;
    }
    impl_->dedup.set_value(permille);
    send_pending();
}

void TaskbarProgress::apply(const ui::TaskbarCall& call)
{
    switch (call.kind) {
    case ui::TaskbarCall::Kind::set_state:
        set_state(call.state);
        break;
    case ui::TaskbarCall::Kind::set_value:
        set_value(call.value);
        break;
    }
}

void TaskbarProgress::send_pending()
{
    const auto hwnd = reinterpret_cast<HWND>(window_handle());
    for (const ui::TaskbarCall& call : impl_->dedup.take_calls()) {
        // COM errors are ignored (01 §10.9): the de-duplication state moves on regardless.
        switch (call.kind) {
        case ui::TaskbarCall::Kind::set_state:
            static_cast<void>(impl_->list->SetProgressState(hwnd, to_tbpf(call.state)));
            break;
        case ui::TaskbarCall::Kind::set_value:
            static_cast<void>(impl_->list->SetProgressValue(hwnd, static_cast<ULONGLONG>(call.value),
                                                            static_cast<ULONGLONG>(ui::TaskbarProgress::kMaxValue)));
            break;
        }
    }
}

} // namespace wizz::app
