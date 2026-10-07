#pragma once
#include <Windows.h>
#include <functional>
#include <string>
#include <vector>

namespace desk {
struct AccessibleRow {
    std::wstring name;
    RECT bounds{}; // Physical screen coordinates, including mixed-DPI monitor origins.
    bool selected = false;
};
struct AccessibleView {
    std::wstring label;
    std::vector<AccessibleRow> rows;
};
// Call only from the owning STA window's WM_GETOBJECT handler. Other object IDs
// return zero so the caller can continue its normal DefWindowProc processing.
LRESULT accessibleObject(HWND hwnd, WPARAM wparam, LPARAM lparam,
                         std::function<AccessibleView()> snapshot,
                         std::function<void(int)> activate);
}
