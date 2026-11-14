#include "core/LibretroHost.h"
#include "core/MovieSession.h"
#include "core/RewindBuffer.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLibrary>
#include <QTemporaryDir>
#include <QtEndian>
#include <array>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void writeFile(const QString& path, const QByteArray& data) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size(),
            "could not create fixture");
}
QByteArray readFile(const QString& path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly), "could not read fixture");
    return file.readAll();
}
// Original v1 slot checkpoints contained three button masks per RLE run.
QByteArray asLegacyCheckpoint(const QByteArray& modern) {
    require(modern.size() >= 72+32 && modern.left(8) == QByteArrayLiteral("KRMCHK01") &&
            qFromLittleEndian<quint32>(modern.constData()+8) == 2,
            "modern checkpoint fixture invalid");
    const quint32 count = qFromLittleEndian<quint32>(modern.constData()+68);
    require(quint64(modern.size()) == 72 + quint64(count)*45 + 32,
            "modern checkpoint length invalid");
    QByteArray old = modern.left(72);
    qToLittleEndian<quint32>(1, old.data()+8);
    for (quint32 i = 0; i < count; ++i) {
        const char* run = modern.constData()+72+qsizetype(i)*45;
        old.append(run,10); old.append(run+20,25);
    }
    old.append(QCryptographicHash::hash(old,QCryptographicHash::Sha256));
    return old;
}
void makeSlotCheckpointLegacy(const QString& path) {
    const QByteArray slot = readFile(path);
    require(slot.size() >= 40+36 && slot.left(8) == QByteArrayLiteral("KRMSLOT1"),
            "movie slot fixture invalid");
    QByteArray payload = slot.mid(40);
    const quint32 state = qFromBigEndian<quint32>(payload.constData());
    const quint32 prefix = qFromBigEndian<quint32>(payload.constData()+4);
    require(quint64(36)+state+prefix <= quint64(payload.size()),
            "movie slot prefix range invalid");
    const QByteArray old = asLegacyCheckpoint(payload.mid(36+state,prefix));
    payload.replace(36+state,prefix,old);
    qToBigEndian<quint32>(quint32(old.size()),payload.data()+4);
    writeFile(path,QByteArrayLiteral("KRMSLOT1")+
            QCryptographicHash::hash(payload,QCryptographicHash::Sha256)+payload);
}
void putLong(QByteArray& data, int at, quint32 value) { qToBigEndian(value, data.data()+at); }

