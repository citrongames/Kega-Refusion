#include "core/BatterySave.h"
#include "core/LibretroHost.h"
#include "core/SegaCdBios.h"
#include "core/DataFolders.h"
#include "core/MovieSession.h"
#include <QDirIterator>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstdio>
#include <cstring>
#include <stdexcept>

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
QByteArray read(const QString& path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly), "read failed"); return file.readAll();
}
void write(const QString& path, const QByteArray& bytes) {
    QFile file(path); require(file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size(), "write failed");
}
void policy() {
    QTemporaryDir temp; require(temp.isValid(), "temporary directory failed");
    const QString path=temp.filePath("test.srm"); QString error;
    BatterySave save; QByteArray bytes(64, char(0xff));
    auto poll=[&](qint64 now, bool force=false) {
        require(save.update(path,bytes.constData(),bytes.size(),now,force,true,error),qPrintable(error));
    };
    poll(0); poll(1000); require(!QFile::exists(path),"blank SRAM created files");
    bytes[0]='a'; poll(2000); poll(3000);
    require(!QFile::exists(path),"partial save burst was written");
    poll(4000); const auto first=read(path);
    require(first==bytes && read(path+".bak")==bytes,"initial save/backup differs");
    bytes[0]='b'; poll(5000); poll(8000);
    require(read(path)==first,"minimum write interval ignored");
    poll(34000); require(read(path)==bytes && read(path+".bak")==first,"backup is not previous version");
    const auto backup=read(path+".bak");
    poll(65000); require(read(path+".bak")==backup,"unchanged data rotated backup");
    for(int i=0;i<30;++i){bytes[1]=char(i);poll(66000+i*1000);}
    require(read(path)!=bytes,"continuous writes bypassed settling before maximum interval");
    bytes[1]=31;poll(96000);require(read(path)==bytes,"continuous changes never reached bounded save deadline");
    bytes[2]='c';poll(97000,true);require(read(path)==bytes,"close-time flush ignored recent change");
    const auto stable=read(path);
    require(QFile::remove(path+".bak") && QDir().mkdir(path+".bak"),"backup failure fixture failed");
    bytes[3]='d';
    require(!save.update(path,bytes.constData(),bytes.size(),98000,true,true,error),"backup failure was ignored");
    require(read(path)==stable,"failed backup destroyed primary save");
}
void putLong(QByteArray& bytes,int offset,quint32 value) {
    qToBigEndian<quint32>(value,bytes.data()+offset);
}
QByteArray makeTestRom() {
    QByteArray rom(64 * 1024, char(0xff));
    putLong(rom, 0x00, 0x00ff0000); // initial 68000 stack pointer
    putLong(rom, 0x04, 0x00000200); // reset vector
    rom.replace(0x100, 16, "SEGA GENESIS    ");
    rom.replace(0x120, 16, "CODEX TEST ROM  ");
    rom.replace(0x150, 16, "CODEX TEST ROM  ");
    rom.replace(0x180, 2, "GM");
    putLong(rom, 0x1a0, 0);
    putLong(rom, 0x1a4, quint32(rom.size() - 1));
    putLong(rom, 0x1a8, 0x00ff0000);
    putLong(rom, 0x1ac, 0x00ffffff);
    rom.replace(0x1f0, 1, "U");

    int pc = 0x200;
    const auto word = [&](quint16 value) {
        rom[pc++] = char(value >> 8);
        rom[pc++] = char(value & 0xff);
    };
    // MOVE.W #$8004, ($C00004).L: enable colour palette output.
    word(0x33fc); word(0x8004); word(0x00c0); word(0x0004);
    // MOVE.W #$8144, ($C00004).L: enable the VDP display.
    word(0x33fc); word(0x8144); word(0x00c0); word(0x0004);
    // MOVE.W #$8700, ($C00004).L: backdrop uses CRAM colour zero.
    word(0x33fc); word(0x8700); word(0x00c0); word(0x0004);
    word(0x7000); // MOVEQ #0,D0
    const int waitVBlank = pc;
    word(0x3239); word(0x00c0); word(0x0004); // MOVE.W VDP status,D1
    word(0x0241); word(0x0008);                // ANDI.W #8,D1
    const int branchOn = pc;
    word(0x6700);                              // BEQ.S waitVBlank
    // MOVE.L #$C0000000, ($C00004).L: set CRAM write address zero.
    word(0x23fc); word(0xc000); word(0x0000); word(0x00c0); word(0x0004);
    word(0x33c0); word(0x00c0); word(0x0000); // MOVE.W D0, ($C00000).L
    word(0x5440);                              // ADDQ.W #2,D0
    const int waitVBlankEnd = pc;
    word(0x3239); word(0x00c0); word(0x0004);
    word(0x0241); word(0x0008);
    const int branchOff = pc;
    word(0x6600);                              // BNE.S waitVBlankEnd
    const int branchAgain = pc;
    word(0x6000);                              // BRA.S waitVBlank
    for (const auto [at, target] : {std::pair{branchOn, waitVBlank},
                                    std::pair{branchOff, waitVBlankEnd},
                                    std::pair{branchAgain, waitVBlank}}) {
        const int displacement = target - (at + 2);
        require(displacement >= -128 && displacement <= 127 && displacement != 0,
                "test ROM branch out of range");
        rom[at + 1] = char(displacement & 0xff);
    }
    return rom;
}

