#include "LinuxDesktopEntry.h"

#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace {
QString desktopValue(QString value) {
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    value.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    value.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
    return value;
}

QString desktopExecArgument(QString value) {
    // Exec quoting is parsed after desktop-entry string escaping. Percent signs
    // are field codes even inside quoted arguments, so literal '%' needs '%%'.
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    value.replace(QLatin1Char('`'), QStringLiteral("\\`"));
    value.replace(QLatin1Char('$'), QStringLiteral("\\$"));
    value.replace(QLatin1Char('%'), QStringLiteral("%%"));
    return desktopValue(QLatin1Char('"') + value + QLatin1Char('"'));
}

QString desktopField(const QByteArray& contents, const QString& key) {
    bool desktopSection = false;
    for (const auto& raw : QString::fromUtf8(contents).split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            desktopSection = line == QStringLiteral("[Desktop Entry]");
        } else if (desktopSection && line.startsWith(key + QLatin1Char('='))) {
            return line.mid(key.size() + 1);
        }
    }
    return {};
}

bool writeIfChanged(const QString& path, const QByteArray& contents) {
    const QFileInfo info(path);
    if (info.isSymLink() || (info.exists() && !info.isFile())) return false;
    QFile previous(path);
    if (previous.open(QIODevice::ReadOnly) && previous.readAll() == contents) return true;
    if (!QDir().mkpath(info.absolutePath())) return false;
    QSaveFile output(path);
    return output.open(QIODevice::WriteOnly) && output.write(contents) == contents.size()
        && output.commit();
}
}

void installLinuxDesktopEntry(const QString& appDirectory) {
    const QString appId = QGuiApplication::desktopFileName();
    static const QRegularExpression validId(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]*$"));
    if (!validId.match(appId).hasMatch()) return;
    const QString dataHome = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (!QDir::isAbsolutePath(dataHome)) return;
    const QDir appDir(appDirectory);
    QString launcher = appDir.absoluteFilePath(QStringLiteral("run.sh"));
    if (!QFileInfo(launcher).isExecutable())
        launcher = appDir.absoluteFilePath(QStringLiteral("KegaRefusion"));
    if (!QFileInfo(launcher).isFile() || !QFileInfo(launcher).isExecutable()) return;

    QString exec = desktopExecArgument(launcher);
    if (launcher.contains(QLatin1Char('%'))) {
        // GLib checks the executable before expanding %% field codes. Put the
        // portable path in an argument so installations containing '%' launch.
        const QString env = QStandardPaths::findExecutable(QStringLiteral("env"));
        if (env.isEmpty()) return;
        exec = desktopExecArgument(env) + QLatin1Char(' ') + exec;
    }

    const QString entryPath = QDir(dataHome).filePath(QStringLiteral("applications/") + appId + QStringLiteral(".desktop"));
    QFile previous(entryPath);
    const QFileInfo entryInfo(entryPath);
    if (entryInfo.isSymLink() || (entryInfo.exists() && !entryInfo.isFile())) return;
    QByteArray oldEntry;
    if (entryInfo.exists()) {
        if (!previous.open(QIODevice::ReadOnly)) return;
        oldEntry = previous.readAll();
        // A distro installation or a user-owned launcher with this ID wins.
        if (desktopField(oldEntry, QStringLiteral("X-KegaRefusion-Portable")) != QStringLiteral("true")) return;
    }

    const QString resourcePath = QStringLiteral(":/icons/app.png");
    const QFileInfo resourceInfo(resourcePath);
    const QString iconRevision = QString::number(resourceInfo.lastModified().toMSecsSinceEpoch())
        + QLatin1Char('-') + QString::number(resourceInfo.size());
    // Absolute Icon avoids depending on theme caches. The portable suffix keeps
    // this file separate from any installed distribution's themed app icon.
    const QString iconPath = QDir(dataHome).filePath(QStringLiteral("icons/") + appId + QStringLiteral("-portable.png"));
    const QFileInfo installedIcon(iconPath);
    const bool currentIcon = installedIcon.isFile() && !installedIcon.isSymLink()
        && installedIcon.size() == resourceInfo.size()
        && desktopField(oldEntry, QStringLiteral("X-KegaRefusion-IconRevision")) == iconRevision;
    if (!currentIcon) {
        QFile resource(resourcePath);
        if (!resource.open(QIODevice::ReadOnly)) return;
        const QByteArray icon = resource.readAll();
        if (icon.isEmpty() || !writeIfChanged(iconPath, icon)) return;
    }

    // Keep an explicit user decision to expose our entry in the application menu.
    const bool visible = desktopField(oldEntry, QStringLiteral("NoDisplay")) == QStringLiteral("false");
    const QString entry = QStringLiteral("[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Kega Refusion\n"
        "Exec=%1\n"
        "Icon=%2\n"
        "Terminal=false\n"
        "Categories=Game;Emulator;\n"
        "NoDisplay=%3\n"
        "X-KegaRefusion-Portable=true\n"
        "X-KegaRefusion-IconRevision=%4\n")
        .arg(exec, desktopValue(iconPath),
             visible ? QStringLiteral("false") : QStringLiteral("true"), iconRevision);
    // KWin looks up the matching desktop file directly before selecting its
    // fallback Wayland icon; no menu database rebuild or per-start process.
    writeIfChanged(entryPath, entry.toUtf8());
}
