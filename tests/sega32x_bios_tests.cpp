#include "core/Sega32xBios.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool ok,const char* text) { if (!ok) throw std::runtime_error(text); }
using Piece=Sega32xBios::Piece;
constexpr std::array<Piece,3> pieces{{Piece::M68k,Piece::MasterSh2,Piece::SlaveSh2}};
constexpr std::array<int,3> sizes{{256,2048,1024}};
QByteArray read(const QString& path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly),"cannot read test file"); return file.readAll();
}
void write(const QString& path,const QByteArray& bytes) {
    QFile file(path); require(file.open(QIODevice::WriteOnly|QIODevice::NewOnly) && file.write(bytes)==bytes.size(),"cannot create test file");
}
void invalid_images() {
    QTemporaryDir temp; require(temp.isValid(),"cannot create temporary directory");
    QString error;
    require(!Sega32xBios::validate(temp.path(),error),"missing BIOS set accepted");
    for (const auto piece:pieces) require(error.contains(Sega32xBios::fileName(piece)),"missing-piece diagnostic incomplete");
    const auto source=temp.filePath("source"), target=temp.filePath("target");
    require(QDir().mkpath(source),"cannot make test source");
    for (std::size_t i=0;i<pieces.size();++i) {
        const auto path=QDir(source).filePath(Sega32xBios::fileName(pieces[i]));
        write(path,QByteArray(sizes[i],char(0x55)));
        require(!Sega32xBios::validateFile(path,pieces[i],error) && !error.isEmpty(),"same-size garbage BIOS accepted");
    }
    require(Sega32xBios::importDirectory(source,target,error).isEmpty() && !error.isEmpty(),"garbage images imported");
    require(!QDir(target).exists(),"failed recognition created destination directories");
    require(Sega32xBios::fileName(static_cast<Piece>(99)).isEmpty(),"unknown piece filename accepted");
}
void read_only_originals(const QString& source) {
    QTemporaryDir temp; require(temp.isValid(),"cannot create preserve-files test directory");
    const auto bios=temp.filePath("bios"); require(QDir().mkpath(bios),"cannot make preserve-files directory");
    QString error;
    for (std::size_t i=0;i<pieces.size();++i) {
        const auto name=Sega32xBios::fileName(pieces[i]);
        const bool valid=Sega32xBios::validateFile(QDir(source).filePath(name),pieces[i],error);
        require(valid,qPrintable(error));
        require(!Sega32xBios::validateFile(QDir(source).filePath(name),pieces[(i+1)%3],error),"wrong BIOS role accepted");
        write(QDir(bios).filePath(name),QByteArray(sizes[i],char(0x55)));
    }
    // Every destination already exists: real source BIOS is only read/hashed,
    // never copied, even temporarily. Exercise recognition and no-overwrite.
    require(Sega32xBios::importDirectory(source,temp.path(),error).isEmpty(),"existing BIOS target replaced");
    require(error.contains(QStringLiteral("без замены")),"preservation diagnostic missing");
    for (std::size_t i=0;i<pieces.size();++i)
        require(read(QDir(bios).filePath(Sega32xBios::fileName(pieces[i])))==QByteArray(sizes[i],char(0x55)),"user destination contents changed");
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        invalid_images();
        if (argc>1) read_only_originals(QString::fromLocal8Bit(argv[1]));
        std::puts("32X BIOS tests passed: content validation, missing pieces, no overwrite");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"32X BIOS tests failed: %s\n",error.what()); return 1;
    }
}
