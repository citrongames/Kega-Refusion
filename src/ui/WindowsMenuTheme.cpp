#include "WindowsMenuTheme.h"

#ifdef Q_OS_WIN

#include <QGuiApplication>
#include <QOperatingSystemVersion>
#include <QPalette>
#include <uxtheme.h>
#include <commctrl.h>
#include <vsstyle.h>
#include <vssym32.h>

#include <algorithm>
#include <array>
#include <cmath>
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

bool fill(HDC dc, const RECT& rectangle, COLORREF color) {
    SetDCBrushColor(dc, color);
    return FillRect(dc, &rectangle, static_cast<HBRUSH>(GetStockObject(DC_BRUSH))) != 0;
}

bool highContrastEnabled() {
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)
            && (contrast.dwFlags & HCF_HIGHCONTRASTON);
}

COLORREF nativeColor(const QColor& color) {
    return RGB(color.red(), color.green(), color.blue());
}

bool darkColor(COLORREF color) {
    return qGray(GetRValue(color), GetGValue(color), GetBValue(color)) < 128;
}

double luminance(COLORREF color) {
    const auto channel = [](BYTE value) {
        const double component = value / 255.0;
        return component <= 0.04045 ? component / 12.92
                : std::pow((component + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(GetRValue(color)) + 0.7152 * channel(GetGValue(color))
            + 0.0722 * channel(GetBValue(color));
}

bool readable(COLORREF background, COLORREF text, bool disabled = false) {
    const double a = luminance(background), b = luminance(text);
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05) >= (disabled ? 1.25 : 3.0);
}

struct MenuVisual {
    COLORREF background = 0;
    COLORREF text = 0;
    HTHEME theme = nullptr;
    int part = 0;
    int state = 0;
};

// Some Windows versions expose light BARITEM parts even for dark menu classes.
// Render the actual parts once when settings change, rather than assuming that
// OpenThemeData succeeding means the class has a usable dark implementation.
class ThemeSampler final {
public:
    explicit ThemeSampler(HWND window) {
        HDC screen = GetDC(window);
        if (!screen) return;
        dc_ = CreateCompatibleDC(screen);
        bitmap_ = CreateCompatibleBitmap(screen, rectangle_.right, rectangle_.bottom);
        ReleaseDC(window, screen);
        if (dc_ && bitmap_) previous_ = SelectObject(dc_, bitmap_);
    }
    ~ThemeSampler() {
        if (dc_ && previous_) SelectObject(dc_, previous_);
        if (bitmap_) DeleteObject(bitmap_);
        if (dc_) DeleteDC(dc_);
    }
    bool color(HTHEME theme, int part, int state, COLORREF base, COLORREF& result) {
        // Normal menu items can inherit a theme part. DrawThemeBackground
        // resolves it even when IsThemePartDefined reports no explicit state.
        if (!dc_ || !previous_
                || !fill(dc_, rectangle_, base)
                || FAILED(DrawThemeBackground(theme, dc_, part, state, &rectangle_, nullptr)))
            return false;
        result = GetPixel(dc_, rectangle_.right / 2, rectangle_.bottom / 2);
        return result != CLR_INVALID;
    }
private:
    RECT rectangle_{0, 0, 80, 30};
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
};

class MenuPaintResources final {
public:
    ~MenuPaintResources() { reset(); }
    HWND window = nullptr;
    bool dark = false;
    bool highContrast = false;
    UINT dpi = 0;
    COLORREF background = 0;
    COLORREF inactiveText = 0;
    COLORREF selectedBorder = 0;
    bool frameSelected = false;
    std::array<MenuVisual, MBI_DISABLEDPUSHED> visuals{};

    HFONT font(bool bold) const {
        const HFONT selected = fonts_[bold ? 1 : 0];
        return selected ? selected : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    void refresh(HWND hwnd, bool darkMode) {
        closeThemeAndFonts();
        window = hwnd;
        dpi = GetDpiForWindow(hwnd);
        if (!dpi) dpi = GetDpiForSystem();
        highContrast = highContrastEnabled();
        dark = darkMode && !highContrast;
        systemPalette();
        if (!highContrast) {
            if (dark) darkPaletteFallback();
            if (IsThemeActive()) {
                ThemeSampler sampler(hwnd);
                if (dark) loadDarkTheme(sampler);
                else loadBarTheme(sampler);
            }
        }
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)
                || SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
            fonts_[0] = CreateFontIndirectW(&metrics.lfMenuFont);
            metrics.lfMenuFont.lfWeight = FW_BOLD;
            fonts_[1] = CreateFontIndirectW(&metrics.lfMenuFont);
        }
        replaceBrush();
    }
    void reset() {
        // A hidden fullscreen bar is still a live, detached HMENU. Remove our
        // brush from that menu before deleting it, as well as on HWND replacement.
        if (brushedMenu_ && IsMenu(brushedMenu_)) setBrush(brushedMenu_, nullptr);
        brushedMenu_ = nullptr;
        if (ownsBrush_ && brush_) DeleteObject(brush_);
        brush_ = nullptr;
        ownsBrush_ = false;
        closeThemeAndFonts();
        window = nullptr;
        dpi = 0;
    }
    void applyBrush(HMENU menu) {
        if (brushedMenu_ != menu && brushedMenu_ && IsMenu(brushedMenu_))
            setBrush(brushedMenu_, nullptr);
        if (setBrush(menu, brush_)) brushedMenu_ = menu;
    }
private:
    HTHEME theme_ = nullptr;
    std::array<HFONT, 2> fonts_{};
    HMENU brushedMenu_ = nullptr;
    HBRUSH brush_ = nullptr;
    bool ownsBrush_ = false;

    static bool setBrush(HMENU menu, HBRUSH brush) {
        MENUINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = MIM_BACKGROUND;
        info.hbrBack = brush;
        return SetMenuInfo(menu, &info) != 0;
    }
    void replaceBrush() {
        HBRUSH replacement = nullptr;
        bool owned = false;
        if (background == GetSysColor(COLOR_MENUBAR))
            replacement = GetSysColorBrush(COLOR_MENUBAR);
        else if (background == GetSysColor(COLOR_MENU))
            replacement = GetSysColorBrush(COLOR_MENU);
        else {
            replacement = CreateSolidBrush(background);
            owned = true;
        }
        if (!replacement) return;
        if (brushedMenu_ && IsMenu(brushedMenu_) && !setBrush(brushedMenu_, replacement)) {
            if (owned) DeleteObject(replacement);
            return;
        }
        if (ownsBrush_ && brush_) DeleteObject(brush_);
        brush_ = replacement;
        ownsBrush_ = owned;
    }
    void closeThemeAndFonts() {
        if (theme_) CloseThemeData(theme_);
        theme_ = nullptr;
        for (auto& font : fonts_) {
            if (font) DeleteObject(font);
            font = nullptr;
        }
    }
    void systemPalette() {
        BOOL flat = FALSE;
        SystemParametersInfoW(SPI_GETFLATMENU, 0, &flat, 0);
        background = GetSysColor(flat ? COLOR_MENUBAR : COLOR_MENU);
        const COLORREF text = GetSysColor(COLOR_MENUTEXT);
        const COLORREF disabled = GetSysColor(COLOR_GRAYTEXT);
        visuals[MBI_NORMAL - 1] = {background, text};
        visuals[MBI_HOT - 1] = {GetSysColor(COLOR_MENUHILIGHT), GetSysColor(COLOR_HIGHLIGHTTEXT)};
        visuals[MBI_PUSHED - 1] = {GetSysColor(COLOR_HIGHLIGHT), GetSysColor(COLOR_HIGHLIGHTTEXT)};
        visuals[MBI_DISABLED - 1] = {background, disabled};
        visuals[MBI_DISABLEDHOT - 1] = {background, disabled};
        visuals[MBI_DISABLEDPUSHED - 1] = {background, disabled};
        inactiveText = text;
        selectedBorder = GetSysColor(COLOR_WINDOWTEXT);
        frameSelected = visuals[MBI_HOT - 1].background == visuals[MBI_PUSHED - 1].background;
    }
    void darkPaletteFallback() {
        const auto palette = QGuiApplication::palette();
        const auto base = nativeColor(palette.color(QPalette::Window));
        const auto text = nativeColor(palette.color(QPalette::WindowText));
        // Qt can still carry a light palette during a Windows theme transition.
        // In that case retain paired native system colors until dark parts load.
        if (!darkColor(base) || !readable(base, text)) return;
        background = base;
        visuals[MBI_NORMAL - 1] = {base, text};
        auto disabled = nativeColor(palette.color(QPalette::Disabled, QPalette::WindowText));
        if (!readable(base, disabled, true)) disabled = GetSysColor(COLOR_GRAYTEXT);
        if (!readable(base, disabled, true)) disabled = text;
        visuals[MBI_DISABLED - 1] = {base, disabled};
        visuals[MBI_DISABLEDHOT - 1] = visuals[MBI_DISABLED - 1];
        visuals[MBI_DISABLEDPUSHED - 1] = visuals[MBI_DISABLED - 1];
        const auto hot = nativeColor(palette.color(QPalette::Button));
        const auto hotText = nativeColor(palette.color(QPalette::ButtonText));
        visuals[MBI_HOT - 1] = readable(hot, hotText) ? MenuVisual{hot, hotText} : visuals[MBI_NORMAL - 1];
        visuals[MBI_PUSHED - 1] = visuals[MBI_HOT - 1];
        inactiveText = nativeColor(palette.color(QPalette::Inactive, QPalette::WindowText));
        if (!readable(base, inactiveText)) inactiveText = text;
        frameSelected = false;
    }
    bool readVisual(ThemeSampler& sampler, HTHEME theme, int part, int state,
                    COLORREF base, bool expectDark, bool disabled, MenuVisual& visual) {
        COLORREF fillColor{}, textColor{};
        if (!sampler.color(theme, part, state, base, fillColor)
                || darkColor(fillColor) != expectDark
                || FAILED(GetThemeColor(theme, part, state, TMT_TEXTCOLOR, &textColor))
                || !readable(fillColor, textColor, disabled)) return false;
        visual = {fillColor, textColor, theme, part, state};
        return true;
    }
    void loadBarTheme(ThemeSampler& sampler) {
        HTHEME theme = OpenThemeData(window, L"Menu");
        if (!theme) return;
        COLORREF base{};
        std::array<MenuVisual, MBI_DISABLEDPUSHED> native{};
        bool valid = sampler.color(theme, MENU_BARBACKGROUND, MB_ACTIVE, background, base)
                && !darkColor(base);
        for (int state = MBI_NORMAL; valid && state <= MBI_DISABLEDPUSHED; ++state)
            valid = readVisual(sampler, theme, MENU_BARITEM, state, base, false,
                               state >= MBI_DISABLED, native[state - 1]);
        if (!valid) {
            CloseThemeData(theme);
            return;
        }
        theme_ = theme;
        visuals = native;
        background = base;
        inactiveText = native[MBI_NORMAL - 1].text;
        frameSelected = false;
    }
    void loadDarkTheme(ThemeSampler& sampler) {
        constexpr const wchar_t* classes[] = {L"DarkMode_ImmersiveStart::Menu", L"Menu",
                                              L"DarkMode::Menu", L"DarkMode_Explorer::Menu"};
        for (const auto* name : classes) {
            HTHEME theme = OpenThemeData(window, name);
            if (!theme) continue;
            MenuVisual normal{}, hot{}, disabled{}, disabledHot{};
            const bool valid = readVisual(sampler, theme, MENU_POPUPITEM, MPI_NORMAL,
                                           background, true, false, normal)
                    && readVisual(sampler, theme, MENU_POPUPITEM, MPI_HOT,
                                  normal.background, true, false, hot)
                    && readVisual(sampler, theme, MENU_POPUPITEM, MPI_DISABLED,
                                  normal.background, true, true, disabled);
            if (!valid) {
                CloseThemeData(theme);
                continue;
            }
            if (!readVisual(sampler, theme, MENU_POPUPITEM, MPI_DISABLEDHOT,
                            normal.background, true, true, disabledHot)) disabledHot = disabled;
            theme_ = theme;
            background = normal.background;
            inactiveText = normal.text;
            // The dark popup theme has one highlight for an active item.
            // Keep it while the heading's dropdown is open.
            visuals = {normal, hot, hot, disabled, disabledHot, disabledHot};
            frameSelected = false;
            break;
        }
    }
};

MenuPaintResources& menuPaintResources(HWND window) {
    static MenuPaintResources resources;
    if (window && (resources.window != window || resources.dpi != GetDpiForWindow(window)))
        resources.refresh(window, resources.window == window ? resources.dark
                                                           : WindowsMenuTheme::systemPrefersDark());
    return resources;
}

class MenuFont final {
public:
    explicit MenuFont(HWND window, bool bold) : font_(menuPaintResources(window).font(bold)) {}
    HFONT get() const { return font_; }
    MenuFont(const MenuFont&) = delete;
    MenuFont& operator=(const MenuFont&) = delete;
private:
    HFONT font_;
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


bool measureHeading(HWND window, HMENU bar, MEASUREITEMSTRUCT& item);

// USER32 paints the native separator after the themed menu callbacks. Finish
// the single nonclient scanline after the normal window procedure has returned.
LRESULT CALLBACK finishMenuPaint(HWND window, UINT msg, WPARAM wParam, LPARAM lParam,
                                 UINT_PTR id, DWORD_PTR) {
    // Qt handles WM_MEASUREITEM before the nativeEvent hook and supplies its
    // own item padding. Measure our headings before forwarding to Qt so USER32
    // can add its native margins exactly once, in both captioned and fullscreen
    // windows. Unrelated menu/control measurements continue through Qt.
    if (msg == WM_MEASUREITEM) {
        auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (item && item->CtlType == ODT_MENU && measureHeading(window, GetMenu(window), *item))
            return TRUE;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(window, finishMenuPaint, id);
    const auto result = DefSubclassProc(window, msg, wParam, lParam);
    if (msg == WM_NCDESTROY) {
        WindowsMenuTheme::resetMenuPaintResources(window);
        return result;
    }
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
            if (saved) fill(dc, band, menuPaintResources(window).background);
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
    return saved && fill(menu->dc, rectangle, menuPaintResources(message.hwnd).background);
}

struct MenuLabel {
    MENUITEMINFOW info{};
    std::wstring text;

    bool read(HMENU menu, UINT position) {
        info.cbSize = sizeof(info);
        info.fMask = MIIM_STRING | MIIM_FTYPE | MIIM_STATE | MIIM_BITMAP | MIIM_SUBMENU | MIIM_DATA;
        if (!GetMenuItemInfoW(menu, position, TRUE, &info) || !info.hSubMenu || info.hbmpItem ||
            (info.fType & (MFT_BITMAP | MFT_SEPARATOR)) || info.cch > 32767)
            return false;
        text.resize(info.cch + 1);
        info.dwTypeData = text.data();
        info.cch = static_cast<UINT>(text.size());
        if (!GetMenuItemInfoW(menu, position, TRUE, &info)) return false;
        text.resize(info.cch);
        return true;
    }
    bool custom() const {
        return (info.fType & MFT_OWNERDRAW) && info.dwItemData == reinterpret_cast<ULONG_PTR>(info.hSubMenu);
    }
};

int headingPosition(HMENU bar, ULONG_PTR data) {
    if (!data) return -1;
    const int count = GetMenuItemCount(bar);
    for (int i = 0; i < count; ++i)
        if (reinterpret_cast<ULONG_PTR>(GetSubMenu(bar, i)) == data) return i;
    return -1;
}

bool paintHeading(HWND window, HDC dc, HMENU menu, UINT position, RECT itemRect, UINT state) {
    MenuLabel label;
    if (!dc || !label.read(menu, position) ||
        ((label.info.fType & MFT_OWNERDRAW) && !label.custom())) return false;
    const UINT itemState = label.info.fState;
    const bool disabled = (state & (ODS_DISABLED | ODS_GRAYED))
            || (itemState & MFS_DISABLED);
    bool selected = (state & (ODS_SELECTED | ODS_FOCUS)) != 0;
    bool hot = (state & ODS_HOTLIGHT) != 0;
    if (label.custom() && (hot || selected || (itemState & MFS_HILITE))) {
        // The classic bar also marks a hovered heading as selected. Only use
        // the pressed color when Windows is actually tracking this window's menu.
        GUITHREADINFO gui{};
        gui.cbSize = sizeof(gui);
        if (GetGUIThreadInfo(GetCurrentThreadId(), &gui)) {
            selected = (gui.flags & GUI_INMENUMODE) && gui.hwndMenuOwner == window;
            hot = !selected;
        }
    }
    auto& resources = menuPaintResources(window);
    const MenuFont font(window, (state & ODS_DEFAULT) || (itemState & MFS_DEFAULT));
    if (!font.get()) return false;
    // Every color, font and clip on the caller-owned DC is restored.
    const SavedDc saved(dc);
    if (!saved) return false;
    if (IntersectClipRect(dc, itemRect.left, itemRect.top, itemRect.right, itemRect.bottom) == ERROR)
        return false;
    const int themeState = disabled
            ? (selected ? MBI_DISABLEDPUSHED : hot ? MBI_DISABLEDHOT : MBI_DISABLED)
            : (selected ? MBI_PUSHED : hot ? MBI_HOT : MBI_NORMAL);
    const auto& visual = resources.visuals[themeState - 1];
    // Paint translucent theme parts once over the menu base.
    if (!fill(dc, itemRect, visual.theme ? resources.background : visual.background)) return false;
    if (visual.theme)
        DrawThemeBackground(visual.theme, dc, visual.part, visual.state, &itemRect, nullptr);
    if (selected && resources.frameSelected) {
        SetDCBrushColor(dc, disabled ? visual.text : resources.selectedBorder);
        FrameRect(dc, &itemRect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    if (!SelectObject(dc, font.get())) return false;
    SetBkMode(dc, TRANSPARENT);
    const bool inactive = (state & ODS_INACTIVE) && !selected && !hot && !disabled;
    const COLORREF text = inactive ? resources.inactiveText : visual.text;
    SetTextColor(dc, text);
    UINT format = DT_CENTER | DT_VCENTER | DT_SINGLELINE;
    if (state & ODS_NOACCEL) format |= DT_HIDEPREFIX;
    const auto exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((label.info.fType & MFT_RIGHTORDER) || (exStyle & (WS_EX_LAYOUTRTL | WS_EX_RTLREADING)))
        format |= DT_RTLREADING;
    if (label.text.empty()) return true;
    if (visual.theme) {
        DTTOPTS options{};
        options.dwSize = sizeof(options);
        options.dwFlags = DTT_TEXTCOLOR;
        options.crText = text;
        if (SUCCEEDED(DrawThemeTextEx(visual.theme, dc, visual.part, visual.state,
                label.text.data(), static_cast<int>(label.text.size()), format, &itemRect, &options)))
            return true;
    }
    return DrawTextW(dc, label.text.data(), static_cast<int>(label.text.size()), &itemRect, format) != 0;
}

bool measureHeading(HWND window, HMENU bar, MEASUREITEMSTRUCT& item) {
    const int pos = headingPosition(bar, item.itemData);
    MenuLabel label;
    if (pos < 0 || !label.read(bar, pos) || !label.custom()) return false;
    const MenuFont font(window, label.info.fState & MFS_DEFAULT);
    if (!font.get()) return false;
    HDC dc = GetDC(window);
    if (!dc) return false;
    RECT size{};
    {
        const SavedDc saved(dc);
        if (saved && SelectObject(dc, font.get()))
            DrawTextW(dc, label.text.data(), static_cast<int>(label.text.size()), &size,
                      DT_SINGLELINE | DT_CALCRECT);
    }
    ReleaseDC(window, dc);
    if (!size.bottom) return false;
    // Return only the text extent. USER32 supplies the same horizontal menu
    // margins as a native string heading (two average character widths).
    // Removing them here clips long labels; adding them duplicates the padding.
    item.itemWidth = std::max(1L, size.right - size.left);
    item.itemHeight = size.bottom - size.top;
    return true;
}

// USER32 caches an owner-drawn item's dimensions even after SetMenuItemInfo,
// DrawMenuBar, a frame change, or detaching/reattaching the same HMENU. Replace
// only the heading record when its font/DPI changes; submenu handles, command
// IDs, strings, state and accessibility data remain part of the native menu.
bool resetHeadingSize(HMENU menu, UINT position) {
    MENUITEMINFOW item{};
    item.cbSize = sizeof(item);
    item.fMask = MIIM_STRING | MIIM_FTYPE | MIIM_STATE | MIIM_BITMAP
            | MIIM_SUBMENU | MIIM_DATA | MIIM_ID | MIIM_CHECKMARKS;
    if (!GetMenuItemInfoW(menu, position, TRUE, &item) || item.cch > 32767) return false;
    std::wstring text(item.cch + 1, L'\0');
    item.dwTypeData = text.data();
    item.cch = static_cast<UINT>(text.size());
    if (!GetMenuItemInfoW(menu, position, TRUE, &item)) return false;
    // Insert first so an allocation failure leaves the original item intact.
    // RemoveMenu keeps the shared submenu alive; DeleteMenu would destroy it.
    if (!InsertMenuItemW(menu, position, TRUE, &item)) return false;
    if (RemoveMenu(menu, position + 1, MF_BYPOSITION)) return true;
    RemoveMenu(menu, position, MF_BYPOSITION);
    return false;
}

// Preserve access keys for owner-drawn headings, including cycling duplicate mnemonics.
bool headingMnemonic(HMENU bar, wchar_t key, qintptr* result) {
    int matches = 0, first = -1, next = -1, highlighted = -1;
    const int count = GetMenuItemCount(bar);
    for (int i = 0; i < count; ++i) {
        MenuLabel label;
        if (!label.read(bar, i) || !label.custom() || (label.info.fState & MFS_DISABLED)) continue;
        if (label.info.fState & MFS_HILITE) highlighted = i;
        for (size_t n = 0; n + 1 < label.text.size(); ++n) {
            if (label.text[n] != L'&') continue;
            const auto& mnemonic = label.text[++n];
            if (mnemonic == L'&') continue;
            if (CompareStringOrdinal(&key, 1, &mnemonic, 1, TRUE) == CSTR_EQUAL) {
                if (matches++ == 0) first = i;
                if (highlighted >= 0 && next < 0 && i > highlighted) next = i;
            }
            break;
        }
    }
    if (!matches) return false;
    *result = MAKELRESULT(next >= 0 ? next : first, matches == 1 ? MNC_EXECUTE : MNC_SELECT);
    return true;
}

} // namespace

bool WindowsMenuTheme::drawMenuBar(const MSG& message, qintptr* result) {
    bool handled = false;
    switch (message.message) {
    case uahDrawMenu: handled = drawBackground(message); break;
    case uahDrawMenuItem: {
        const auto* drawing = reinterpret_cast<const UahDrawMenuItem*>(message.lParam);
        if (drawing && drawing->position >= 0 && drawing->menu.menu == GetMenu(message.hwnd))
            handled = paintHeading(message.hwnd, drawing->menu.dc, drawing->menu.menu,
                                   drawing->position, drawing->item.rcItem, drawing->item.itemState);
        break;
    }
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(message.lParam);
        const HMENU bar = GetMenu(message.hwnd);
        if (!item || item->CtlType != ODT_MENU || reinterpret_cast<HMENU>(item->hwndItem) != bar) return false;
        const int pos = headingPosition(bar, item->itemData);
        if (pos < 0 || !paintHeading(message.hwnd, item->hDC, bar, pos, item->rcItem, item->itemState))
            return false;
        *result = TRUE;
        return true;
    }
    case WM_MEASUREITEM: {
        auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(message.lParam);
        if (!item || item->CtlType != ODT_MENU || !measureHeading(message.hwnd, GetMenu(message.hwnd), *item))
            return false;
        *result = TRUE;
        return true;
    }
    case WM_MENUCHAR: {
        const auto bar = reinterpret_cast<HMENU>(message.lParam);
        return bar && bar == GetMenu(message.hwnd) && headingMnemonic(bar, LOWORD(message.wParam), result);
    }
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

void WindowsMenuTheme::refreshMenuBar(HWND hwnd, bool dark, bool remeasure) {
    const HMENU menu = GetMenu(hwnd);
    if (!menu) return;
    // The same headings receive the same measure and paint messages in both modes.
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW item{};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, i, TRUE, &item) || !item.hSubMenu ||
            (item.fType & (MFT_BITMAP | MFT_SEPARATOR))) continue;
        const auto tag = reinterpret_cast<ULONG_PTR>(item.hSubMenu);
        if ((item.fType & MFT_OWNERDRAW) ? item.dwItemData != tag : item.dwItemData != 0) continue;
        const bool newHeading = !(item.fType & MFT_OWNERDRAW);
        if (!newHeading && !remeasure) continue;
        item.fType |= MFT_OWNERDRAW;
        item.dwItemData = tag;
        item.fMask = MIIM_FTYPE | MIIM_DATA;
        if (SetMenuItemInfoW(menu, i, TRUE, &item))
            resetHeadingSize(menu, static_cast<UINT>(i));
    }
    // Share the exact themed base with the unused strip and nonclient edge.
    // The cache keeps a detached fullscreen menu's brush alive until replaced.
    auto& resources = menuPaintResources(hwnd);
    if (resources.dark != (dark && !resources.highContrast)) resources.refresh(hwnd, dark);
    resources.applyBrush(menu);
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

void WindowsMenuTheme::refreshMenuPaintResources(HWND hwnd, bool dark) {
    if (hwnd) menuPaintResources(nullptr).refresh(hwnd, dark);
}

void WindowsMenuTheme::resetMenuPaintResources(HWND hwnd) {
    auto& resources = menuPaintResources(nullptr);
    if (!hwnd || resources.window == hwnd) resources.reset();
}

void WindowsMenuTheme::applyPopupTheme(HWND hwnd, bool dark) {
    // This also intercepts native heading measurements before Qt in both modes.
    SetWindowSubclass(hwnd, finishMenuPaint, 1, 0);
    const auto& api = popupApi();
    if (!api.allowWindow || (!api.setMode && !api.setLegacyMode)) return;
    if (api.setMode) api.setMode(dark ? PopupThemeApi::AppMode::AllowDark : PopupThemeApi::AppMode::Default);
    else api.setLegacyMode(dark);
    api.allowWindow(hwnd, dark);
    if (api.flush) api.flush();
}

#endif
