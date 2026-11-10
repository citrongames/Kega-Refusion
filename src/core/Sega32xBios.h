#pragma once
#include <QString>
#include <QStringList>

// Resolves optional original 32X firmware. The core supplies missing boot programs.
class Sega32xBios final {
public:
    enum class Piece { M68k, MasterSh2, SlaveSh2 };
    static QString fileName(Piece piece);
    // Returns the actual installed image path, or an empty string with a diagnostic.
    static QString resolve(const QString& dataPath, Piece piece, QString& error);
    static bool validateFile(const QString& path, Piece piece, QString& error);
    static bool validate(const QString& dataPath, QString& error);
    static QString status(const QString& dataPath);
    // Recognizes by content, imports into dataPath/bios, never replaces a file.
    static QStringList importDirectory(const QString& sourceDirectory, const QString& dataPath,
                                       QString& error);
};
