#include "core/CartridgeProfile.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
QByteArray fixture() {
    QByteArray bytes(64 * 1024, '\0');
    bytes.replace(0x100,32,"SEGA GENESIS (C) 2004 FVRING    ");
    return bytes;
}
void setters(QByteArray& bytes) {
    // Original generated 68000 fixture, with a shared RAM flag and complete
    // Firecore VDP register-$18 setters. No game code/data is included.
    const auto code = QByteArray::fromHex(
        "41f900ff812310100000ff801080024098ff0040980033c000c000044e75"
        "41f900ff812310100200007f10800240987f0040980033c000c000044e75");
    require(code.size() == 60, "fixture length");
    bytes.replace(0x33b2,code.size(),code);
}
bool detects(const char* ext,const QByteArray& bytes) {
    return CartridgeProfile::firecore(QString::fromLatin1(ext),QByteArrayView(bytes));
}
}

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        const auto ordinary=fixture();
        require(!detects("bin",ordinary),"shared Sega header must remain MD");
        require(detects("atf",ordinary) && detects("ATF",ordinary),"explicit ATF profile");
        require(!detects("cue",ordinary) && !detects("smd",ordinary),"disc/legacy formats not sniffed");
        for (const auto* family : {"DMC RedKid      ","DMC  EMDGAME (C)"}) {
            auto rom=ordinary;rom.replace(0x100,16,QByteArray(family));
            require(detects("bin",rom) && detects("BIN",rom),"exact DMC BIN family");
            require(!detects("md",rom),"only BIN auto-detected");
            rom[0x100]='S';
            require(!detects("bin",rom),"near-match header rejected");
        }
        auto niutai=ordinary;setters(niutai);
        const auto hash=QCryptographicHash::hash(niutai,QCryptographicHash::Sha256);
        require(detects("bin",niutai),"paired executable setters");
        require(hash==QCryptographicHash::hash(niutai,QCryptographicHash::Sha256),"original bytes unchanged");
        require(!detects("bin",niutai.left(0x33ed)),"truncated setter rejected");
        for (int at=0x33b2;at<0x33ee;++at) {
            auto changed=niutai;changed[at]=char(changed[at]^1);
            require(!detects("bin",changed),"incomplete/altered instruction signature rejected");
        }
        auto relocated=ordinary;relocated.replace(0x8000,60,niutai.mid(0x33b2,60));
        require(!detects("bin",relocated),"code bytes in unrelated data rejected");
        auto constants=ordinary;constants.replace(0x33c0,4,QByteArray::fromHex("98009801"));
        require(!detects("bin",constants),"isolated register values rejected");
        require(!detects("bin",QByteArray(511,'\0')),"short BIN rejected");
        if (argc>1) {
            unsigned total=0,firecore=0;
            QDirIterator files(QString::fromLocal8Bit(argv[1]),QDir::Files,QDirIterator::Subdirectories);
            while(files.hasNext()) {
                const auto path=files.next();
                if(QFileInfo(path).suffix().compare("bin",Qt::CaseInsensitive))continue;
                QFile f(path);require(f.open(QIODevice::ReadOnly),"corpus read");
                ++total;firecore+=detects("bin",f.readAll());
            }
            std::printf("corpus BIN profile: %u Firecore, %u ordinary, %u total\n",firecore,total-firecore,total);
            require(total==101 && firecore==63,"corpus profile coverage changed");
        }
        if (argc>2) {
            unsigned count=0;
            QDirIterator files(QString::fromLocal8Bit(argv[2]),QDir::Files,QDirIterator::Subdirectories);
            while(files.hasNext()) {
                const auto path=files.next();
                const auto suffix=QFileInfo(path).suffix().toLower();
                if(!QStringList{"md","gen","bin","smd","32x","68k"}.contains(suffix))continue;
                QFile f(path);require(f.open(QIODevice::ReadOnly),"ordinary collection read");
                const auto bytes=f.read(0x33ee);++count;
                if(detects("bin",bytes)) {
                    std::fprintf(stderr,"Unexpected Firecore signature: %s\n",qPrintable(path));
                    throw std::runtime_error("ordinary collection false positive");
                }
            }
            require(count>0,"ordinary collection empty");
            std::printf("ordinary MD signature comparison: %u files, zero false positives\n",count);
        }
        std::puts("Firecore profile routing passed");
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what());return 1; }
}
