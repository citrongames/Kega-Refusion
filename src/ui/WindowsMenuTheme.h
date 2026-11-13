#pragma once

#include <QtGlobal>

#ifdef Q_OS_WIN
#include <qt_windows.h>

namespace WindowsMenuTheme {

bool systemPrefersDark();
void refreshMenuPaintResources(HWND hwnd, bool dark);
void resetMenuPaintResources(HWND hwnd);
void refreshMenuBar(HWND hwnd, bool dark, bool remeasure = false);
void applyPopupTheme(HWND hwnd, bool dark);

// Handles painting, measuring and access keys for the native menu bar.
// Does not replace the window/menu, change menu ownership, or paint the client.
bool drawMenuBar(const MSG& message, qintptr* result);

} // namespace WindowsMenuTheme
#endif
