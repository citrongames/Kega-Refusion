#include "core/SegaLdBios.h"
#include "core/SegaCdBios.h"
#include "core/DataFolders.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool value,const char* detail) { if (!value) throw std::runtime_error(detail); }
QByteArray read(const QString& path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly),"cannot read original firmware");
    require(file.size()==131072,"unexpected firmware size"); return file.read(131073);
}
void write(const QString& path,const QByteArray& bytes) {
    QFile file(path); require(file.open(QIODevice::WriteOnly|QIODevice::NewOnly),"cannot create firmware fixture");
    require(file.write(bytes)==bytes.size(),"cannot write firmware fixture");
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        if (argc!=2) {
            std::puts("usage: megald_bios_tests BIOS_DIRECTORY");
            return argc<2?77:2;
        }
        const auto source=QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
        require(QFileInfo(source).isDir(),"supplied original BIOS directory is missing");
        QTemporaryDir data; require(data.isValid(),"cannot create temporary data root");
        QString error;
        const auto imported=SegaLdBios::importDirectory(source,data.path(),error);
        require(error.isEmpty() && imported.size()==2,"expected two verified PAC BIOS imports");
        const auto jp=SegaLdBios::resolve(data.path(),1,error);
        const auto us=SegaLdBios::resolve(data.path(),2,error);
        require(!jp.isEmpty() && !us.isEmpty() && jp!=us,"PAC regions not resolved separately");
        require(SegaCdBios::resolve(data.path(),1,error).isEmpty() && SegaCdBios::resolve(data.path(),2,error).isEmpty(),
                "PAC firmware was accepted as ordinary Sega CD BIOS");
        require(SegaLdBios::isPacImage(read(jp)) && SegaLdBios::isPacImage(read(us)),"imported PAC fingerprints invalid");
        require(SegaLdBios::importDirectory(source,data.path(),error).isEmpty() && error.isEmpty(),"import was not idempotent");
        const auto renamed=data.filePath("bios/renamed-pac.rom");
        require(QFile::rename(jp,renamed),"cannot rename isolated PAC");
        write(jp,QByteArray(131072,char(0x55)));
        require(SegaLdBios::resolve(data.path(),1,error)==renamed,"bad canonical shadowed verified renamed PAC");
        require(SegaLdBios::importDirectory(source,data.path(),error).isEmpty(),"import duplicated renamed PAC");
        require(QFile::remove(renamed),"cannot remove isolated PAC");
        require(SegaLdBios::resolve(data.path(),1,error).isEmpty(),"deleted PAC remained cached");
        require(SegaLdBios::importDirectory(source,data.path(),error).isEmpty() && !error.isEmpty(),"import overwrote invalid existing firmware");
        require(read(jp)==QByteArray(131072,char(0x55)),"existing firmware modified");
        QTemporaryDir settings; require(settings.isValid(),"cannot create configured root");
        require(DataFolders::save(settings.path(),{{QStringLiteral("bios"),source}},error),"cannot configure original firmware folder");
        require(!SegaLdBios::resolve(settings.path(),1,error).isEmpty() && !SegaLdBios::resolve(settings.path(),2,error).isEmpty(),
                "configured original PAC directory not resolved");
        require(SegaLdBios::resolve(settings.path(),3,error).isEmpty(),"PAL PAC wrongly accepted");
        std::puts("MegaLD BIOS tests passed"); return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"MegaLD BIOS tests failed: %s\n",error.what()); return 1;
    }
}