QByteArray makeSaveRamRom() {
    QByteArray rom = makeTestRom();
    rom.replace(0x1b0, 2, "RA");
    rom[0x1b2] = char(0xf8);
    rom[0x1b3] = char(0x20);
    putLong(rom, 0x1b4, 0x00200000);
    putLong(rom, 0x1b8, 0x0020ffff);
    // Prefix the existing video program with MOVE.B #$42,($200000).L.
    // All branches in the copied program remain relative to each other.
    const QByteArray oldProgram = rom.mid(0x200, 0x100);
    putLong(rom, 0x04, 0x00000300);
    rom[0x300] = char(0x13); rom[0x301] = char(0xfc);
    rom[0x302] = char(0x00); rom[0x303] = char(0x42);
    rom[0x304] = char(0x00); rom[0x305] = char(0x20);
    rom[0x306] = char(0x00); rom[0x307] = char(0x00);
    rom.replace(0x308, oldProgram.size(), oldProgram);
    return rom;
}

QByteArray replay(LibretroHost& host) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for(int i=0;i<60;++i) {
        host.runFrame();
        const auto& frame=host.frame();
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(frame.constBits()),frame.sizeInBytes()));
        hash.addData(host.audio());
    }
    return hash.result();
}
void cartridgeIntegration(const QString& core, const QString& oldCore) {
    QTemporaryDir temp; require(temp.isValid(),"temporary directory failed");QString error;
    const auto rom=makeSaveRamRom();const auto path=temp.filePath("original.md");write(path,rom);
    const auto data=temp.filePath("data");
    const auto ram=data+"/saves/original.srm";
    QByteArray expectedReplay;
    {
        LibretroHost host(oldCore,data);require(host.load(path,error),qPrintable(error));
        for(int i=0;i<4;++i)host.runFrame();
        require(host.saveState(0,error),"old-core state save failed");
        require(host.loadState(0,error),"old-core state roundtrip failed");
        expectedReplay=replay(host);
    }
    {
        LibretroHost host(core,data);require(host.load(path,error),qPrintable(error));
        require(host.loadState(0,error),qPrintable(error));
        // The core rebuilds native FM pointers on deserialize, so raw state
        // bytes differ across shared libraries. Compare deterministic replay.
        require(replay(host)==expectedReplay,"previous-core state replay differs");
        const auto original=read(ram);
        require(quint8(original[0])==0x42,"original cartridge did not write SRAM");
        // Roll back through a real serialized state with SRAM data.
        const auto rewind=host.captureRewindState();
        QLibrary lib(core);
        auto memory=reinterpret_cast<void*(*)(unsigned)>(lib.resolve("retro_get_memory_data"));
        require(memory,"core memory API missing");
        static_cast<char*>(memory(RETRO_MEMORY_SAVE_RAM))[0]=0x55;
        require(host.pollSaveRam(1000,error)&&host.pollSaveRam(3000,error),qPrintable(error));
        require(quint8(read(ram)[0])==0x55&&read(ram+".bak")==original,"live SRAM or backup not saved");
        require(host.restoreRewindState(rewind),"rewind restore failed");
        require(host.pollSaveRam(4000,error)&&host.pollSaveRam(33000,error),qPrintable(error));
        require(read(ram)==original&&quint8(read(ram+".bak")[0])==0x55,"rewound SRAM did not persist correctly");
    }
}
void storageRootsIntegration(const QString& core) {
    QTemporaryDir temp; require(temp.isValid(),"temporary directory failed"); QString error;
    const QString data=temp.filePath("data"), path=temp.filePath("original.md");
    const auto rom=makeSaveRamRom(); write(path,rom);
    const QString legacy=QString::fromLatin1(QCryptographicHash::hash(rom,QCryptographicHash::Sha256).toHex());
    QMap<QString,QString> first,second;
    for (const auto& category:DataFolders::categories()) {
        first.insert(category,temp.filePath(QStringLiteral("first folders/\u0441\u0435\u0433\u0430/")+category));
        second.insert(category,temp.filePath("second folders/"+category));
    }
    require(DataFolders::save(data,first,error),qPrintable(error));
    require(QDir().mkpath(first.value("saves")),"cannot create selected legacy save folder");
    write(first.value("saves")+'/'+legacy+".srm",QByteArray(65536,char(0x33)));
    LibretroHost host(core,data);
    require(host.load(path,error),qPrintable(error));
    QLibrary lib(core);
    const auto memory=reinterpret_cast<void*(*)(unsigned)>(lib.resolve("retro_get_memory_data"));
    require(memory && quint8(static_cast<char*>(memory(RETRO_MEMORY_SAVE_RAM))[0])==0x33,
            "selected folder legacy battery save was not restored");
    static_cast<char*>(memory(RETRO_MEMORY_SAVE_RAM))[0]=0x55;
    require(host.flushSaveRam(error) && host.saveState(0,error),qPrintable(error));
    require(DataFolders::save(data,second,error),qPrintable(error));
    require(host.hardReset(error),qPrintable(error));
    require(quint8(static_cast<char*>(memory(RETRO_MEMORY_SAVE_RAM))[0])==0x55,
            "power cycle moved battery restore to changed settings");
    static_cast<char*>(memory(RETRO_MEMORY_SAVE_RAM))[0]=0x66;
    require(host.flushSaveRam(error) && host.saveState(1,error),qPrintable(error));
    require(host.setCheats({{QStringLiteral("FF0000:0001"),QStringLiteral("selected folder"),false}},error),qPrintable(error));
    require(QFile::exists(first.value("cheats")+"/original.json") &&
            !QFile::exists(second.value("cheats")+"/original.json"),"loaded game cheat file moved with settings");
    require(quint8(read(first.value("saves")+"/original.srm")[0])==0x66 &&
            !QFile::exists(second.value("saves")+"/original.srm"),"loaded game battery writes split across roots");
    require(QFile::exists(first.value("states")+"/original.slot1.krs") &&
            !QFile::exists(second.value("states")+"/original.slot1.krs"),"loaded game quick states moved with settings");
    MovieSession movie(host,data);
    require(QDir().mkpath(first.value("movies")),"cannot create movie test folder");
    require(movie.startRecording(first.value("movies")+"/original.krm",error) && movie.saveSlot(2,error),qPrintable(error));
    QDirIterator movieStates(first.value("states")+"/movies",{QStringLiteral("slot2.krs")},QDir::Files,QDirIterator::Subdirectories);
    require(movieStates.hasNext(),"movie-bound quick state ignored selected states folder");
    require(movie.finish(false,error),qPrintable(error));
    host.unload();
    require(host.load(path,error),qPrintable(error));
    require(host.dataFolder("saves")==second.value("saves"),"next load did not select changed folders");
    static_cast<char*>(memory(RETRO_MEMORY_SAVE_RAM))[0]=0x77;
    require(host.flushSaveRam(error) && host.saveState(3,error),qPrintable(error));
    require(quint8(read(second.value("saves")+"/original.srm")[0])==0x77 &&
            QFile::exists(second.value("states")+"/original.slot3.krs"),"next game ignored changed storage folders");
    require(!QDir(data+"/bios").exists() && !QDir(data+"/saves").exists() &&
            !QDir(data+"/states").exists(),"core created default storage folders despite overrides");
}
void cdIntegration(const QString& core, const QString& bios) {
    QTemporaryDir temp; require(temp.isValid(),"temporary directory failed");QString error;
    const auto data=temp.filePath("data");
    SegaCdBios::importDirectory(bios,data,error);
    require(error.isEmpty(),qPrintable(error));
    QByteArray disc(150*2048,0);disc.replace(0,14,"SEGADISCSYSTEM");disc[0x20b]=0;
    const auto cd=temp.filePath("original.iso");write(cd,disc);
    const auto cue=temp.filePath("original.cue");
    const auto otherCue=temp.filePath("other.cue");
    const QByteArray cueText("FILE \"original.iso\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n");
    write(cue,cueText);write(otherCue,cueText);
    QLibrary lib(core);
    using Getter=bool(*)(unsigned,const void**,size_t*,const char**);
    auto get=reinterpret_cast<Getter>(lib.resolve("kegarefusion_get_bram"));
    require(get,"BRAM extension missing");
    auto bramMemory=[&](const QString& expected) {
        const void* memory=nullptr;size_t size=0;const char* raw=nullptr;
        require(get(0,&memory,&size,&raw)&&memory&&size==8192&&raw,"internal BRAM unavailable");
        require(QDir::cleanPath(QString::fromUtf8(raw))==QDir::cleanPath(expected),
                "internal BRAM path does not match CD save group");
        const void* cart=nullptr;size_t cartSize=0;const char* cartPath=nullptr;
        if (get(1,&cart,&cartSize,&cartPath))
            require(cart&&cartSize&&cartPath&&
                    QFileInfo(QString::fromUtf8(cartPath)).absolutePath()==QFileInfo(expected).absolutePath(),
                    "external backup cart is outside the CD save group");
        return const_cast<char*>(static_cast<const char*>(memory));
    };
    LibretroHost host(core,data);
    require(host.load(cue,error),qPrintable(error));
    const QString bram=data+"/saves/original/scd_U.brm";
    auto* writable=bramMemory(bram);
    require(!QFile::exists(bram),"unused per-game BRAM was written");
    writable[0]^=1; // Simulate a completed in-game battery update, preserve format footer.
    require(host.pollSaveRam(1000,error)&&host.pollSaveRam(3000,error),qPrintable(error));
    const auto first=read(bram);
    require(first.size()==8192&&read(bram+".bak")==first,"live BRAM not saved before unload");
    writable[0]^=2;
    require(host.pollSaveRam(4000,error)&&host.pollSaveRam(33000,error),qPrintable(error));
    const auto second=read(bram);
    require(second!=first&&read(bram+".bak")==first,"BRAM change or previous backup missing");
    host.unload();
    require(read(bram)==second&&read(bram+".bak")==first,"core unload overwrote/rotated host backup");
    require(host.load(cue,error),qPrintable(error));
    writable=bramMemory(bram);
    require(QByteArray(writable,8192)==second,"core did not reload per-game BRAM");

    host.unload();
    require(host.load(otherCue,error),qPrintable(error));
    const QString otherBrm=data+"/saves/other/scd_U.brm";
    writable=bramMemory(otherBrm);
    require(!QFile::exists(otherBrm)&&QByteArray(writable,8192)!=second,
            "a different CD inherited the first game's BRAM");
    writable[0]^=4;
    require(host.flushSaveRam(error),qPrintable(error));
    require(read(otherBrm).size()==8192&&read(otherBrm+".bak")==read(otherBrm),
            "second CD BRAM or first backup missing");
    host.unload();
    require(host.load(cue,error),qPrintable(error));
    require(QByteArray(bramMemory(bram),8192)==second,
            "second CD overwrote the first game's BRAM");

    host.unload();
    host.setPerGameCdBackup(false);
    require(host.load(cue,error),qPrintable(error));
    const QString sharedBrm=data+"/saves/scd_U.brm";
    writable=bramMemory(sharedBrm);
    require(!QFile::exists(sharedBrm)&&QByteArray(writable,8192)!=second,
            "shared mode unexpectedly loaded per-game BRAM");
    writable[0]^=8;
    require(host.flushSaveRam(error),qPrintable(error));
    const auto shared=read(sharedBrm);
    require(shared.size()==8192&&read(sharedBrm+".bak")==shared,
            "shared CD BRAM was not written to the legacy path");
    host.unload();
    host.setPerGameCdBackup(true);
    require(host.load(cue,error),qPrintable(error));
    require(QByteArray(bramMemory(bram),8192)==second&&read(bram)==second,
            "shared CD BRAM replaced the per-game copy");
    host.unload();
    host.setPerGameCdBackup(false);
    require(host.load(cue,error),qPrintable(error));
    require(QByteArray(bramMemory(sharedBrm),8192)==shared,
            "legacy shared CD BRAM is no longer reachable");

    const QString selected=temp.filePath(QStringLiteral("selected saves/\u0441\u0435\u0433\u0430"));
    require(DataFolders::save(data,{{QStringLiteral("saves"),selected}},error),qPrintable(error));
    require(host.hardReset(error),qPrintable(error));
    require(QByteArray(bramMemory(sharedBrm),8192)==shared,
            "power cycle moved running CD BRAM into changed settings");
    host.unload();
    require(host.load(cue,error),qPrintable(error));
    const QString selectedShared=selected+"/scd_U.brm";
    writable=bramMemory(selectedShared);
    require(!QFile::exists(selectedShared) && QByteArray(writable,8192)!=shared,
            "configured shared CD BRAM fell back to default save root");
    writable[0]^=16;
    require(host.flushSaveRam(error),qPrintable(error));
    require(read(selectedShared).size()==8192,"configured shared CD BRAM was not written");
    host.unload();
    host.setPerGameCdBackup(true);
    require(host.load(cue,error),qPrintable(error));
    const QString selectedGroup=selected+"/original/scd_U.brm";
    writable=bramMemory(selectedGroup); writable[0]^=32;
    require(host.flushSaveRam(error),qPrintable(error));
    const auto selectedBytes=read(selectedGroup);
    require(host.hardReset(error),qPrintable(error));
    require(QByteArray(bramMemory(selectedGroup),8192)==selectedBytes,
            "configured per-game CD BRAM did not survive power cycle");
}
// These sequential consoles use the actual network envelope without a socket.
void networkCartridgeIntegration(const QString& core, bool linked) {
    QTemporaryDir temp; require(temp.isValid(),"temporary directory failed"); QString error;
    const auto path=temp.filePath("network.md"); write(path,makeSaveRamRom());
    QLibrary lib(core);
    const auto memory=reinterpret_cast<void*(*)(unsigned)>(lib.resolve("retro_get_memory_data"));
    const auto info=QFileInfo(core);
    QLibrary peer(info.dir().filePath(info.completeBaseName()+"_linkpeer."+info.suffix()));
    const auto peerMemory=linked
        ? reinterpret_cast<void*(*)(unsigned)>(peer.resolve("retro_get_memory_data")) : nullptr;
    require(memory && (!linked || peerMemory),"network SRAM memory APIs missing");
    auto bytes=[&](bool second=false) {
        auto* data=static_cast<char*>((second?peerMemory:memory)(RETRO_MEMORY_SAVE_RAM));
        require(data,"network SRAM unavailable"); return data;
    };
    const auto hostData=temp.filePath("host");
    const auto hostSave=hostData+"/saves/network.srm";
    QByteArray live(65536,char(0xff)); live[0]=0x73; live[0x8123]=0x5d;
    auto peerLive=live; peerLive[0]=0x26;
    QByteArray snapshot; QJsonObject configuration;
    {
        LibretroHost host(core,hostData);
        require(host.setLinkCableEnabled(linked,error) && host.load(path,error),qPrintable(error));
        host.runFrame(); require(host.flushSaveRam(error),qPrintable(error));
        const auto disk=read(hostSave);
        std::memcpy(bytes(),live.constData(),size_t(live.size()));
        if(linked) std::memcpy(bytes(true),peerLive.constData(),size_t(peerLive.size()));
        configuration=host.movieConfiguration();
        configuration.insert("networkCompatibility",QString::fromLatin1(host.networkCompatibility().toHex()));
        snapshot=host.captureNetworkState();
        require(!snapshot.isEmpty() && read(hostSave)==disk && disk!=live,
                "network capture did not preserve unsaved authoritative SRAM");
        host.setNetworkReadOnly(true); host.unload();
    }
    for(bool hasSave:{false,true}) {
        const auto guestData=temp.filePath(hasSave?"guest-saved":"guest-empty");
        const auto guestSave=guestData+"/saves/network.srm";
        const QByteArray personal(65536,char(0x29)), backup(65536,char(0x19));
        if(hasSave) {
            require(QDir().mkpath(QFileInfo(guestSave).absolutePath()),"guest save directory failed");
            write(guestSave,personal); write(guestSave+".bak",backup);
        }
        LibretroHost guest(core,guestData); require(guest.load(path,error),qPrintable(error));
        require(QByteArray(bytes(),65536)==(hasSave?personal:QByteArray(65536,char(0xff))),
                "guest SRAM fixture was not loaded");
        guest.setNetworkReadOnly(true);
        require(guest.prepareNetwork(configuration,error) &&
                guest.restoreNetworkState(snapshot,error),qPrintable(error));
        require(QByteArray(bytes(),65536)==live &&
                (!linked || QByteArray(bytes(true),65536)==peerLive),
                "network snapshot did not replace both consoles' live SRAM");
        require(guest.flushSaveRam(error),qPrintable(error));
        guest.unload(); // Real guest exit keeps disk protection until after unload.
        guest.setNetworkReadOnly(false);
        require(hasSave ? read(guestSave)==personal && read(guestSave+".bak")==backup
                        : !QFile::exists(guestSave) && !QFile::exists(guestSave+".bak"),
                "network guest exit replaced or created personal SRAM files");
    }
}
void networkCdIntegration(const QString& core, const QString& bios) {
    QTemporaryDir temp; require(temp.isValid(),"temporary directory failed"); QString error;
    QByteArray disc(150*2048,0); disc.replace(0,14,"SEGADISCSYSTEM"); disc[0x20b]=0;
    const auto path=temp.filePath("network.iso"); write(path,disc);
    QLibrary lib(core);
    using Getter=bool(*)(unsigned,const void**,size_t*,const char**);
    const auto get=reinterpret_cast<Getter>(lib.resolve("retro_kegarefusion_get_bram_raw"));
    require(get,"raw BRAM extension missing");
    auto region=[&](unsigned index, QString* path=nullptr) {
        const void* data=nullptr; size_t size=0; const char* raw=nullptr;
        require(get(index,&data,&size,&raw) && data && raw &&
                size==(index?512*1024u:8192u),"network BRAM region unavailable or wrong size");
        if(path) *path=QString::fromUtf8(raw);
        return QByteArray(static_cast<const char*>(data),qsizetype(size));
    };
    auto replace=[&](unsigned index,const QByteArray& bytes) {
        const void* data=nullptr; size_t size=0; const char* raw=nullptr;
        require(get(index,&data,&size,&raw) && data && size==size_t(bytes.size()),
                "network BRAM destination changed");
        std::memcpy(const_cast<void*>(data),bytes.constData(),size);
    };
    const auto hostData=temp.filePath("host");
    SegaCdBios::importDirectory(bios,hostData,error); require(error.isEmpty(),qPrintable(error));
    std::array<QByteArray,2> live,disk; QByteArray snapshot; QJsonObject configuration;
    {
        LibretroHost host(core,hostData); require(host.load(path,error),qPrintable(error));
        for(unsigned i=0;i<2;++i) {
            auto seed=region(i); seed[0]=char(0x31+i); replace(i,seed);
        }
        require(host.flushSaveRam(error),qPrintable(error));
        for(unsigned i=0;i<2;++i) {
            QString savePath; live[i]=region(i,&savePath); disk[i]=read(savePath);
            live[i][0]=char(0x63+i);
            // An in-progress format marker still belongs to the current core.
            live[i][live[i].size()-1]^=1; replace(i,live[i]);
        }
        configuration=host.movieConfiguration();
        configuration.insert("networkCompatibility",QString::fromLatin1(host.networkCompatibility().toHex()));
        snapshot=host.captureNetworkState(); require(!snapshot.isEmpty(),"network CD capture failed");
        for(unsigned i=0;i<2;++i) {
            QString savePath; region(i,&savePath);
            require(read(savePath)==disk[i] && live[i]!=disk[i],"CD capture wrote live BRAM to disk");
        }
        host.setNetworkReadOnly(true); host.unload();
    }
    const auto guestData=temp.filePath("guest");
    SegaCdBios::importDirectory(bios,guestData,error); require(error.isEmpty(),qPrintable(error));
    const std::array<QString,2> saves{guestData+"/saves/network/scd_U.brm",
                                     guestData+"/saves/network/4Mbit_cart.brm"};
    std::array<QByteArray,2> personal=disk,backup=disk;
    require(QDir().mkpath(QFileInfo(saves[0]).absolutePath()),"guest BRAM directory failed");
    for(unsigned i=0;i<2;++i) {
        personal[i][0]=char(0x21+i); backup[i][0]=char(0x11+i);
        write(saves[i],personal[i]); write(saves[i]+".bak",backup[i]);
    }
    LibretroHost guest(core,guestData); require(guest.load(path,error),qPrintable(error));
    for(unsigned i=0;i<2;++i)
        require(region(i)==personal[i],"guest personal BRAM fixture was not loaded");
    guest.setNetworkReadOnly(true);
    require(guest.prepareNetwork(configuration,error) &&
            guest.restoreNetworkState(snapshot,error),qPrintable(error));
    for(unsigned i=0;i<2;++i)
        require(region(i)==live[i],"network snapshot did not restore raw CD BRAM");
    require(guest.flushSaveRam(error),qPrintable(error));
    guest.unload(); guest.setNetworkReadOnly(false);
    for(unsigned i=0;i<2;++i)
        require(read(saves[i])==personal[i] && read(saves[i]+".bak")==backup[i],
                "network guest exit replaced personal CD BRAM or backup");
}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);
    try{
        policy();
        if(argc>=2) {
            const auto core=QString::fromLocal8Bit(argv[1]);
            storageRootsIntegration(core);
            networkCartridgeIntegration(core,false);
            networkCartridgeIntegration(core,true);
        }
        if(argc==4) {
            cartridgeIntegration(QString::fromLocal8Bit(argv[1]),QString::fromLocal8Bit(argv[2]));
            cdIntegration(QString::fromLocal8Bit(argv[1]),QString::fromLocal8Bit(argv[3]));
            networkCdIntegration(QString::fromLocal8Bit(argv[1]),QString::fromLocal8Bit(argv[3]));
        } else if(argc==3) {
            cdIntegration(QString::fromLocal8Bit(argv[1]),QString::fromLocal8Bit(argv[2]));
            networkCdIntegration(QString::fromLocal8Bit(argv[1]),QString::fromLocal8Bit(argv[2]));
        } else require(argc==1,"usage: battery_tests [CORE BIOS_DIR | CORE OLD_CORE BIOS_DIR]");
        std::puts("Battery saves: debounce, rate limit, backup, failure preservation and requested integration passed");
        return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
}
