#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>

// Only hash-verified Sega LaserActive PAC-S firmware is accepted. Ordinary
// Sega CD firmware and NEC PAC firmware are separate, incompatible roles.
class SegaLdBios final {
public:
    static QString resolve(const QString& dataPath, int region, QString& error);
    static QString resolveInDirectory(const QString& biosDirectory, int region, QString& error);
    static QString status(const QString& dataPath, int region);
    static QStringList importDirectory(const QString& sourceDirectory, const QString& dataPath, QString& error);
    static bool isPacImage(const QByteArray& bytes);
};
