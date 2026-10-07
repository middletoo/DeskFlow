#include "accessibility.hpp"
#include <commctrl.h>
#include <oleacc.h>
#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <new>
#include <wrl/client.h>

namespace desk {
namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t Property[] = L"DeskFlow.Msaa.Lifetime";
constexpr UINT_PTR SubclassId = 0x44464143;
struct WindowState {
    HWND window = nullptr;
    DWORD thread = 0;
    LONG_PTR identity = 0;
    bool alive = true;
    std::function<AccessibleView()> snapshot;
    std::function<void(int)> activate;
    HRESULT valid() const {
        if (GetCurrentThreadId() != thread) return RPC_E_WRONG_THREAD;
        if (!alive || !IsWindow(window) || GetWindowLongPtrW(window, GWLP_USERDATA) != identity)
            return RPC_E_DISCONNECTED;
        return S_OK;
    }
};
struct WindowLink { std::shared_ptr<WindowState> state; };
LRESULT CALLBACK lifetimeProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                   UINT_PTR, DWORD_PTR data) {
    auto link = reinterpret_cast<WindowLink*>(data);
    if (message == WM_NCDESTROY) {
        link->state->alive = false;
        link->state->snapshot = {};
        link->state->activate = {};
        RemovePropW(window, Property);
        RemoveWindowSubclass(window, lifetimeProcedure, SubclassId);
        delete link;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}
struct Frame {
    AccessibleView view;
    std::vector<HWND> controls;
    long count() const { return static_cast<long>(view.rows.size() + controls.size()); }
};
VARIANT child(long value) {
    VARIANT result{};
    result.vt = VT_I4;
    result.lVal = value;
    return result;
}
RECT clientBounds(HWND window) {
    RECT bounds{};
    GetClientRect(window, &bounds);
    MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&bounds), 2);
    return bounds;
}

