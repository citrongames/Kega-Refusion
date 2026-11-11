#include "WindowsMenuTheme.h"

#ifdef Q_OS_WIN

#include <QOperatingSystemVersion>
#include <uxtheme.h>
#include <commctrl.h>

#include <algorithm>
#include <string>

namespace {

// Undocumented UAH drawing ABI described by the original MIT-licensed research:
// https://github.com/adzm/win32-custom-menubar-aero-theme
// Only the prefix we read is declared; the trailing item metrics are untouched.
constexpr UINT uahDrawMenu = 0x0091;
constexpr UINT uahDrawMenuItem = 0x0092;

struct UahMenu {
    HMENU menu;
    HDC dc;
    DWORD flags;
};

struct UahDrawMenuItem {
    DRAWITEMSTRUCT item;
    UahMenu menu;
    int position;
};

constexpr COLORREF background = RGB(32, 32, 32);
constexpr COLORREF hoverBackground = RGB(53, 53, 53);
constexpr COLORREF selectedBackground = RGB(65, 65, 65);
constexpr COLORREF textColor = RGB(235, 235, 235);
constexpr COLORREF inactiveTextColor = RGB(160, 160, 160);
constexpr COLORREF disabledTextColor = RGB(112, 112, 112);

class SavedDc final {
public:
    explicit SavedDc(HDC dc) : dc_(dc), saved_(SaveDC(dc)) {}
    ~SavedDc() { if (saved_) RestoreDC(dc_, saved_); }
    explicit operator bool() const { return saved_ != 0; }
    SavedDc(const SavedDc&) = delete;
    SavedDc& operator=(const SavedDc&) = delete;
private:
    HDC dc_;
    int saved_;
};

class MenuFont final {
public:
    explicit MenuFont(HWND window, bool bold) {
        // These APIs are available on the Windows 10 baseline required by Qt 6.
        // Query for every menu paint, so monitor DPI/settings/HWND replacement
        // cannot leave a cached font or theme handle attached to an old window.
        const UINT dpi = GetDpiForWindow(window);
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (dpi && SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,
                sizeof(metrics), &metrics, 0, dpi)) {
            if (bold) metrics.lfMenuFont.lfWeight = FW_BOLD;
            font_ = CreateFontIndirectW(&metrics.lfMenuFont);
        }
    }
    ~MenuFont() { if (font_) DeleteObject(font_); }
    HFONT get() const { return font_; }
    MenuFont(const MenuFont&) = delete;
    MenuFont& operator=(const MenuFont&) = delete;
private:
    HFONT font_ = nullptr;
};

bool menuBarRect(HWND window, HMENU menu, RECT& rectangle) {
    if (!window || !menu || GetMenu(window) != menu) return false;
    MENUBARINFO bar{};
    bar.cbSize = sizeof(bar);
    RECT windowRect{};
    RECT clientRect{};
    if (!GetMenuBarInfo(window, OBJID_MENU, 0, &bar) || bar.hMenu != menu
            || !GetWindowRect(window, &windowRect)
            || !GetClientRect(window, &clientRect) || IsRectEmpty(&bar.rcBar)) {
        return false;
    }
    // The UAH DC and item rectangles use window coordinates. Native menu/client
    // metrics already include the current DPI; scaling them again is incorrect.
    // Mapping two points together also normalizes a mirrored window's rectangle.
    SetLastError(ERROR_SUCCESS);
    if (!MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&clientRect), 2)
            && GetLastError() != ERROR_SUCCESS) {
        return false;
    }
    rectangle = bar.rcBar;
    rectangle.left = std::min(rectangle.left, clientRect.left);
    rectangle.right = std::max(rectangle.right, clientRect.right);
    // Include unused background and the nonclient separator above the client,
    // but never the first client pixel, even for wrapped/multiple menu rows.
    rectangle.bottom = clientRect.top;
    OffsetRect(&rectangle, -windowRect.left, -windowRect.top);
    return !IsRectEmpty(&rectangle);
}

bool fill(HDC dc, const RECT& rectangle, COLORREF color) {
    SetDCBrushColor(dc, color);
    return FillRect(dc, &rectangle, static_cast<HBRUSH>(GetStockObject(DC_BRUSH))) != 0;
}