// Original 68000 program: sample port A every VBlank, store its value in
// cartridge SRAM and system RAM, and use it as the next backdrop colour.
QByteArray makeInputRom() {
    QByteArray rom(64 * 1024, char(0xff));
    putLong(rom,0,0x00ff0000); putLong(rom,4,0x00000200);
    rom.replace(0x100,16,"SEGA GENESIS    ");
    rom.replace(0x120,16,"MOVIE INPUT TEST");
    rom.replace(0x150,16,"MOVIE INPUT TEST");
    rom.replace(0x180,2,"GM");
    putLong(rom,0x1a0,0); putLong(rom,0x1a4,quint32(rom.size()-1));
    putLong(rom,0x1a8,0x00ff0000); putLong(rom,0x1ac,0x00ffffff);
    rom.replace(0x1f0,1,"U");
    rom.replace(0x1b0,2,"RA"); rom[0x1b2]=char(0xf8); rom[0x1b3]=char(0x20);
    putLong(rom,0x1b4,0x00200000); putLong(rom,0x1b8,0x0020ffff);
    int pc=0x200;
    const auto word=[&](quint16 v) { rom[pc++]=char(v>>8); rom[pc++]=char(v); };
    // VDP display and backdrop, then port A TH high.
    word(0x33fc);word(0x8004);word(0x00c0);word(0x0004);
    word(0x33fc);word(0x8144);word(0x00c0);word(0x0004);
    word(0x33fc);word(0x8700);word(0x00c0);word(0x0004);
    word(0x13fc);word(0x0040);word(0x00a1);word(0x0009);
    word(0x13fc);word(0x0040);word(0x00a1);word(0x0003);
    word(0x7000);
    const int waitOn=pc;
    word(0x3239);word(0x00c0);word(0x0004);
    word(0x0241);word(0x0008);
    const int branchOn=pc;word(0x6700);
    word(0x23fc);word(0xc000);word(0x0000);word(0x00c0);word(0x0004);
    word(0x33c0);word(0x00c0);word(0x0000);
    // MOVE.B $A10003,D0; copy it into RAM and SRAM for input assertions.
    word(0x1039);word(0x00a1);word(0x0003);
    word(0x33c0);word(0x00ff);word(0x0000);
    word(0x13c0);word(0x0020);word(0x0000);
    const int waitOff=pc;
    word(0x3239);word(0x00c0);word(0x0004);
    word(0x0241);word(0x0008);
    const int branchOff=pc;word(0x6600);
    const int branchAgain=pc;word(0x6000);
    for (const auto [from,to] : {std::pair{branchOn,waitOn},
                                 std::pair{branchOff,waitOff},
                                 std::pair{branchAgain,waitOn}}) {
        const int displacement=to-(from+2);
        require(displacement >= -128 && displacement <= 127 && displacement != 0,
                "fixture branch out of range");
        rom[from+1]=char(displacement & 0xff);
    }
    return rom;
}
using Memory = void* (*)(unsigned);
QByteArray observation(LibretroHost& host, Memory memory) {
    const auto& frame=host.frame();
    require(!frame.isNull(), "fixture did not render video");
    const auto* ram=static_cast<const char*>(memory(RETRO_MEMORY_SYSTEM_RAM));
    const auto* sram=static_cast<const char*>(memory(RETRO_MEMORY_SAVE_RAM));
    require(ram && sram, "fixture did not expose RAM and SRAM");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView(reinterpret_cast<const char*>(frame.constBits()),frame.sizeInBytes()));
    hash.addData(QByteArrayView(ram,64));
    hash.addData(QByteArrayView(sram,64));
    return hash.result();
}
MovieInput inputFor(int frame, bool alternate=false) {
    MovieInput input;
    input.buttons[0]=quint16(alternate ? ((frame & 1) ? 1u<<4 : 1u<<5)
                                          : ((frame % 3) ? 1u<<0 : 1u<<7));
    return input;
}
QByteArray step(MovieSession& movie, LibretroHost& host, Memory memory,
                const MovieInput& live, QString& error) {
    require(movie.beforeFrame(live,error), qPrintable(error));
    host.runFrame(true,false); movie.afterFrame();
    return observation(host,memory);
}
// Original Z80 loop; a four-byte CRC fixup selects the PAL metadata rule
// without including copyrighted game code or graphics.
QByteArray makeSmsRegionFixture() {
    QByteArray rom(0x80000,char(0));
    rom.replace(0,10,QByteArray::fromHex("f331f0df2100c03418fd"));
    rom.replace(0x7ff0,8,"TMR SEGA");rom[0x7fff]=char(0x41);
    const auto crc=[&](){
        quint32 value=0xffffffff;
        static const auto table=[](){std::array<quint32,256> t{};
            for(quint32 i=0;i<256;++i){quint32 v=i;for(int n=0;n<8;++n)v=(v>>1)^((v&1)?0xedb88320u:0);t[i]=v;}return t;}();
        for(const char byte:rom)value=table[(value^quint8(byte))&255]^(value>>8);
        return value^0xffffffff;
    };
    const int fix=rom.size()-4;const quint32 initial=crc();quint32 basis[32]{},masks[32]{};
    for(unsigned bit=0;bit<32;++bit){rom[fix+int(bit/8)]=char(1u<<(bit%8));quint32 v=crc()^initial,m=quint32(1)<<bit;rom[fix+int(bit/8)]=0;
        for(int k=31;k>=0;--k)if(v&(quint32(1)<<k)){if(basis[k]){v^=basis[k];m^=masks[k];}else{basis[k]=v;masks[k]=m;break;}}}
    quint32 delta=0xc8718d40u^initial,mask=0;
    for(int k=31;k>=0;--k)if(delta&(quint32(1)<<k)){require(basis[k]!=0,"SMS fixture CRC matrix failed");delta^=basis[k];mask^=masks[k];}
    for(int i=0;i<4;++i)rom[fix+i]=char(mask>>(8*i));
    require(crc()==0xc8718d40u,"SMS region fixture CRC mismatch");return rom;
}

void smsRegionRecording(const QString& core) {
    QTemporaryDir temp;require(temp.isValid(),"SMS movie temp failed");QString error;
    const auto rom=makeSmsRegionFixture();const QString path=temp.filePath("renamed-original-fixture.sms");writeFile(path,rom);
    LibretroHost host(core,temp.filePath("data"));host.setExpansionDevices(ExpansionMode::Auto, ExpansionMode::Auto);
    require(host.load(path,error),qPrintable(error));host.runFrame();
    require(host.isMasterSystem()&&std::abs(host.fps()-50.0)<1.0,"SMS region fixture did not select PAL AUTO");
    require(host.setRegion(2,error)&&host.hardReset(error),qPrintable(error));host.runFrame();
    const double ntsc=host.fps();require(std::abs(ntsc-60.0)<1.0,"SMS manual NTSC did not override PAL metadata");
    auto metadata=host.movieConfiguration();auto options=metadata.value("options").toObject();options.insert("genesis_plus_gx_region_detect","auto");metadata.insert("options",options);
    // Simulate an older AUTO recording whose native snapshot was made at NTSC.
    const auto snapshot=host.captureMovieState();MovieTape legacy;
    const QString oldPath=temp.filePath("legacy-auto-ntsc.krm");
    require(legacy.create(metadata,snapshot,error)&&legacy.append(MovieInput{},error)&&legacy.save(oldPath,error),qPrintable(error));
    MovieSession movie(host,temp.filePath("data"));
    require(movie.play(oldPath,false,error),qPrintable(error));
    require(std::abs(host.fps()-ntsc)<1e-9&&std::abs(movie.frameRate()-ntsc)<1e-9,"legacy SMS AUTO replay lost its native NTSC cadence");
    require(host.movieConfiguration().value("options").toObject().value("genesis_plus_gx_region_detect").toString()=="ntsc-u","legacy cadence was not movie-local pinned");
    require(movie.finish(false,error),qPrintable(error));
    // A pending user AUTO change must be applied for a new recording; its old
    // live NTSC frame rate must not be treated as legacy replay metadata.
    require(host.setRegion(0,error),qPrintable(error));
    const QString newPath=temp.filePath("new-auto-pal.krm");
    require(movie.startRecording(newPath,error),qPrintable(error));const double pal=host.fps();
    require(std::abs(pal-50.0)<1.0&&std::abs(movie.frameRate()-pal)<1e-9,"new SMS recording retained stale pre-reset NTSC cadence");
    MovieTape current;require(current.load(newPath,error),qPrintable(error));
    require(std::abs(current.metadata().value("frameRate").toDouble()-pal)<1e-9,"new SMS recording did not store actual PAL rate");
    require(current.metadata().value("options").toObject().value("genesis_plus_gx_region_detect").toString()=="auto","new recording unnecessarily pinned AUTO");
    require(movie.finish(false,error)&&movie.play(newPath,false,error),qPrintable(error));
    require(std::abs(host.fps()-pal)<1e-9&&host.movieConfiguration().value("options").toObject().value("genesis_plus_gx_region_detect").toString()=="auto","current SMS PAL recording did not keep AUTO");
    require(movie.finish(false,error),qPrintable(error));
    require(readFile(path)==rom,"SMS region fixture source changed");
}

