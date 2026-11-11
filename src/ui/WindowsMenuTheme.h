#pragma once

#include <QtGlobal>

#ifdef Q_OS_WIN
#include <qt_windows.h>

namespace WindowsMenuTheme {

bool systemPrefersDark();
void refreshMenuBar(HWND hwnd, bool dark);
void applyPopupTheme(HWND hwnd, bool dark);

// Call only for dark mode with high contrast disabled. Handles just the native
// menu-bar draw messages; all other messages must continue through Qt normally.
// Does not replace the window/menu, change menu ownership, or paint the client.
bool drawMenuBar(const MSG& message, qintptr* result);

} // namespace WindowsMenuTheme
#endif
