#pragma once
#include <QString>

// Materialize the embedded release files without replacing personal edits.
bool installBundledLuaFiles(const QString& applicationDirectory,
                            const QString& scriptsDirectory,
                            const QString& dataDirectory,
                            const QString& sourceDirectory = QStringLiteral(":/bundled-lua"));