class Accessible final : public IAccessible {
    std::atomic<ULONG> references{1};
    std::shared_ptr<WindowState> state;
    ComPtr<ITypeInfo> typeInfo;
    HRESULT frame(Frame& result) const {
        auto status = state->valid();
        if (FAILED(status)) return status;
        try {
            // A callback may reenter message handling; retain its closure even
            // if WM_NCDESTROY clears the state's host references during that call.
            auto snapshot = state->snapshot;
            result.view = snapshot();
            for (HWND item = GetWindow(state->window, GW_CHILD); item; item = GetWindow(item, GW_HWNDNEXT))
                result.controls.push_back(item);
            if (result.view.rows.size() + result.controls.size() > std::numeric_limits<long>::max())
                return E_FAIL;
            return state->valid();
        } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
          catch (...) { return E_FAIL; }
    }
    static HRESULT index(VARIANT value, const Frame& data, long& result) {
        if (value.vt == VT_EMPTY || (value.vt == VT_ERROR && value.scode == DISP_E_PARAMNOTFOUND))
            result = CHILDID_SELF;
        else if (value.vt == VT_I4) result = value.lVal;
        else return E_INVALIDARG;
        return result >= 0 && result <= data.count() ? S_OK : E_INVALIDARG;
    }
    static HRESULT native(const Frame& data, long id, ComPtr<IAccessible>& result) {
        const auto ordinal = static_cast<size_t>(id - 1);
        if (id <= 0 || ordinal < data.view.rows.size() || ordinal >= static_cast<size_t>(data.count()))
            return E_INVALIDARG;
        return AccessibleObjectFromWindow(data.controls[ordinal - data.view.rows.size()],
            static_cast<DWORD>(OBJID_CLIENT), __uuidof(IAccessible),
            reinterpret_cast<void**>(result.GetAddressOf()));
    }
    static HRESULT string(BSTR* output, const std::wstring& text) {
        *output = SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
        return *output ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT read(VARIANT item, Frame& data, long& id) const {
        HRESULT status = frame(data);
        return FAILED(status) ? status : index(item, data, id);
    }
    HRESULT element(const Frame& data, long id, VARIANT* output) {
        if (id <= static_cast<long>(data.view.rows.size())) {
            *output = child(id);
            return S_OK;
        }
        ComPtr<IAccessible> control;
        HRESULT status = native(data, id, control);
        if (FAILED(status)) return status;
        output->vt = VT_DISPATCH;
        return control->QueryInterface(__uuidof(IDispatch), reinterpret_cast<void**>(&output->pdispVal));
    }
    using StringProperty = HRESULT (STDMETHODCALLTYPE IAccessible::*)(VARIANT, BSTR*);
    HRESULT absent(VARIANT item, BSTR* output, StringProperty property) {
        if (!output) return E_POINTER;
        *output = nullptr;
        Frame data; long id = 0;
        HRESULT status = read(item, data, id);
        if (FAILED(status)) return status;
        if (id <= static_cast<long>(data.view.rows.size())) return S_FALSE;
        ComPtr<IAccessible> control;
        status = native(data, id, control);
        return FAILED(status) ? status : (control.Get()->*property)(child(CHILDID_SELF), output);
    }
public:
    explicit Accessible(std::shared_ptr<WindowState> windowState) : state(std::move(windowState)) {
        constexpr GUID libraryId{0x1ea4dbf0, 0x3c3b, 0x11cf,
                                 {0x81, 0x0c, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
        ComPtr<ITypeLib> library;
        if (FAILED(LoadRegTypeLib(libraryId, 1, 1, LOCALE_SYSTEM_DEFAULT, &library)))
            LoadRegTypeLib(libraryId, 1, 0, LOCALE_SYSTEM_DEFAULT, &library);
        if (library) library->GetTypeInfoOfGuid(__uuidof(IAccessible), &typeInfo);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IDispatch) && iid != __uuidof(IAccessible))
            return E_NOINTERFACE;
        *output = static_cast<IAccessible*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override {
        if (!count) return E_POINTER;
        *count = typeInfo ? 1 : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT ordinal, LCID, ITypeInfo** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (ordinal) return DISP_E_BADINDEX;
        if (!typeInfo) return E_NOTIMPL;
        return typeInfo.CopyTo(output);
    }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID iid, LPOLESTR* names, UINT count, LCID,
                                            DISPID* ids) override {
        if (iid != GUID{}) return DISP_E_UNKNOWNINTERFACE;
        if (!names || !ids) return E_POINTER;
        if (!typeInfo) return E_NOTIMPL;
        return DispGetIDsOfNames(typeInfo.Get(), names, count, ids);
    }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID iid, LCID, WORD flags, DISPPARAMS* parameters,
                                      VARIANT* result, EXCEPINFO* exception, UINT* argument) override {
        if (iid != GUID{}) return DISP_E_UNKNOWNINTERFACE;
        if (!parameters) return E_POINTER;
        if (!typeInfo) return E_NOTIMPL;
        const HRESULT status = state->valid();
        if (FAILED(status)) return status;
        return DispInvoke(static_cast<IAccessible*>(this), typeInfo.Get(), id, flags,
                          parameters, result, exception, argument);
    }
    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        HRESULT status = state->valid();
        if (FAILED(status)) return status;
        return CreateStdAccessibleObject(state->window, OBJID_WINDOW, __uuidof(IDispatch),
                                         reinterpret_cast<void**>(output));
    }
    HRESULT STDMETHODCALLTYPE get_accChildCount(long* output) override {
        if (!output) return E_POINTER;
        *output = 0;
        Frame data;
        HRESULT status = frame(data);
        if (FAILED(status)) return status;
        *output = data.count();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT item, IDispatch** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        Frame data; long id = 0;
        HRESULT status = read(item, data, id);
        if (FAILED(status)) return status;
        if (id <= static_cast<long>(data.view.rows.size())) return S_FALSE;
        ComPtr<IAccessible> control;
        status = native(data, id, control);
        return FAILED(status) ? status : control->QueryInterface(__uuidof(IDispatch), reinterpret_cast<void**>(output));
    }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT item, BSTR* output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        Frame data; long id = 0;
        HRESULT status = read(item, data, id);
        if (FAILED(status)) return status;
        if (!id) return string(output, data.view.label);
        if (id <= static_cast<long>(data.view.rows.size())) return string(output, data.view.rows[id - 1].name);
        ComPtr<IAccessible> control;
        status = native(data, id, control);
        return FAILED(status) ? status : control->get_accName(child(CHILDID_SELF), output);
    }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT item, BSTR* output) override {
        return absent(item, output, &IAccessible::get_accValue);
    }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT item, BSTR* output) override {
        return absent(item, output, &IAccessible::get_accDescription);
    }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT item, VARIANT* output) override {
        if (!output) return E_POINTER;
        VariantInit(output);
        Frame data; long id = 0;
        HRESULT status = read(item, data, id);
        if (FAILED(status)) return status;
        if (id <= static_cast<long>(data.view.rows.size())) {
            *output = child(id ? ROLE_SYSTEM_LISTITEM : ROLE_SYSTEM_LIST);
            return S_OK;
        }
        ComPtr<IAccessible> control;
        status = native(data, id, control);
        return FAILED(status) ? status : control->get_accRole(child(CHILDID_SELF), output);
    }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT item, VARIANT* output) override {
        if (!output) return E_POINTER;
        VariantInit(output);
        Frame data; long id = 0;
        HRESULT status = read(item, data, id);
        if (FAILED(status)) return status;
        if (id > static_cast<long>(data.view.rows.size())) {
            ComPtr<IAccessible> control;
            status = native(data, id, control);
            return FAILED(status) ? status : control->get_accState(child(CHILDID_SELF), output);
        }
        long bits = STATE_SYSTEM_FOCUSABLE;
        if (!IsWindowVisible(state->window)) bits |= STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN;
        if (id) {
            bits |= STATE_SYSTEM_SELECTABLE;
            if (data.view.rows[id - 1].selected) bits |= STATE_SYSTEM_SELECTED | STATE_SYSTEM_FOCUSED;
            RECT intersection{}, bounds = clientBounds(state->window);
            if (!IntersectRect(&intersection, &bounds, &data.view.rows[id - 1].bounds)) bits |= STATE_SYSTEM_OFFSCREEN;
        } else if (GetFocus() == state->window) bits |= STATE_SYSTEM_FOCUSED;
        *output = child(bits);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT item, BSTR* output) override {
        return absent(item, output, &IAccessible::get_accHelp);
    }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* file, VARIANT item, long* topic) override {
        if (!file || !topic) return E_POINTER;
        *file = nullptr; *topic = -1;
        Frame data; long id = 0;
        HRESULT status = read(item, data, id);
        if (FAILED(status)) return status;
        if (id <= static_cast<long>(data.view.rows.size())) return S_FALSE;
        ComPtr<IAccessible> control;
        status = native(data, id, control);
        return FAILED(status) ? status : control->get_accHelpTopic(file, child(CHILDID_SELF), topic);
    }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT item, BSTR* output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        Frame data; long id = 0;
        HRESULT status = read(item, data, id);
        if (FAILED(status)) return status;
        if (!id) return S_FALSE;
        if (id <= static_cast<long>(data.view.rows.size())) return string(output, L"Enter");
        ComPtr<IAccessible> control;
        status = native(data, id, control);
        return FAILED(status) ? status : control->get_accKeyboardShortcut(child(CHILDID_SELF), output);
    }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* output) override {
        if (!output) return E_POINTER;
        VariantInit(output);
        Frame data;
        HRESULT status = frame(data);
        if (FAILED(status)) return status;
        for (size_t i = 0; i < data.view.rows.size(); ++i)
            if (data.view.rows[i].selected) return element(data, static_cast<long>(i + 1), output);
        const HWND focus = GetFocus();
        for (size_t i = 0; i < data.controls.size(); ++i)
            if (focus == data.controls[i] || IsChild(data.controls[i], focus))
                return element(data, static_cast<long>(data.view.rows.size() + i + 1), output);
        if (focus == state->window) { *output = child(CHILDID_SELF); return S_OK; }
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* output) override {
        if (!output) return E_POINTER;
        VariantInit(output);
        Frame data;
        HRESULT status = frame(data);
        if (FAILED(status)) return status;
        for (size_t i = 0; i < data.view.rows.size(); ++i)
            if (data.view.rows[i].selected) return element(data, static_cast<long>(i + 1), output);
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT item, BSTR* output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        Frame data; long id = 0;
        HRESULT status = read(item, data, id);
        if (FAILED(status)) return status;
        if (!id) return S_FALSE;
        if (id <= static_cast<long>(data.view.rows.size())) return string(output, L"激活");
        ComPtr<IAccessible> control;
        status = native(data, id, control);
        return FAILED(status) ? status : control->get_accDefaultAction(child(CHILDID_SELF), output);
    }
    HRESULT STDMETHODCALLTYPE accSelect(long flags, VARIANT item) override {
        Frame data; long id = 0;
        HRESULT status = read(item, data, id);
        if (FAILED(status)) return status;
        if (id <= static_cast<long>(data.view.rows.size())) return E_NOTIMPL;
        ComPtr<IAccessible> control;
        status = native(data, id, control);
        return FAILED(status) ? status : control->accSelect(flags, child(CHILDID_SELF));
    }
    HRESULT STDMETHODCALLTYPE accLocation(long* x, long* y, long* width, long* height, VARIANT item) override {
        if (!x || !y || !width || !height) return E_POINTER;
        *x = *y = *width = *height = 0;
        Frame data; long id = 0;
        HRESULT status = read(item, data, id);
        if (FAILED(status)) return status;
        if (id > static_cast<long>(data.view.rows.size())) {
            ComPtr<IAccessible> control;
            status = native(data, id, control);
            return FAILED(status) ? status : control->accLocation(x, y, width, height, child(CHILDID_SELF));
        }
        const RECT bounds = id ? data.view.rows[id - 1].bounds : clientBounds(state->window);
        *x = bounds.left; *y = bounds.top;
        *width = std::max(0L, bounds.right - bounds.left);
        *height = std::max(0L, bounds.bottom - bounds.top);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accNavigate(long direction, VARIANT item, VARIANT* output) override {
        if (!output) return E_POINTER;
        VariantInit(output);
        Frame data; long id = 0;
        HRESULT status = read(item, data, id);
        if (FAILED(status)) return status;
        long next = 0;
        if (direction == NAVDIR_FIRSTCHILD || direction == NAVDIR_LASTCHILD) {
            if (id) return S_FALSE;
            next = direction == NAVDIR_FIRSTCHILD ? 1 : data.count();
        } else if (direction == NAVDIR_NEXT || direction == NAVDIR_DOWN) {
            if (!id) return S_FALSE;
            next = id + 1;
        } else if (direction == NAVDIR_PREVIOUS || direction == NAVDIR_UP) {
            if (!id) return S_FALSE;
            next = id - 1;
        } else return E_NOTIMPL;
        return next >= 1 && next <= data.count() ? element(data, next, output) : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE accHitTest(long x, long y, VARIANT* output) override {
        if (!output) return E_POINTER;
        VariantInit(output);
        Frame data;
        HRESULT status = frame(data);
        if (FAILED(status)) return status;
        POINT point{x, y};
        for (size_t i = 0; i < data.controls.size(); ++i) {
            RECT bounds{};
            if (GetWindowRect(data.controls[i], &bounds) && PtInRect(&bounds, point))
                return element(data, static_cast<long>(data.view.rows.size() + i + 1), output);
        }
        for (size_t i = 0; i < data.view.rows.size(); ++i)
            if (PtInRect(&data.view.rows[i].bounds, point)) return element(data, static_cast<long>(i + 1), output);
        const RECT bounds = clientBounds(state->window);
        if (PtInRect(&bounds, point)) { *output = child(CHILDID_SELF); return S_OK; }
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT item) override {
        Frame data; long id = 0;
        HRESULT status = read(item, data, id);
        if (FAILED(status)) return status;
        if (!id) return S_FALSE;
        if (id <= static_cast<long>(data.view.rows.size())) {
            try { auto action = state->activate; action(id - 1); return S_OK; }
            catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
            catch (...) { return E_FAIL; }
        }
        ComPtr<IAccessible> control;
        status = native(data, id, control);
        return FAILED(status) ? status : control->accDoDefaultAction(child(CHILDID_SELF));
    }
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT item, BSTR value) override {
        Frame data; long id = 0;
        HRESULT status = read(item, data, id);
        if (FAILED(status)) return status;
        if (id <= static_cast<long>(data.view.rows.size())) return E_ACCESSDENIED;
        ComPtr<IAccessible> control;
        status = native(data, id, control);
        return FAILED(status) ? status : control->put_accName(child(CHILDID_SELF), value);
    }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT item, BSTR value) override {
        Frame data; long id = 0;
        HRESULT status = read(item, data, id);
        if (FAILED(status)) return status;
        if (id <= static_cast<long>(data.view.rows.size())) return E_ACCESSDENIED;
        ComPtr<IAccessible> control;
        status = native(data, id, control);
        return FAILED(status) ? status : control->put_accValue(child(CHILDID_SELF), value);
    }
};
} // namespace