// USER32 paints the native separator after the themed menu callbacks. Finish
// the single nonclient scanline after the normal window procedure has returned.
LRESULT CALLBACK finishMenuPaint(HWND window, UINT msg, WPARAM wParam, LPARAM lParam,
                                 UINT_PTR id, DWORD_PTR) {
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(window, finishMenuPaint, id);
    const auto result = DefSubclassProc(window, msg, wParam, lParam);
    const bool printing = msg == WM_PRINT && (lParam & PRF_NONCLIENT);
    if (!printing && msg != WM_NCPAINT && !(msg == WM_NCACTIVATE && lParam != -1))
        return result;
    RECT band{};
    if (!menuBarRect(window, GetMenu(window), band)) return result;
    band.top = band.bottom - 1;
    HDC dc = printing ? reinterpret_cast<HDC>(wParam) : GetWindowDC(window);
    if (dc) {
        {
            const SavedDc saved(dc);
            if (saved) fill(dc, band, background);
        }
        if (!printing) ReleaseDC(window, dc);
    }
    return result;
}

bool drawBackground(const MSG& message) {
    const auto* menu = reinterpret_cast<const UahMenu*>(message.lParam);
    RECT rectangle{};
    if (!menu || !menu->dc || !menuBarRect(message.hwnd, menu->menu, rectangle)) {
        return false;
    }
    const SavedDc saved(menu->dc);
    return saved && fill(menu->dc, rectangle, background);
}

bool drawItem(const MSG& message) {
    const auto* drawing = reinterpret_cast<const UahDrawMenuItem*>(message.lParam);
    RECT barRect{};
    if (!drawing || !drawing->menu.dc || drawing->position < 0
            || !menuBarRect(message.hwnd, drawing->menu.menu, barRect)) {
        return false;
    }
    MENUITEMINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_STRING | MIIM_FTYPE | MIIM_STATE | MIIM_BITMAP;
    const auto position = static_cast<UINT>(drawing->position);
    if (!GetMenuItemInfoW(drawing->menu.menu, position, TRUE, &info)
            || (info.fType & (MFT_OWNERDRAW | MFT_BITMAP | MFT_SEPARATOR))
            || info.hbmpItem || info.cch > 32767) {
        return false;
    }
    std::wstring label(info.cch + 1, L'\0');
    const UINT itemType = info.fType;
    const UINT itemState = info.fState;
    info.fMask = MIIM_STRING;
    info.dwTypeData = label.data();
    info.cch = static_cast<UINT>(label.size());
    if (!GetMenuItemInfoW(drawing->menu.menu, position, TRUE, &info)) return false;

    const UINT state = drawing->item.itemState;
    const bool disabled = (state & (ODS_DISABLED | ODS_GRAYED))
            || (itemState & MFS_DISABLED);
    const bool inactive = (state & ODS_INACTIVE) != 0;
    const bool selected = (state & (ODS_SELECTED | ODS_FOCUS)) != 0;
    const bool hot = (state & ODS_HOTLIGHT) != 0;
    const MenuFont font(message.hwnd, (state & ODS_DEFAULT) || (itemState & MFS_DEFAULT));
    if (!font.get()) return false;
    // Restore the DC before MenuFont is destroyed, so it is never deleted while
    // selected. The caller owns the DC; every color, font and clip is restored.
    const SavedDc saved(drawing->menu.dc);
    if (!saved) return false;
    HDC dc = drawing->menu.dc;
    RECT itemRect = drawing->item.rcItem;
    RECT clip{};
    if (!IntersectRect(&clip, &itemRect, &barRect)) return false;
    if (IntersectClipRect(dc, clip.left, clip.top, clip.right, clip.bottom) == ERROR) {
        return false;
    }
    COLORREF color = background;
    if (!inactive && !disabled) {
        if (selected) color = selectedBackground;
        else if (hot) color = hoverBackground;
    }
    if (!fill(dc, itemRect, color)) return false;
    if (!SelectObject(dc, font.get())) return false;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? disabledTextColor : inactive ? inactiveTextColor : textColor);
    UINT format = DT_CENTER | DT_VCENTER | DT_SINGLELINE;
    if (state & ODS_NOACCEL) format |= DT_HIDEPREFIX;
    const auto exStyle = GetWindowLongPtrW(message.hwnd, GWL_EXSTYLE);
    if ((itemType & MFT_RIGHTORDER) || (exStyle & (WS_EX_LAYOUTRTL | WS_EX_RTLREADING))) {
        format |= DT_RTLREADING;
    }
    return !info.cch || DrawTextW(dc, label.data(), static_cast<int>(info.cch),
                                 &itemRect, format) != 0;
}

} // namespace