void multitapRecording(const QString& core) {
    QTemporaryDir temp; require(temp.isValid(), "multitap movie directory failed");
    const QString romPath=temp.filePath("multitap.md");
    const QString moviePath=temp.filePath("multitap.krm");
    const QString dataPath=temp.filePath("data");
    writeFile(romPath,makeInputRom());
    QString error;
    LibretroHost host(core,dataPath);
    require(host.load(romPath,error),qPrintable(error));
    QLibrary library(core);
    const auto memory=reinterpret_cast<Memory>(library.resolve("retro_get_memory_data"));
    require(memory,"multitap fixture memory API missing");
    require(host.setControllerAdapter(ControllerAdapter::TeamPlayerBoth,error),qPrintable(error));
    for (unsigned player=0;player<8;++player) {
        host.setControllerConnected(player,true);
        require(host.controllerConnected(player),"multitap player disconnected");
    }
    require(host.controllerPlayerCount()==8,"multitap player count wrong");
    MovieSession movie(host,dataPath);
    require(movie.startRecording(moviePath,error),qPrintable(error));
    const auto topology=QJsonDocument::fromJson(movie.controllers());
    require(topology.isObject() && topology.object().value("adapter").toInt()==3 &&
            topology.object().value("controllers").toArray().size()==8,
            "active movie did not expose eight-player adapter topology");
    std::vector<MovieInput> inputs;
    std::vector<QByteArray> expected;
    for (int i=0;i<8;++i) {
        MovieInput value=inputFor(i);
        value.buttons[2]=quint16(1u<<(i%8));
        value.buttons[3]=quint16(1u<<((i+2)%8));
        value.buttons[7]=quint16(1u<<((i+4)%8));
        inputs.push_back(value);
        expected.push_back(step(movie,host,memory,value,error));
        if (i==3) require(movie.saveSlot(1,error),qPrintable(error));
    }
    require(movie.finish(false,error),qPrintable(error));
    MovieTape tape;
    require(tape.load(moviePath,error) && tape.size()==8,qPrintable(error));
    require(tape.metadata().value("adapter").toInt()==3 &&
            tape.metadata().value("controllers").toArray().size()==8,
            "recorded multitap configuration missing");
    for (int i=0;i<8;++i) {
        MovieInput read;
        require(tape.inputAt(i,read) && read==inputs[size_t(i)],
                "player three, four or eight mask lost in movie file");
    }
    require(host.setControllerAdapter(ControllerAdapter::None,error),qPrintable(error));
    require(movie.play(moviePath,false,error),qPrintable(error));
    require(host.controllerAdapter()==ControllerAdapter::TeamPlayerBoth &&
            host.controllerPlayerCount()==8,"movie playback did not restore multitap adapter");
    for (int i=0;i<8;++i) {
        MovieInput decoy; decoy.buttons.fill(0xffff);
        require(step(movie,host,memory,decoy,error)==expected[size_t(i)],
                "multitap movie playback diverged");
    }
    QImage frame; double aspect=0; bool missing=false;
    require(movie.loadSlot(1,frame,aspect,missing,error) && movie.position()==4,
            "multitap movie checkpoint did not restore");
    for (int i=4;i<8;++i) {
        MovieInput decoy; decoy.buttons.fill(0xffff);
        require(step(movie,host,memory,decoy,error)==expected[size_t(i)],
                "multitap movie checkpoint replay diverged");
    }
    require(movie.finish(false,error),qPrintable(error));
    require(movie.play(moviePath,true,error) && movie.recording() && movie.position()==8,
            "multitap movie resume failed");
    MovieInput continued=inputs.back(); continued.buttons[7]=0x8000;
    step(movie,host,memory,continued,error);
    require(movie.finish(false,error) && tape.load(moviePath,error) &&
            tape.size()==9,qPrintable(error));
    MovieInput read;
    require(tape.inputAt(8,read) && read==continued,"resumed player eight input lost");
}

