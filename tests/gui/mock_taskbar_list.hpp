#pragma once
// A recording ITaskbarList3 for TaskbarProgress / BusyPanel tests: no shell, no
// taskbar button, just the calls the wrapper makes (01 §10.9).

#include "taskbar_progress.hpp"

#include <shobjidl.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace wizz::test {

class MockTaskbarList final : public ITaskbarList3 {
public:
    struct Call {
        enum class Kind { set_state, set_value };
        Kind kind = Kind::set_state;
        HWND window = nullptr;
        TBPFLAG state = TBPF_NOPROGRESS;
        ULONGLONG completed = 0;
        ULONGLONG total = 0;
    };

    HRESULT init_result = S_OK;
    HRESULT call_result = S_OK;
    int init_calls = 0;
    std::vector<Call> calls;

    [[nodiscard]] ULONG references() const noexcept { return references_.load(); }

    /// A factory handing TaskbarProgress one more reference to this mock.
    [[nodiscard]] app::TaskbarProgress::Factory factory()
    {
        return [this] {
            AddRef();
            return app::TaskbarListPtr{this};
        };
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ITaskbarList) || riid == __uuidof(ITaskbarList2) ||
            riid == __uuidof(ITaskbarList3)) {
            *object = static_cast<ITaskbarList3*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { return --references_; }  // owned by the test, never deleted

    // ITaskbarList
    HRESULT STDMETHODCALLTYPE HrInit() override
    {
        ++init_calls;
        return init_result;
    }
    HRESULT STDMETHODCALLTYPE AddTab(HWND) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DeleteTab(HWND) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ActivateTab(HWND) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetActiveAlt(HWND) override { return E_NOTIMPL; }

    // ITaskbarList2
    HRESULT STDMETHODCALLTYPE MarkFullscreenWindow(HWND, BOOL) override { return E_NOTIMPL; }

    // ITaskbarList3
    HRESULT STDMETHODCALLTYPE SetProgressValue(HWND window, ULONGLONG completed, ULONGLONG total) override
    {
        calls.push_back(Call{.kind = Call::Kind::set_value, .window = window, .completed = completed, .total = total});
        return call_result;
    }
    HRESULT STDMETHODCALLTYPE SetProgressState(HWND window, TBPFLAG state) override
    {
        calls.push_back(Call{.kind = Call::Kind::set_state, .window = window, .state = state});
        return call_result;
    }
    HRESULT STDMETHODCALLTYPE RegisterTab(HWND, HWND) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnregisterTab(HWND) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetTabOrder(HWND, HWND) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetTabActive(HWND, HWND, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ThumbBarAddButtons(HWND, UINT, LPTHUMBBUTTON) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ThumbBarUpdateButtons(HWND, UINT, LPTHUMBBUTTON) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ThumbBarSetImageList(HWND, HIMAGELIST) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetOverlayIcon(HWND, HICON, LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetThumbnailTooltip(HWND, LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetThumbnailClip(HWND, RECT*) override { return E_NOTIMPL; }

private:
    std::atomic<ULONG> references_{1};
};

/// Shorthands for expected calls.
[[nodiscard]] inline bool is_state_call(const MockTaskbarList::Call& call, TBPFLAG state)
{
    return call.kind == MockTaskbarList::Call::Kind::set_state && call.state == state;
}
[[nodiscard]] inline bool is_value_call(const MockTaskbarList::Call& call, ULONGLONG completed)
{
    return call.kind == MockTaskbarList::Call::Kind::set_value && call.completed == completed && call.total == 1000;
}

} // namespace wizz::test
