#include "core/SegaCdBios.h"
#include "core/Sega32xBios.h"
#include "core/DataFolders.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QSettings>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
void require(bool ok,const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void write(const QString& path,const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly|QIODevice::Truncate),"cannot open test image");
    require(file.write(bytes)==bytes.size(),"cannot write test image");
}
void update(const QString& path,const QByteArray& bytes) {
    write(path,bytes);
    QFile file(path);
    require(file.open(QIODevice::ReadWrite),"cannot reopen test image");
    static int revision=0;
    require(file.setFileTime(QDateTime::currentDateTimeUtc().addSecs(++revision*5),QFileDevice::FileModificationTime),
            "cannot update test image timestamp");
}
QByteArray cdImage() {
    QByteArray bytes(128*1024,char(0));
    bytes[0]=char(0xff);
    bytes[1]=char(0xff);
    bytes[2]=char(0xfd);
    bytes[3]=char(0x00);
    bytes[4]=char(0x00);
    bytes[5]=char(0x00);
    bytes[6]=char(0x02);
    bytes[7]=char(0x00);
    std::memcpy(bytes.data()+0x100,"SEGA MEGA DRIVE ",16);
    std::memcpy(bytes.data()+0x110,"(C)SEGA",7);
    std::memcpy(bytes.data()+0x120,"SEGA-CD BOOT ROM",16);
    std::memcpy(bytes.data()+0x150,"SEGA-CD BOOT ROM",16);
    std::memcpy(bytes.data()+0x180,"BR ",3);
    bytes[0x1f0]='U';
    // The zero-filled body has a valid checksum of zero in the header.
    return bytes;
}
void portableFolders() {
    QTemporaryDir original, moved;
    require(original.isValid() && moved.isValid(), "cannot create portable data roots");
    QString error;
    const QString category = QStringLiteral("screenshots");
    const QString nested = QStringLiteral("captures/скриншоты");
    require(DataFolders::configuredPath(original.path(), category) == category,
            "default folder should be stored relative to settings");
    require(DataFolders::save(original.path(), {{category, nested}}, error), qPrintable(error));
    require(QFileInfo(original.filePath(nested)).isDir(), "nested destination was not created");
    require(QFile::copy(original.filePath("settings.ini"), moved.filePath("settings.ini")),
            "cannot copy portable settings");
    {
        QSettings settings(moved.filePath("settings.ini"), QSettings::IniFormat);
#ifdef Q_OS_WIN
        settings.setValue("folders/linux/screenshots", "/mnt/other/captures");
#else
        settings.setValue("folders/windows/screenshots", "D:/other/captures");
#endif
    }
    require(DataFolders::path(moved.path(), category) == moved.filePath(nested),
            "relative destination did not move with data");
    require(DataFolders::path(moved.path(), "bios") == moved.filePath("bios"),
            "default BIOS directory did not move with data");
    const QString shaders = QStringLiteral("shaders");
    const QString bundled = QDir(QCoreApplication::applicationDirPath()).filePath(shaders);
    require(DataFolders::path(moved.path(), shaders) == bundled, "bundled shader directory lost");
    require(DataFolders::save(moved.path(), {{shaders, "graphics/presets"}}, error), qPrintable(error));
    require(QDir(moved.filePath("graphics/presets")).exists() &&
            DataFolders::path(moved.path(), shaders) == moved.filePath("graphics/presets"),
            "custom shader directory was not created or applied");
    const QString fixed = original.filePath("external");
    require(DataFolders::save(moved.path(), {{category, fixed}}, error), qPrintable(error));
    require(DataFolders::path(moved.path(), category) == fixed, "absolute destination moved");
    QString failed;
    require(!DataFolders::save(moved.path(), {{category, "settings.ini/child"}}, error, &failed)
            && failed == category && DataFolders::path(moved.path(), category) == fixed,
            "invalid destination replaced the previous setting");
    require(DataFolders::save(moved.path(), {{category, category}}, error), qPrintable(error));
    require(DataFolders::path(moved.path(), category) == moved.filePath(category),
            "reset did not restore portable default");
}

