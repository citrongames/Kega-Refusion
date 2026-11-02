#pragma once

#include <QList>
#include <QString>

namespace Localization {

struct LanguageInfo {
    QString code;
    QString name;
};

// Load catalogs once during startup, before worker threads and UI are created.
// ui/language in settings.ini may be "system" or an installed language code.
void initialize(const QString& applicationDirectory, const QString& dataDirectory);
QString text(const QString& source);
QList<LanguageInfo> availableLanguages();
QString currentLanguage();
QString selectedLanguage();

}
