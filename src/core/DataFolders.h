#pragma once

#include <QMap>
#include <QStringList>

namespace DataFolders {
QStringList categories();
QString defaultPath(const QString& dataRoot, const QString& category);
QString configuredPath(const QString& dataRoot, const QString& category);
QString path(const QString& dataRoot, const QString& category);
QMap<QString, QString> paths(const QString& dataRoot);
bool save(const QString& dataRoot, const QMap<QString, QString>& changes,
          QString& error, QString* failedCategory = nullptr);
}