void cdResolution() {
    QTemporaryDir temp;
    require(temp.isValid(),"cannot create CD test directory");
    const QString bios=temp.filePath("bios");
    require(QDir().mkpath(bios),"cannot create CD BIOS directory");
    const QByteArray valid=cdImage();
    const QString renamed=QDir(bios).filePath("a-usa.image");
    const QString next=QDir(bios).filePath("b-usa.rom");
    const QString canonical=QDir(bios).filePath("bios_CD_U.bin");
    write(renamed,valid);
    write(next,valid);
    write(canonical,QByteArray(valid.size(),char(0x55)));
    QString error;
    require(SegaCdBios::resolve(temp.path(),2,error)==renamed,"renamed CD BIOS did not outrank bad canonical");
    require(SegaCdBios::validate(temp.path(),2,error),"renamed CD BIOS was not validated");
    require(SegaCdBios::status(temp.path(),2).contains(renamed),"CD status omits actual path");
    require(SegaCdBios::resolve(temp.path(),1,error).isEmpty(),"wrong CD region accepted");
    update(canonical,valid);
    require(SegaCdBios::resolve(temp.path(),2,error)==canonical,"valid canonical CD BIOS not preferred");
    update(canonical,QByteArray(valid.size(),char(0x55)));
    require(SegaCdBios::resolve(temp.path(),2,error)==renamed,"changed canonical CD BIOS shadowed renamed image");
    update(renamed,QByteArray(valid.size(),char(0x55)));
    require(SegaCdBios::resolve(temp.path(),2,error)==next,"changed CD image remained cached");
    require(QFile::remove(next),"cannot delete CD image");
    require(SegaCdBios::resolve(temp.path(),2,error).isEmpty(),"deleted CD image remained cached");
    require(!error.isEmpty(),"missing CD image has no diagnostic");

    QTemporaryDir imported;
    require(imported.isValid(),"cannot create CD import directory");
    const QString importedBios=imported.filePath("bios");
    require(QDir().mkpath(importedBios),"cannot create CD import BIOS directory");
    write(QDir(importedBios).filePath("renamed.any"),valid);
    QTemporaryDir source;
    require(source.isValid(),"cannot create CD source directory");
    write(source.filePath("source.bin"),valid);
    require(SegaCdBios::importDirectory(source.path(),imported.path(),error).isEmpty(),
            "CD import duplicated a valid renamed image");
    require(!QFile::exists(QDir(importedBios).filePath("bios_CD_U.bin")),
            "CD import created a redundant canonical image");
}
void configuredCdDirectory() {
    QTemporaryDir temp;
    require(temp.isValid(),"cannot create configured BIOS test directory");
    const QString data=temp.filePath("data"), selected=temp.filePath("selected BIOS");
    require(QDir().mkpath(data+"/bios") && QDir().mkpath(selected),"cannot create configured BIOS folders");
    write(data+"/bios/bios_CD_U.bin",cdImage());
    QString error;
    require(DataFolders::save(data,{{QStringLiteral("bios"),selected}},error),qPrintable(error));
    require(SegaCdBios::resolve(data,2,error).isEmpty(),"configured BIOS search fell back to default folder");
    const QString renamed=selected+"/renamed.rom";
    write(renamed,cdImage());
    require(SegaCdBios::resolve(data,2,error)==renamed,"configured renamed BIOS was not resolved");
    require(QFile::remove(renamed),"cannot remove configured test image");
    QTemporaryDir source;
    require(source.isValid(),"cannot create configured BIOS import source");
    write(source.filePath("original.rom"),cdImage());
    require(!SegaCdBios::importDirectory(source.path(),data,error).isEmpty(),qPrintable(error));
    require(SegaCdBios::resolve(data,2,error)==selected+"/bios_CD_U.bin","BIOS import ignored configured folder");
    require(QFile::remove(selected+"/bios_CD_U.bin"),"cannot remove imported test image");
    require(SegaCdBios::resolve(data,2,error).isEmpty(),"missing configured BIOS used default image");
}
void x32Resolution(const QString& source) {
    QTemporaryDir temp;
    require(temp.isValid(),"cannot create 32X test directory");
    const QString bios=temp.filePath("bios");
    require(QDir().mkpath(bios),"cannot create 32X BIOS directory");
    using Piece=Sega32xBios::Piece;
    constexpr std::array<Piece,3> pieces{{Piece::M68k,Piece::MasterSh2,Piece::SlaveSh2}};
    const std::array<int,3> sizes{{256,2048,1024}};
    std::array<QByteArray,3> original;
    std::array<QString,3> renamed;
    QString error;
    for (std::size_t i=0;i<pieces.size();++i) {
        QFile file(QDir(source).filePath(Sega32xBios::fileName(pieces[i])));
        require(file.open(QIODevice::ReadOnly),"cannot read original 32X BIOS");
        original[i]=file.readAll();
        require(Sega32xBios::validateFile(file.fileName(),pieces[i],error),
                "provided 32X BIOS failed recognition");
        renamed[i]=QDir(bios).filePath(QString::number(i)+".arbitrary");
        write(renamed[i],original[i]);
        write(QDir(bios).filePath(Sega32xBios::fileName(pieces[i])),
              QByteArray(sizes[i],char(0x55)));
        require(Sega32xBios::resolve(temp.path(),pieces[i],error)==renamed[i],
                "renamed 32X BIOS did not outrank bad canonical");
    }
    require(Sega32xBios::validate(temp.path(),error),"renamed 32X set was not validated");
    require(Sega32xBios::importDirectory(source,temp.path(),error).isEmpty(),
            "32X import duplicated a renamed image");
    const QString canonical=QDir(bios).filePath(Sega32xBios::fileName(pieces[0]));
    update(canonical,original[0]);
    require(Sega32xBios::resolve(temp.path(),pieces[0],error)==canonical,
            "valid canonical 32X BIOS not preferred");
    update(canonical,QByteArray(sizes[0],char(0x55)));
    require(Sega32xBios::resolve(temp.path(),pieces[0],error)==renamed[0],
            "changed canonical 32X BIOS shadowed renamed image");
    update(renamed[0],QByteArray(sizes[0],char(0x55)));
    require(Sega32xBios::resolve(temp.path(),pieces[0],error).isEmpty(),
            "changed 32X image remained cached");
    update(renamed[0],original[0]);
    require(Sega32xBios::resolve(temp.path(),pieces[0],error)==renamed[0],
            "restored 32X image was not recognized");
    require(QFile::remove(renamed[0]),"cannot delete 32X image");
    require(Sega32xBios::resolve(temp.path(),pieces[0],error).isEmpty(),
            "deleted 32X image remained cached");
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        portableFolders();
        cdResolution();
        configuredCdDirectory();
        if (argc>1) x32Resolution(QString::fromLocal8Bit(argv[1]));
        std::puts("BIOS resolution tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"BIOS resolution tests failed: %s\n",error.what());
        return 1;
    }
}
