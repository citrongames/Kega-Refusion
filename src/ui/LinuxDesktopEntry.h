#pragma once

#include <QString>

#ifdef Q_OS_LINUX
// Call after desktopFileName is set and before creating the first window.
void installLinuxDesktopEntry(const QString& appDirectory);
#endif