LRESULT accessibleObject(HWND hwnd, WPARAM wparam, LPARAM lparam,
                         std::function<AccessibleView()> snapshot,
                         std::function<void(int)> activate) {
    if (static_cast<LONG>(lparam) != OBJID_CLIENT || !snapshot || !activate || !IsWindow(hwnd)) return 0;
    const DWORD thread = GetWindowThreadProcessId(hwnd, nullptr);
    if (thread != GetCurrentThreadId()) return 0;
    try {
        auto link = reinterpret_cast<WindowLink*>(GetPropW(hwnd, Property));
        if (!link) {
            auto state = std::make_shared<WindowState>();
            state->window = hwnd;
            state->thread = thread;
            state->identity = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            auto owned = std::make_unique<WindowLink>();
            owned->state = state;
            if (!SetPropW(hwnd, Property, owned.get())) return 0;
            if (!SetWindowSubclass(hwnd, lifetimeProcedure, SubclassId,
                                   reinterpret_cast<DWORD_PTR>(owned.get()))) {
                RemovePropW(hwnd, Property);
                return 0;
            }
            link = owned.release();
        }
        if (FAILED(link->state->valid())) return 0;
        link->state->snapshot = std::move(snapshot);
        link->state->activate = std::move(activate);
        auto object = new Accessible(link->state);
        const LRESULT result = LresultFromObject(__uuidof(IAccessible), wparam, object);
        object->Release();
        return result;
    } catch (...) { return 0; }
}
} // namespace desk
