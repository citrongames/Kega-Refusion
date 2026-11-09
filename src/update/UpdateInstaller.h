#pragma once
#include <QString>

namespace UpdateInstaller {
// Run off the GUI thread. The archive remains owned by the caller and can be
// removed after return. Success transfers ownership of the isolated stage and
// detached helper; the caller may then close the application normally.
bool prepareAndLaunch(const QString& archivePath, QString& error);
// A previous detached installation error, if any. Reading removes the report.
QString takeFailureReport();
}