void integration(const QString& core) {
    QTemporaryDir temp; require(temp.isValid(), "temporary directory failed");
    const QString romPath=temp.filePath("movie-test.md");
    const QString moviePath=temp.filePath("original.krm");
    const QString forkPath=temp.filePath("branch.krm");
    const QString dataPath=temp.filePath("data");
    writeFile(romPath,makeInputRom());
    QString error;
    LibretroHost host(core,dataPath);
    require(host.load(romPath,error),qPrintable(error));
    QLibrary library(core);
    const auto memory=reinterpret_cast<Memory>(library.resolve("retro_get_memory_data"));
    require(memory,"core memory API missing");
    require(host.cheatsSupported(),"movie fixture requires cheat-capable core");
    require(host.setCheats({{QStringLiteral("00FF00:0000"),QStringLiteral("disabled test code"),false}},error),
            qPrintable(error));
    const auto recordedConfig=host.movieConfiguration();
    require(!recordedConfig.isEmpty(),"movie config was empty");
    require(host.saveState(0,error),qPrintable(error));
    const QString normalState=dataPath+"/states/movie-test.slot0.krs";
    const QByteArray normalBytes=readFile(normalState);
    MovieSession movie(host,dataPath);
    require(movie.startRecording(moviePath,error),qPrintable(error));
    require(movie.recording() && movie.position()==0 && host.movieConfiguration()==recordedConfig,
            "recording did not start from saved configuration");
    std::vector<QByteArray> expected;
    for (int i=0;i<10;++i) expected.push_back(step(movie,host,memory,inputFor(i),error));
    require(movie.saveSlot(0,error),qPrintable(error));
    require(movie.occupiedSlots() & 1u,"movie slot was not created");
    MovieTape slotMovie;
    require(slotMovie.load(moviePath,error),qPrintable(error));
    const QString sourceSlot=dataPath+"/states/movies/original_"+
        QString::fromLatin1(slotMovie.id().toHex())+"/slot0.krs";
    require(!QFileInfo::exists(sourceSlot + QStringLiteral(".bak")),
            "first movie quick save unexpectedly created a backup");
    makeSlotCheckpointLegacy(sourceSlot); // Exercise embedded v1 checkpoint during record and replay.

    const auto rewind=movie.captureRewind();
    require(!rewind.isEmpty(),"movie rewind snapshot unavailable");
    for (int i=10;i<16;++i) step(movie,host,memory,inputFor(i),error);
    require(movie.position()==16,"recording cursor did not advance");
    require(movie.restoreRewind(rewind,error) && movie.position()==10,qPrintable(error));
    for (int i=10;i<13;++i) step(movie,host,memory,inputFor(i,true),error);
    QImage savedFrame; double aspect=0; bool missing=false;
    require(movie.loadSlot(0,savedFrame,aspect,missing,error),qPrintable(error));
    require(movie.position()==10 && !savedFrame.isNull() && aspect>0,
            "movie slot did not restore its prefix and frame");
    const QString backupTestSlot = QFileInfo(sourceSlot).dir().filePath(QStringLiteral("slot1.krs"));
    require(movie.saveSlot(1,error),qPrintable(error));
    const QByteArray firstMovieSlot = readFile(backupTestSlot);
    error.clear();
    require(movie.saveSlot(1, error), qPrintable(error));
    require(readFile(backupTestSlot + QStringLiteral(".bak")) == firstMovieSlot,
            "movie quick slot backup did not preserve the previous save");
    const QByteArray secondMovieSlot = readFile(backupTestSlot);
    for (int i=10;i<14;++i) expected.push_back(step(movie,host,memory,inputFor(i,true),error));
    error.clear();
    require(movie.saveSlot(1, error), qPrintable(error));
    require(readFile(backupTestSlot + QStringLiteral(".bak")) == secondMovieSlot,
            "movie quick slot backup did not roll forward exactly once");
    const QByteArray thirdMovieSlot = readFile(backupTestSlot);
    require(QFile::remove(backupTestSlot + QStringLiteral(".bak")), "could not remove movie test backup");
    require(QDir().mkpath(backupTestSlot + QStringLiteral(".bak")), "could not block movie test backup path");
    error.clear();
    require(!movie.saveSlot(1, error) && !error.isEmpty(),
            "movie quick slot overwrote primary after backup failure");
    require(readFile(backupTestSlot) == thirdMovieSlot, "backup failure changed movie quick slot");
    require(QDir().rmdir(backupTestSlot + QStringLiteral(".bak")), "could not remove movie test backup directory");
    const auto savedClock=host.runClock();
    require(savedClock.frames==movie.position() && savedClock.ticks>0,
            "movie finish did not capture its elapsed frame clock");
    require(movie.finish(false,error),qPrintable(error));
    require(readFile(normalState)==normalBytes,"movie slot changed a normal quick state");
    require(host.movieConfiguration()==recordedConfig,"recording left a configuration overlay");

    // Make local preferences differ from the recording. Replay must use the
    // recorded configuration, then restore the user's current preferences.
    require(host.setRegion(3,error),qPrintable(error));
    require(host.setControllerConnected(1,true),"test topology did not change");
    require(host.setCheats({{QStringLiteral("00FF02:0000"),QStringLiteral("user code"),false}},error),
            qPrintable(error));
    const auto liveConfig=host.movieConfiguration();
    require(liveConfig!=recordedConfig,"test did not change live preferences");
    require(movie.play(moviePath,false,error),qPrintable(error));
    require(host.movieConfiguration()==recordedConfig,"replay did not apply recorded configuration");
    const QString savePath=dataPath+"/saves/movie-test.srm";
    const auto diskBattery=readFile(savePath);
    for (size_t i=0;i<expected.size();++i) {
        MovieInput decoy=inputFor(int(i),true); decoy.buttons[0]=0xffff;
        require(step(movie,host,memory,decoy,error)==expected[i],
                "replay future video or CPU/RAM diverged");
    }
    require(movie.mode()==MovieSession::Finished && movie.position()==expected.size(),
            "playback did not stop at recorded end");
    require(host.pollSaveRam(1000000,error),qPrintable(error));
    require(readFile(savePath)==diskBattery,"read-only playback changed user's battery save");
    require(movie.finish(false,error),qPrintable(error));
    require(host.movieConfiguration()==liveConfig,"playback did not restore user preferences");

    // Simulate a later launch by unloading and reloading the cartridge before
    // resuming the saved end state from disk.
    host.unload();
    require(host.load(romPath,error),qPrintable(error));
    require(movie.play(moviePath,true,error),qPrintable(error));
    require(movie.recording() && movie.position()==expected.size(),"resume cursor mismatch");
    const auto resumedClock=host.runClock();
    require(resumedClock.frames==savedClock.frames && resumedClock.ticks==savedClock.ticks,
            "movie resume did not restore the clock saved at finish");
    for (int i=14;i<17;++i) expected.push_back(step(movie,host,memory,inputFor(i),error));
    require(movie.finish(false,error),qPrintable(error));
    require(movie.play(moviePath,false,error),qPrintable(error));
    for (size_t i=0;i<expected.size();++i)
        require(step(movie,host,memory,inputFor(int(i),true),error)==expected[i],
                "replay after resume diverged");
    require(movie.mode()==MovieSession::Finished,"extended playback did not finish");
    require(movie.loadSlot(0,savedFrame,aspect,missing,error),qPrintable(error));
    require(movie.position()==10,"read-only slot did not seek to saved prefix");
    const auto originalBytes=readFile(moviePath);
    require(movie.rerecord(forkPath,error),qPrintable(error));
    require(movie.recording() && movie.position()==10,"takeover did not branch at cursor");
    MovieTape originalTape, forkTape;
    require(originalTape.load(moviePath,error) && forkTape.load(forkPath,error),qPrintable(error));
    const QString originalSlot=dataPath+"/states/movies/original_"+
        QString::fromLatin1(originalTape.id().toHex())+"/slot0.krs";
    const QString forkSlot=dataPath+"/states/movies/branch_"+
        QString::fromLatin1(forkTape.id().toHex())+"/slot0.krs";
    require(QDir().mkpath(QFileInfo(forkSlot).absolutePath()),"fork slot directory failed");
    writeFile(forkSlot,readFile(originalSlot));
    require(!movie.loadSlot(0,savedFrame,aspect,missing,error) && !missing,
            "fork accepted a slot from the original movie ID");
    error.clear();
    for (int i=10;i<12;++i) step(movie,host,memory,inputFor(i),error);
    require(movie.finish(false,error),qPrintable(error));
    require(readFile(moviePath)==originalBytes,"takeover overwrote original movie");
    require(readFile(normalState)==normalBytes,"movie work changed normal quick state");

    // A valid movie envelope can still contain a corrupt native snapshot.
    // Preparation has already reset the core when that snapshot is rejected;
    // the failed start must put the former live game back exactly.
    // A restored user region option is pending until one normal frame. Settle
    // it before comparing the failed-start rollback against a stable baseline.
    step(movie,host,memory,inputFor(0),error);
    const auto beforeFailedPlay=host.captureMovieState();
    const auto beforeFailedConfig=host.movieConfiguration();
    const double beforeFailedFps=host.fps();
    require(!beforeFailedPlay.isEmpty() && !host.frame().isNull(),
            "failed-start baseline unavailable");
    const auto batteryPart=[](const QByteArray& state) {
        const quint32 nativeSize=qFromBigEndian<quint32>(state.constData()+72);
        return state.mid(128+nativeSize);
    };
    const auto beforeBattery=batteryPart(beforeFailedPlay);
    const QByteArray beforeRam(static_cast<const char*>(memory(RETRO_MEMORY_SYSTEM_RAM)),64*1024);
    std::vector<QByteArray> expectedAfterFailure;
    for (int i=0;i<4;++i) expectedAfterFailure.push_back(step(movie,host,memory,inputFor(i),error));
    require(std::abs(host.fps()-beforeFailedFps)<1e-9,
            "reference frames changed settled region timing");
    require(host.restoreMovieState(beforeFailedPlay,error),qPrintable(error));
    require(std::abs(host.fps()-beforeFailedFps)<1e-9,
            "reference state restore changed settled region timing");
    auto corruptInitial=beforeFailedPlay;
    corruptInitial[corruptInitial.size()-1] ^= 1;
    MovieTape badTape;
    const QString badPath=temp.filePath("bad-initial.krm");
    require(badTape.create(beforeFailedConfig,corruptInitial,error) &&
            badTape.save(badPath,error),qPrintable(error));
    require(!movie.play(badPath,false,error),"corrupt initial state was accepted");
    error.clear();
    require(host.loaded() && host.movieConfiguration()==beforeFailedConfig &&
            std::abs(host.fps()-beforeFailedFps)<1e-9,
            "failed playback did not restore live configuration");
    require(batteryPart(host.captureMovieState())==beforeBattery &&
            QByteArray(static_cast<const char*>(memory(RETRO_MEMORY_SYSTEM_RAM)),64*1024)==beforeRam,
            "failed playback did not restore RAM or battery");
    for (int i=0;i<4;++i)
        require(step(movie,host,memory,inputFor(i),error)==expectedAfterFailure[size_t(i)],
                "failed playback changed future video or CPU/RAM");

    // Fast-forward can suppress intermediate video without changing any
    // emulated frame. Compare its final native/battery state and rendered frame
    // against ordinary playback of the same recorded input sequence.
    const QString fastPath=temp.filePath("fast-forward.krm");
    require(movie.startRecording(fastPath,error),qPrintable(error));
    for (int i=0;i<24;++i) {
        require(movie.beforeFrame(inputFor(i),error),qPrintable(error));
        host.runFrame(i==23,false); movie.afterFrame();
    }
    const auto acceleratedState=host.captureMovieState();
    const auto acceleratedFrame=observation(host,memory);
    require(!acceleratedState.isEmpty(),"accelerated movie state unavailable");
    require(movie.finish(false,error),qPrintable(error));
    require(movie.play(fastPath,false,error),qPrintable(error));
    for (int i=0;i<24;++i) {
        require(movie.beforeFrame(inputFor(i,true),error),qPrintable(error));
        host.runFrame(true,false); movie.afterFrame();
    }
    require(movie.mode()==MovieSession::Finished,"accelerated replay did not reach its end");
    require(observation(host,memory)==acceleratedFrame,
            "accelerated recording changed final video or RAM");
    require(host.captureMovieState()==acceleratedState,
            "accelerated recording changed final full state");
    require(movie.finish(false,error),qPrintable(error));

    // The app captures a full movie state and reverse delta every other frame.
    // Measure this real-core path without imposing a machine-dependent limit.
    require(movie.startRecording(temp.filePath("rewind-benchmark.krm"),error),qPrintable(error));
    RewindBuffer history; history.configure(true);
    std::chrono::nanoseconds captureTime{0}, deltaTime{0};
    const auto allStart=std::chrono::steady_clock::now();
    for (int i=0;i<120;++i) {
        require(movie.beforeFrame(inputFor(i),error),qPrintable(error));
        host.runFrame(true,false); movie.afterFrame();
        if (!(i & 1)) {
            const auto started=std::chrono::steady_clock::now();
            const auto state=movie.captureRewind();
            const auto captured=std::chrono::steady_clock::now();
            require(!state.isEmpty() && history.capture(state,host.frame(),host.aspectRatio()),
                    "movie rewind capture failed");
            const auto finished=std::chrono::steady_clock::now();
            captureTime+=captured-started; deltaTime+=finished-captured;
        }
    }
    const auto totalTime=std::chrono::steady_clock::now()-allStart;
    require(movie.finish(false,error),qPrintable(error));
    std::printf("Movie rewind benchmark: 120 frames, 60 snapshots, %.1f ms total, %.2f ms host snapshot, %.2f ms reverse delta, %lld KiB retained\n",
        std::chrono::duration<double,std::milli>(totalTime).count(),
        std::chrono::duration<double,std::milli>(captureTime).count()/60.0,
        std::chrono::duration<double,std::milli>(deltaTime).count()/60.0,
        static_cast<long long>(history.bytesUsed()/1024));
    const auto nativeStart=std::chrono::steady_clock::now();
    for (int i=0;i<60;++i) require(!host.serialize().isEmpty(),"native benchmark state failed");
    const auto nativeEnd=std::chrono::steady_clock::now();
    for (int i=0;i<60;++i) require(!host.captureMovieState().isEmpty(),"movie benchmark state failed");
    const auto fullEnd=std::chrono::steady_clock::now();
    std::printf("State capture baseline: %.2f ms native, %.2f ms full movie per snapshot; %lld/%lld KiB\n",
        std::chrono::duration<double,std::milli>(nativeEnd-nativeStart).count()/60.0,
        std::chrono::duration<double,std::milli>(fullEnd-nativeEnd).count()/60.0,
        static_cast<long long>(host.serialize().size()/1024),
        static_cast<long long>(host.captureMovieState().size()/1024));

    // Region is pending until the power reset at record start. The file must
    // retain the actual post-reset cadence, including any fractional part.
    for (const int region : {2,3}) {
        const QString ratePath=temp.filePath(QStringLiteral("region-%1.krm").arg(region));
        require(host.setRegion(region,error),qPrintable(error));
        require(movie.startRecording(ratePath,error),qPrintable(error));
        const double actual=host.fps();
        require(std::abs(actual-(region==3 ? 50.0 : 60.0))<1.0,
                "record start did not apply requested PAL/NTSC region");
        require(std::abs(movie.frameRate()-actual)<1e-9,
                "active movie rate differs from post-reset core rate");
        MovieTape rateTape;
        require(rateTape.load(ratePath,error),qPrintable(error));
        require(std::abs(rateTape.metadata().value(QStringLiteral("frameRate")).toDouble()-actual)<1e-9,
                "initial movie file stored the pre-reset frame rate");
        for (int i=0;i<3;++i) {
            require(movie.beforeFrame(inputFor(i),error),qPrintable(error));
            host.runFrame(true,false); movie.afterFrame();
        }
        require(movie.finish(false,error),qPrintable(error));
        require(rateTape.load(ratePath,error) && rateTape.resumeFrame()==3,qPrintable(error));
        require(movie.play(ratePath,true,error),qPrintable(error));
        require(movie.recording() && movie.position()==3 &&
                std::abs(movie.frameRate()-actual)<1e-9 && std::abs(host.fps()-actual)<1e-9,
                "resumed movie lost its recorded fractional frame rate");
        require(movie.finish(false,error),qPrintable(error));
    }
}

