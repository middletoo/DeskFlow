#include "accessibility.hpp"
#include <oleacc.h>
#include <wrl/client.h>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;
static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static VARIANT child(long id) {
    VARIANT value{};
    value.vt = VT_I4;
    value.lVal = id;
    return value;
}
struct Fixture {
    desk::AccessibleView view{L"测试结果列表", {
        {L"文件：项目资料.txt", {210, 250, 610, 285}, false},
        {L"剪贴板：中文搜索内容", {210, 285, 610, 320}, true},
    }};
    int activated = -1;
    int snapshots = 0;
    DWORD thread = GetCurrentThreadId();
};
static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto state = reinterpret_cast<Fixture*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<Fixture*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (message == WM_GETOBJECT && state) {
        const auto result = desk::accessibleObject(hwnd, wparam, lparam,
            [state] {
                check(GetCurrentThreadId() == state->thread, "Snapshot ran off the owning UI thread");
                ++state->snapshots;
                return state->view;
            },
            [state](int row) {
                check(GetCurrentThreadId() == state->thread, "Default action ran off the owning UI thread");
                state->activated = row;
            });
        if (result) return result;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
static std::wstring accessibleName(IAccessible* object, VARIANT id) {
    BSTR name = nullptr;
    check(object->get_accName(id, &name) == S_OK && name, "Accessible name was not available");
    std::wstring result(name, SysStringLen(name));
    SysFreeString(name);
    return result;
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "Cannot initialize test STA");
    HWND window = nullptr;
    try {
        Fixture fixture;
        WNDCLASSW cls{};
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpfnWndProc = procedure;
        cls.lpszClassName = L"DeskFlowPrivateAccessibilityTests";
        check(RegisterClassW(&cls) != 0, "Cannot register private fixture window");
        window = CreateWindowExW(0, cls.lpszClassName, L"Private test window", WS_OVERLAPPEDWINDOW,
                                 200, 200, 640, 360, nullptr, nullptr, cls.hInstance, &fixture);
        check(window != nullptr, "Cannot create private hidden fixture window");
        HWND edit = CreateWindowExW(0, L"EDIT", L"原生输入", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                   10, 10, 320, 25, window, nullptr, cls.hInstance, nullptr);
        check(edit != nullptr, "Cannot create native input child");
        ComPtr<IAccessible> object;
        check(AccessibleObjectFromWindow(window, static_cast<DWORD>(OBJID_CLIENT),
                                        __uuidof(IAccessible), reinterpret_cast<void**>(object.GetAddressOf())) == S_OK,
              "Cannot retrieve native result-list accessibility object");
        check(accessibleName(object.Get(), child(CHILDID_SELF)) == fixture.view.label,
              "Custom result list has no accessible label");
        long count = 0;
        check(object->get_accChildCount(&count) == S_OK && count == 3,
              "Accessible enumeration must expose two rows and the native edit child");
        check(accessibleName(object.Get(), child(1)) == fixture.view.rows[0].name,
              "First result row name changed");
        VARIANT role{};
        check(object->get_accRole(child(2), &role) == S_OK && role.vt == VT_I4 &&
              role.lVal == ROLE_SYSTEM_LISTITEM, "Result row does not expose the list-item role");
        VariantClear(&role);
        VARIANT state{};
        check(object->get_accState(child(2), &state) == S_OK && state.vt == VT_I4 &&
              (state.lVal & STATE_SYSTEM_SELECTED) && (state.lVal & STATE_SYSTEM_FOCUSED),
              "Selected result row does not expose selection and logical focus");
        VariantClear(&state);
        VARIANT focus{}, selection{};
        check(object->get_accFocus(&focus) == S_OK && focus.vt == VT_I4 && focus.lVal == 2,
              "Logical result-row focus is missing");
        check(object->get_accSelection(&selection) == S_OK && selection.vt == VT_I4 && selection.lVal == 2,
              "Result selection is missing");
        VariantClear(&focus); VariantClear(&selection);
        long x = 0, y = 0, width = 0, height = 0;
        check(object->accLocation(&x, &y, &width, &height, child(1)) == S_OK &&
              x == 210 && y == 250 && width == 400 && height == 35,
              "Row bounds must retain physical screen coordinates");
        VARIANT next{};
        check(object->accNavigate(NAVDIR_NEXT, child(1), &next) == S_OK && next.vt == VT_I4 && next.lVal == 2,
              "Row navigation failed");
        VariantClear(&next);
        check(object->accNavigate(NAVDIR_FIRSTCHILD, child(CHILDID_SELF), &next) == S_OK &&
              next.vt == VT_I4 && next.lVal == 1, "First-child navigation failed");
        VariantClear(&next);
        VARIANT hit{};
        check(object->accHitTest(220, 300, &hit) == S_OK && hit.vt == VT_I4 && hit.lVal == 2,
              "Physical row hit testing failed");
        VariantClear(&hit);
        BSTR action = nullptr;
        check(object->get_accDefaultAction(child(1), &action) == S_OK && action && SysStringLen(action) > 0,
              "Default action label is missing");
        SysFreeString(action);
        check(object->accDoDefaultAction(child(1)) == S_OK && fixture.activated == 0,
              "Accessible default action did not activate the zero-based result index");

        ComPtr<IDispatch> editDispatch;
        check(object->get_accChild(child(3), editDispatch.GetAddressOf()) == S_OK && editDispatch,
              "Native edit is not discoverable from its parent");
        ComPtr<IAccessible> editAccessible;
        check(editDispatch.As(&editAccessible) == S_OK, "Native edit child lacks IAccessible");
        check(editAccessible->get_accRole(child(CHILDID_SELF), &role) == S_OK && role.vt == VT_I4 &&
              role.lVal == ROLE_SYSTEM_TEXT, "Parent provider swallowed native EDIT accessibility");
        VariantClear(&role);
        ComPtr<IAccessible> directlyRetrievedEdit;
        check(AccessibleObjectFromWindow(edit, static_cast<DWORD>(OBJID_CLIENT), __uuidof(IAccessible),
              reinterpret_cast<void**>(directlyRetrievedEdit.GetAddressOf())) == S_OK,
              "Native EDIT must remain individually discoverable");
        VARIANT children[3]{};
        long obtained = 0;
        check(AccessibleChildren(object.Get(), 0, 3, children, &obtained) == S_OK && obtained == 3,
              "MSAA child enumeration failed");
        for (auto& value : children) VariantClear(&value);

        UINT typeInfoCount = 0;
        check(object->GetTypeInfoCount(&typeInfoCount) == S_OK && typeInfoCount == 1,
              "IAccessible dispatch type information is unavailable");
        wchar_t property[] = L"accName";
        wchar_t* names[]{property};
        DISPID dispatchId = 0;
        check(object->GetIDsOfNames(GUID{}, names, 1, LOCALE_USER_DEFAULT, &dispatchId) == S_OK,
              "IAccessible dispatch name lookup failed");
        VARIANT argument = child(1), dispatchName{};
        DISPPARAMS parameters{&argument, nullptr, 1, 0};
        check(object->Invoke(dispatchId, GUID{}, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET,
              &parameters, &dispatchName, nullptr, nullptr) == S_OK && dispatchName.vt == VT_BSTR &&
              std::wstring(dispatchName.bstrVal) == fixture.view.rows[0].name,
              "IAccessible property dispatch failed");
        VariantClear(&dispatchName);
        const int beforeWrongThread = fixture.snapshots;
        HRESULT wrongThread = S_OK;
        std::thread other([&] {
            BSTR name = nullptr;
            wrongThread = object->get_accName(child(1), &name);
            SysFreeString(name);
        });
        other.join();
        check(wrongThread == RPC_E_WRONG_THREAD && fixture.snapshots == beforeWrongThread,
              "Direct off-apartment access must not invoke UI callbacks");
        // A legitimate cross-apartment client uses COM's standard marshaler;
        // its call must execute on the owning STA while that STA pumps messages.
        IStream* stream = nullptr;
        check(CoMarshalInterThreadInterfaceInStream(__uuidof(IAccessible), object.Get(), &stream) == S_OK,
              "Cannot marshal provider to a legitimate cross-apartment client");
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        check(done != nullptr, "Cannot create private COM test completion event");
        HRESULT marshaledStatus = E_FAIL;
        std::wstring marshaledName;
        std::thread marshaled([&] {
            HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            ComPtr<IAccessible> proxy;
            marshaledStatus = CoGetInterfaceAndReleaseStream(stream, __uuidof(IAccessible),
                reinterpret_cast<void**>(proxy.GetAddressOf()));
            if (SUCCEEDED(marshaledStatus)) {
                BSTR name = nullptr;
                marshaledStatus = proxy->get_accName(child(2), &name);
                if (name) marshaledName.assign(name, SysStringLen(name));
                SysFreeString(name);
            }
            proxy.Reset();
            if (SUCCEEDED(initialized)) CoUninitialize();
            SetEvent(done);
        });
        const ULONGLONG deadline = GetTickCount64() + 4000;
        while (WaitForSingleObject(done, 0) != WAIT_OBJECT_0 && GetTickCount64() < deadline) {
            MsgWaitForMultipleObjects(1, &done, FALSE, 25, QS_ALLINPUT);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (WaitForSingleObject(done, 0) != WAIT_OBJECT_0) {
            std::cerr << "FAIL legitimate STA marshaling exceeded the private test's four-second deadline\n";
            ExitProcess(1); // Terminate only this isolated test, never an application process.
        }
        marshaled.join();
        CloseHandle(done);
        check(marshaledStatus == S_OK && marshaledName == fixture.view.rows[1].name,
              "Legitimate cross-apartment access failed to return the owning STA's snapshot");
        BSTR invalidName = nullptr;
        check(object->get_accName(child(99), &invalidName) == E_INVALIDARG && invalidName == nullptr,
              "Invalid row must not alias another result");
        check(desk::accessibleObject(window, 0, OBJID_WINDOW, [] { return desk::AccessibleView{}; },
              [](int) {}) == 0, "Non-client object IDs must preserve standard window behavior");
        const int beforeDestroy = fixture.snapshots;
        DestroyWindow(window);
        window = nullptr;
        check(object->get_accName(child(1), &invalidName) == RPC_E_DISCONNECTED &&
              fixture.snapshots == beforeDestroy, "Destroyed HWND must invalidate captured host callbacks");
        directlyRetrievedEdit.Reset();
        editAccessible.Reset();
        editDispatch.Reset();
        object.Reset();
        CoUninitialize();
        std::cout << "PASS MSAA rows, names, roles, focus, physical bounds, navigation, default action, native EDIT, dispatch, threading and destruction\n";
        return 0;
    } catch (const std::exception& error) {
        if (window) DestroyWindow(window);
        CoUninitialize();
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
