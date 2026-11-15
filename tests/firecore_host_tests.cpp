#include "core/LibretroHost.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtEndian>
#include <archive.h>
#include <archive_entry.h>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool ok,const char* why) { if(!ok)throw std::runtime_error(why); }
void save(const QString& path,const QByteArray& bytes) {
    QFile f(path);
    require(f.open(QIODevice::WriteOnly) && f.write(bytes)==bytes.size(),"write generated fixture");
}
QByteArray fixture() {
    QByteArray rom(64*1024,'\0');
    qToBigEndian<quint32>(0x00ff0000,rom.data());
    qToBigEndian<quint32>(0x00000200,rom.data()+4);
    rom.replace(0x100,32,"SEGA GENESIS (C) 2004 FVRING    ");
    rom.replace(0x120,20,"FIRECORE HOST CHECK ");
    rom.replace(0x180,2,"GM");
    qToBigEndian<quint32>(quint32(rom.size()-1),rom.data()+0x1a4);
    qToBigEndian<quint32>(0x00ff0000,rom.data()+0x1a8);
    qToBigEndian<quint32>(0x00ffffff,rom.data()+0x1ac);
    rom[0x1f0]='U';
    // Original 68000 display setup and endless branch. The unused complete
    // setters below represent a known family without embedding any game data.
    const auto program=QByteArray::fromHex("46fc270033fc800400c0000433fc814400c0000460fe");
    rom.replace(0x200,program.size(),program);
    const auto code=QByteArray::fromHex(
        "41f900ff812310100000ff801080024098ff0040980033c000c000044e75"
        "41f900ff812310100200007f10800240987f0040980033c000c000044e75");
    rom.replace(0x33b2,code.size(),code);
    return rom;
}
QByteArray pack(bool sevenZip,const char* name,const QByteArray& bytes) {
    QByteArray result(bytes.size()*2,'\0');size_t used=0;
    auto* a=archive_write_new();require(a,"archive writer");
    require((sevenZip?archive_write_set_format_7zip(a):archive_write_set_format_zip(a))==ARCHIVE_OK,"archive format");
    require(archive_write_open_memory(a,result.data(),size_t(result.size()),&used)==ARCHIVE_OK,"archive memory");
    auto* entry=archive_entry_new();
    archive_entry_set_pathname(entry,name);archive_entry_set_size(entry,bytes.size());
    archive_entry_set_filetype(entry,AE_IFREG);archive_entry_set_perm(entry,0644);
    require(archive_write_header(a,entry)==ARCHIVE_OK,"archive header");
    require(archive_write_data(a,bytes.constData(),size_t(bytes.size()))==bytes.size(),"archive data");
    archive_entry_free(entry);require(archive_write_close(a)==ARCHIVE_OK,"archive finish");archive_write_free(a);
    result.resize(qsizetype(used));return result;
}
}

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    if(argc!=2) {std::fprintf(stderr,"Usage: firecore_host_tests CORE\n");return 2;}
    try {
        QTemporaryDir temp;require(temp.isValid(),"fixture directory");
        const auto rom=fixture(),rawHash=QCryptographicHash::hash(rom,QCryptographicHash::Sha256);
        const auto bin=temp.filePath("original.bin"),atf=temp.filePath("renamed.ATF"),md=temp.filePath("ordinary.md");
        save(bin,rom);save(atf,rom);save(md,rom);
        LibretroHost host(QString::fromLocal8Bit(argv[1]),temp.filePath("data"));
        QString error;
        host.setExpansionDevices(ExpansionMode::Connected,ExpansionMode::Connected);
        require(host.setAutoFixChecksum(true,error),"enable checksum option");
        require(host.load(bin,error),qPrintable(error));
        require(host.isFirecore() && !host.isCd(),"BIN Firecore route");
        require(host.contentName()=="original.bin" && host.gamePath()==bin,"original source identity preserved");
        const auto config=host.movieConfiguration();
        const auto options=config.value("options").toObject();
        require(options.value("genesis_plus_gx_add_on")=="none","CD preference overridden");
        require(!options.contains("genesis_plus_gx_32x_add_on") ||
            options.value("genesis_plus_gx_32x_add_on")=="disconnected","32X preference overridden");
        require(config.value("cartridgeProfile")=="firecore","movie hardware profile");
        const auto game=host.movieGameIdentity(),network=host.networkCompatibility(),movieCore=host.movieCoreIdentity();
        host.runFrame();
        const auto movieState=host.captureMovieState(),networkState=host.captureNetworkState();
        require(movieState.mid(8,32)==rawHash,"state uses hash of original ROM bytes");
        require(!networkState.isEmpty(),"network state availability");
        require(host.hardReset(error),qPrintable(error));
        require(host.isFirecore() && host.movieGameIdentity()==game,"power reset retains selected profile");
        require(host.load(atf,error),qPrintable(error));
        require(host.isFirecore() && host.movieGameIdentity()==game &&
            host.movieCoreIdentity()==movieCore && host.networkCompatibility()==network,"rename preserves profile identity");
        require(host.prepareMovie(config,error),qPrintable(error));
        require(host.restoreMovieState(movieState,error),qPrintable(error));
        require(host.restoreMovieConfiguration(error),qPrintable(error));
        auto networkConfig=config;networkConfig.insert("networkCompatibility",QString::fromLatin1(network.toHex()));
        require(host.prepareNetwork(networkConfig,error),qPrintable(error));
        require(host.restoreNetworkState(networkState,error),qPrintable(error));
        require(host.restoreMovieConfiguration(error),qPrintable(error));
        const auto dmcKey = QStringLiteral("kegarefusion_firecore_dmc_pcm");
        host.setFirecoreDmcPcm(true);
        require(!host.movieConfiguration().value("options").toObject().contains(dmcKey),"pending PCM change leaves running game alone");
        auto enhanced = host.movieConfiguration(true);
        require(enhanced.value("options").toObject().value(dmcKey)=="enabled","next power records enhanced PCM");
        require(host.hardReset(error),qPrintable(error));
        require(host.movieConfiguration().value("options").toObject().contains(dmcKey),"power applies local PCM preference");
        require(host.prepareMovie(config,error),qPrintable(error));
        require(!host.movieConfiguration().value("options").toObject().contains(dmcKey),"old recording keeps original PCM");
        require(host.restoreMovieConfiguration(error),qPrintable(error));
        host.setFirecoreDmcPcm(false);
        require(host.hardReset(error),qPrintable(error));
        enhanced.insert("networkCompatibility",QString::fromLatin1(network.toHex()));
        require(host.prepareNetwork(enhanced,error),qPrintable(error));
        require(host.movieConfiguration().value("options").toObject().contains(dmcKey),"peer adopts host PCM");
        require(host.restoreMovieConfiguration(error),qPrintable(error));
        // Changing only the declared hardware must fail before interpreting state.
        auto wrongProfile=config;wrongProfile.insert("cartridgeProfile","md");
        require(!host.prepareMovie(wrongProfile,error),"movie rejects another hardware profile");
        for(bool sevenZip:{false,true}) for(const auto* member:{"nested/fixture.bin","nested/fixture.ATF"}) {
            const auto path=temp.filePath(sevenZip?"fixture.7z":"fixture.zip");save(path,pack(sevenZip,member,rom));
            require(host.load(path,error),qPrintable(error));
            require(host.isFirecore() && host.contentName()==QString::fromLatin1(member) &&
                host.movieGameIdentity()==game && host.networkCompatibility()==network,"archive member retains hardware and bytes");
            require(host.hardReset(error),qPrintable(error));
            require(host.isFirecore() && host.movieGameIdentity()==game,"archive reset retains profile");
        }
        host.setExpansionDevices(ExpansionMode::Auto,ExpansionMode::Auto);
        require(host.load(md,error),qPrintable(error));
        require(!host.isFirecore(),"Firecore to MD session swap");
        require(host.movieGameIdentity()!=game && host.networkCompatibility()!=network,"same raw bytes different hardware identity");
        require(!host.movieConfiguration().contains("cartridgeProfile"),"existing hardware metadata unchanged");
        require(!host.prepareMovie(config,error),"MD rejects Firecore movie");
        require(!host.prepareNetwork(networkConfig,error),"MD rejects Firecore network configuration");
        require(!host.restoreNetworkState(networkState,error),"MD rejects Firecore network state");
        require(host.load(bin,error) && host.isFirecore(),"MD to Firecore session swap");
        QFile source(bin);require(source.open(QIODevice::ReadOnly) && source.readAll()==rom,"original BIN unchanged");
        std::puts("Firecore frontend identity, archives, resets and session swaps passed");
        return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
