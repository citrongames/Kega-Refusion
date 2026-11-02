#include "core/LibretroHost.h"
#include "core/MovieSession.h"
#include "core/RewindBuffer.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QLibrary>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstdio>
#include <chrono>
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
    for (int i=10;i<14;++i) expected.push_back(step(movie,host,memory,inputFor(i,true),error));
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
    const auto beforeFailedPlay=host.captureMovieState();
    const auto beforeFailedConfig=host.movieConfiguration();
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
    require(host.restoreMovieState(beforeFailedPlay,error),qPrintable(error));
    auto corruptInitial=beforeFailedPlay;
    corruptInitial[corruptInitial.size()-1] ^= 1;
    MovieTape badTape;
    const QString badPath=temp.filePath("bad-initial.krm");
    require(badTape.create(beforeFailedConfig,corruptInitial,error) &&
            badTape.save(badPath,error),qPrintable(error));
    require(!movie.play(badPath,false,error),"corrupt initial state was accepted");
    error.clear();
    require(host.loaded() && host.movieConfiguration()==beforeFailedConfig,
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
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc,argv);
    if (argc!=2) { std::fprintf(stderr,"usage: movie_integration_tests CORE\n"); return 2; }
    try {
        integration(QString::fromLocal8Bit(argv[1]));
        std::puts("Movie integration: deterministic replay, slots, rewind, resume, battery guard and fork passed");
        return 0;
    } catch (const std::exception& failure) {
        std::fprintf(stderr,"FAIL: %s\n",failure.what()); return 1;
    }
}