QByteArray legacySnapshot(const QByteArray& state, const QByteArray& legacyHash) {
    require(state.size() >= 128 && state.startsWith("KRMOV002") && legacyHash.size() == 32,
            "legacy snapshot source invalid");
    QByteArray old = state;
    old.replace(0,8,"KRMOV001");
    old.replace(40,32,legacyHash);
    // The native payload and its digest at offset 96 are unchanged.
    return old;
}
struct RecordedSequence {
    std::vector<QByteArray> frames;
    std::vector<QByteArray> future;
};
void verifyPlayback(const QString& core, const QString& romPath, const QString& moviePath,
                    const QString& dataPath, const RecordedSequence& expected,
                    bool expectWarning) {
    QString error;
    require(QDir().mkpath(dataPath),"compatibility playback data directory failed");
    const QString workingMovie=dataPath+QStringLiteral("/working.krm");
    writeFile(workingMovie,readFile(moviePath));
    LibretroHost host(core,dataPath);
    require(host.load(romPath,error),qPrintable(error));
    QLibrary library(core);
    const auto memory=reinterpret_cast<Memory>(library.resolve("retro_get_memory_data"));
    require(memory,"playback core memory API missing");
    MovieSession movie(host,dataPath);
    require(movie.play(workingMovie,false,error),qPrintable(error));
    require(host.movieCompatibilityWarning().isEmpty() != expectWarning,
            "unexpected core compatibility warning on playback");
    for (size_t i=0;i<expected.frames.size();++i) {
        MovieInput decoy=inputFor(int(i),true); decoy.buttons[0]=0xffff;
        require(step(movie,host,memory,decoy,error)==expected.frames[i],
                "cross-build replay video or CPU/RAM diverged");
    }
    require(movie.mode()==MovieSession::Finished && movie.position()==expected.frames.size(),
            "cross-build playback did not finish at recorded length");
    require(movie.finish(false,error),qPrintable(error));
    require(movie.play(workingMovie,true,error),qPrintable(error));
    require(host.movieCompatibilityWarning().isEmpty() != expectWarning,
            "unexpected core compatibility warning on resume");
    require(movie.recording() && movie.position()==expected.frames.size(),
            "cross-build resume cursor mismatch");
    for (size_t i=0;i<expected.future.size();++i)
        require(step(movie,host,memory,inputFor(int(expected.frames.size()+i),true),error)==
                expected.future[i], "cross-build resumed future diverged");
    require(movie.finish(false,error),qPrintable(error));
}
void compatibility(const QString& currentCore, const QString& legacyCore) {
    QTemporaryDir temp; require(temp.isValid(),"compatibility temporary directory failed");
    const QString romPath=temp.filePath("compatibility.md");
    const QString legacyPath=temp.filePath("legacy.krm");
    const QString modernPath=temp.filePath("modern.krm");
    const QString unknownOptionPath=temp.filePath("unknown-option.krm");
    const QString appendedCore=temp.filePath("same-contract-appended.so");
    const QString changedCore=temp.filePath("other-contract.so");
    writeFile(romPath,makeInputRom());
    const QByteArray legacyHash=QCryptographicHash::hash(readFile(legacyCore),QCryptographicHash::Sha256);
    require(legacyHash.toHex()=="83c593f3138f24439520fd91f7b9f1251b35cd0d63087cdc4f9dc9476e3b529a",
            "legacy fixture is not the verified original Linux core");
    QString error;
    RecordedSequence legacyExpected;
    {
        LibretroHost host(legacyCore,temp.filePath("old-data"));
        require(host.load(romPath,error),qPrintable(error));
        QLibrary library(legacyCore);
        const auto memory=reinterpret_cast<Memory>(library.resolve("retro_get_memory_data"));
        require(memory,"legacy core memory API missing");
        MovieSession movie(host,temp.filePath("old-data"));
        const QString source=temp.filePath("old-source.krm");
        require(movie.startRecording(source,error),qPrintable(error));
        MovieTape sourceTape;
        require(sourceTape.load(source,error),qPrintable(error));
        for (int i=0;i<12;++i)
            legacyExpected.frames.push_back(step(movie,host,memory,inputFor(i),error));
        const QByteArray savedEnd=host.captureMovieState();
        for (int i=12;i<16;++i)
            legacyExpected.future.push_back(step(movie,host,memory,inputFor(i,true),error));
        require(movie.finish(false,error),qPrintable(error));
        QJsonObject oldMetadata=sourceTape.metadata();
        oldMetadata.insert(QStringLiteral("version"),1);
        oldMetadata.insert(QStringLiteral("coreIdentity"),QString::fromLatin1(legacyHash.toHex()));
        oldMetadata.remove(QStringLiteral("coreBuild"));
        oldMetadata.remove(QStringLiteral("coreVersion"));
        oldMetadata.remove(QStringLiteral("coreCompatibility"));
        oldMetadata.remove(QStringLiteral("frameRate"));
        MovieTape legacyMovie;
        require(legacyMovie.create(oldMetadata,legacySnapshot(sourceTape.initialState(),legacyHash),error),
                qPrintable(error));
        for (int i=0;i<12;++i) require(legacyMovie.append(inputFor(i),error),qPrintable(error));
        require(legacyMovie.setResumeState(legacySnapshot(savedEnd,legacyHash),12,error) &&
                legacyMovie.save(legacyPath,error),qPrintable(error));
    }
    verifyPlayback(currentCore,romPath,legacyPath,temp.filePath("legacy-play-data"),
                   legacyExpected,false);

    RecordedSequence modernExpected;
    {
        LibretroHost host(currentCore,temp.filePath("modern-data"));
        require(host.load(romPath,error),qPrintable(error));
        QLibrary library(currentCore);
        const auto memory=reinterpret_cast<Memory>(library.resolve("retro_get_memory_data"));
        require(memory,"current core memory API missing");
        MovieSession movie(host,temp.filePath("modern-data"));
        require(movie.startRecording(modernPath,error),qPrintable(error));
        for (int i=0;i<12;++i)
            modernExpected.frames.push_back(step(movie,host,memory,inputFor(i),error));
        require(movie.finish(false,error),qPrintable(error));
        for (int i=12;i<16;++i)
            modernExpected.future.push_back(step(movie,host,memory,inputFor(i,true),error));
    }
    // A newer recording can mention a core option this build does not know.
    // Keep replay available and warn about the ignored value.
    MovieTape modernTape, unknownOptionMovie;
    require(modernTape.load(modernPath,error),qPrintable(error));
    QJsonObject unknownMetadata=modernTape.metadata();
    QJsonObject unknownOptions=unknownMetadata.value(QStringLiteral("options")).toObject();
    unknownOptions.insert(QStringLiteral("kegarefusion_test_future_option"),QStringLiteral("on"));
    unknownMetadata.insert(QStringLiteral("options"),unknownOptions);
    require(unknownOptionMovie.create(unknownMetadata,modernTape.initialState(),error),qPrintable(error));
    for (int i=0;i<12;++i) require(unknownOptionMovie.append(inputFor(i),error),qPrintable(error));
    require(unknownOptionMovie.setResumeState(modernTape.resumeState(),12,error) &&
            unknownOptionMovie.save(unknownOptionPath,error),qPrintable(error));
    verifyPlayback(currentCore,romPath,unknownOptionPath,temp.filePath("unknown-option-data"),
                   modernExpected,true);
    const QByteArray currentBytes=readFile(currentCore);
    writeFile(appendedCore,currentBytes+QByteArrayLiteral("compatible-rebuild-padding"));
    require(QCryptographicHash::hash(currentBytes,QCryptographicHash::Sha256) !=
            QCryptographicHash::hash(readFile(appendedCore),QCryptographicHash::Sha256),
            "appended core did not change binary hash");
    verifyPlayback(appendedCore,romPath,modernPath,temp.filePath("appended-data"),
                   modernExpected,false);

    const QByteArray oldAbi("kegarefusion-recording-v2");
    const QByteArray newAbi("kegarefusion-recording-v9");
    require(currentBytes.count(oldAbi)==1 && oldAbi.size()==newAbi.size(),
            "recording ABI marker not uniquely present in current core");
    QByteArray changedBytes=currentBytes;
    changedBytes.replace(oldAbi,newAbi);
    writeFile(changedCore,changedBytes);
    QLibrary changedLibrary(changedCore);
    const auto abi=reinterpret_cast<const char*(*)()>(changedLibrary.resolve("retro_kegarefusion_recording_abi"));
    require(abi && QByteArray(abi())==newAbi,"mutated core did not export changed ABI");
    verifyPlayback(changedCore,romPath,modernPath,temp.filePath("changed-modern-data"),
                   modernExpected,true);
    verifyPlayback(changedCore,romPath,legacyPath,temp.filePath("changed-legacy-data"),
                   legacyExpected,true);
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc,argv);
    if (argc!=2 && argc!=3) {
        std::fprintf(stderr,"usage: movie_integration_tests CORE [LEGACY_CORE|--sms-region]\n"); return 2;
    }
    try {
        if (argc==3 && QString::fromLocal8Bit(argv[2])==QLatin1String("--sms-region")) {
            smsRegionRecording(QString::fromLocal8Bit(argv[1]));
            std::puts("SMS movie region: legacy AUTO cadence, new pending AUTO and current PAL replay passed");
        } else if (argc==3) {
            compatibility(QString::fromLocal8Bit(argv[1]),QString::fromLocal8Bit(argv[2]));
            std::puts("Movie core compatibility: legacy, compatible rebuild, and advisory mismatch passed");
        } else {
            integration(QString::fromLocal8Bit(argv[1]));
            multitapRecording(QString::fromLocal8Bit(argv[1]));
            std::puts("Movie integration: deterministic replay, slots, rewind, resume, battery guard, fork and multitap passed");
        }
        return 0;
    } catch (const std::exception& failure) {
        std::fprintf(stderr,"FAIL: %s\n",failure.what()); return 1;
    }
}