bool WindowsMenuTheme::drawMenuBar(const MSG& message, qintptr* result) {
    bool handled = false;
    switch (message.message) {
    case uahDrawMenu: handled = drawBackground(message); break;
    case uahDrawMenuItem: handled = drawItem(message); break;
    default: return false;
    }
    if (handled && result) *result = 0;
    return handled;
}


namespace {
// UxTheme uses two different signatures for ordinal 135 on Windows 10.
// Link only documented APIs; an unavailable dark-menu interface keeps the native theme.
struct PopupThemeApi {
    enum class AppMode { Default, AllowDark };
    using SetMode = AppMode (WINAPI*)(AppMode);
    using SetLegacyMode = bool (WINAPI*)(bool);
    using AllowWindow = bool (WINAPI*)(HWND, bool);
    using Flush = void (WINAPI*)();

    SetMode setMode = nullptr;
    SetLegacyMode setLegacyMode = nullptr;
    AllowWindow allowWindow = nullptr;
    Flush flush = nullptr;

    PopupThemeApi() {
        const auto version = QOperatingSystemVersion::current();
        if (version.majorVersion() != 10 || version.microVersion() < 17763) return;
        // IsThemeActive is a regular import, so UxTheme stays loaded for our lifetime.
        const auto library = GetModuleHandleW(L"uxtheme.dll");
        if (!library) return;
        const auto mode = GetProcAddress(library, MAKEINTRESOURCEA(135));
        if (version.microVersion() >= 18362)
            setMode = reinterpret_cast<SetMode>(mode);
        else
            setLegacyMode = reinterpret_cast<SetLegacyMode>(mode);
        allowWindow = reinterpret_cast<AllowWindow>(GetProcAddress(library, MAKEINTRESOURCEA(133)));
        flush = reinterpret_cast<Flush>(GetProcAddress(library, MAKEINTRESOURCEA(136)));
    }
};

const PopupThemeApi& popupApi() {
    static const PopupThemeApi api;
    return api;
}
}

void WindowsMenuTheme::refreshMenuBar(HWND hwnd, bool dark) {
    const HMENU menu = GetMenu(hwnd);
    if (!menu) return;
    // A borderless window can use the classic menu background instead of UAH.
    // Set the root brush only; popup menus keep their native theme.
    struct BackgroundBrush {
        HBRUSH handle = CreateSolidBrush(background);
        ~BackgroundBrush() { if (handle) DeleteObject(handle); }
    };
    static const BackgroundBrush brush;
    MENUINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = MIM_BACKGROUND;
    info.hbrBack = dark ? brush.handle : nullptr;
    SetMenuInfo(menu, &info);
    DrawMenuBar(hwnd);
}

bool WindowsMenuTheme::systemPrefersDark() {
    const auto& api = popupApi();
    if (!IsThemeActive() || !api.allowWindow || (!api.setMode && !api.setLegacyMode)) return false;
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) ||
        (contrast.dwFlags & HCF_HIGHCONTRASTON)) return false;

    DWORD light = 1;
    DWORD size = sizeof(light);
    return RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
        RRF_RT_REG_DWORD, nullptr, &light, &size) == ERROR_SUCCESS && light == 0;
}

void WindowsMenuTheme::applyPopupTheme(HWND hwnd, bool dark) {
    if (dark) SetWindowSubclass(hwnd, finishMenuPaint, 1, 0);
    else RemoveWindowSubclass(hwnd, finishMenuPaint, 1);
    const auto& api = popupApi();
    if (!api.allowWindow || (!api.setMode && !api.setLegacyMode)) return;
    if (api.setMode) api.setMode(dark ? PopupThemeApi::AppMode::AllowDark : PopupThemeApi::AppMode::Default);
    else api.setLegacyMode(dark);
    api.allowWindow(hwnd, dark);
    if (api.flush) api.flush();
}

#endif
