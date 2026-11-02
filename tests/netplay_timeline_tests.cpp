#include "net/NetplayTimeline.h"
#include "core/LibretroHost.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace {
void check(bool okay, const char* message) { if (!okay) throw std::runtime_error(message); }
void putLong(QByteArray& bytes, int at, quint32 value) { qToBigEndian(value, bytes.data()+at); }
QByteArray fixture() {
    // Original 68000 ROM: once per VBlank, read both controller registers and
    // store their samples in work RAM. No copyrighted content or external BIOS.
    QByteArray rom(64*1024, char(0xff));
    putLong(rom,0,0x00ff0000); putLong(rom,4,0x00000200);
    rom.replace(0x100,16,"SEGA GENESIS    ");
    rom.replace(0x120,16,"NETPLAY INPUT   ");
    rom.replace(0x150,16,"NETPLAY INPUT   ");
    rom.replace(0x180,2,"GM");
    putLong(rom,0x1a0,0); putLong(rom,0x1a4,quint32(rom.size()-1));
    putLong(rom,0x1a8,0x00ff0000); putLong(rom,0x1ac,0x00ffffff);
    rom.replace(0x1f0,1,"U");
    int pc=0x200;
    const auto word=[&](quint16 value) { rom[pc++]=char(value>>8); rom[pc++]=char(value); };
    word(0x13fc);word(0x0040);word(0x00a1);word(0x0009); // TH high
    word(0x13fc);word(0x0040);word(0x00a1);word(0x0003);
    const int waitOn=pc;
    word(0x3239);word(0x00c0);word(0x0004); // VDP status
    word(0x0241);word(0x0008);
    const int branchOn=pc; word(0x6700);
    word(0x1039);word(0x00a1);word(0x0003); // port A
    word(0x13c0);word(0x00ff);word(0x0000); // RAM[0]
    word(0x1039);word(0x00a1);word(0x0005); // port B
    word(0x13c0);word(0x00ff);word(0x0001); // RAM[1]
    const int waitOff=pc;
    word(0x3239);word(0x00c0);word(0x0004);
    word(0x0241);word(0x0008);
    const int branchOff=pc; word(0x6600);
    const int branchAgain=pc; word(0x6000);
    for (const auto [from,to] : {std::pair{branchOn,waitOn},
                                 std::pair{branchOff,waitOff},
                                 std::pair{branchAgain,waitOn}}) {
        const int displacement=to-(from+2);
        check(displacement>=-128 && displacement<=127 && displacement!=0,"fixture branch");
        rom[from+1]=char(displacement&0xff);
    }
    return rom;
}
MovieInput local(int frame) {
    MovieInput result;
    result.buttons[0]=quint16(1u<<(frame%4));
    result.aim.fill(quint64(1)<<32);
    return result;
}
MovieInput authoritative(int frame) {
    MovieInput result;
    result.buttons[0]=local(frame).buttons[0];
    result.buttons[1]=quint16(1u<<(4+(frame%4)));
    result.aim.fill(quint64(1)<<32);
    return result;
}
void run(const QString& core) {
    MovieInput parsed;
    const auto encoded=NetplayTimeline::inputBytes(authoritative(0));
    check(NetplayTimeline::parseInput(encoded,parsed) && parsed==authoritative(0),
          "input wire roundtrip");
    check(!NetplayTimeline::parseInput(encoded+QByteArray(1,'x'),parsed),
          "reject oversized input");
    MovieInput contradictory=local(0); contradictory.buttons[0]=0x30;
    const auto merged=NetplayTimeline::merge({{1,contradictory},{2,local(1)}});
    check(merged.buttons[0]==0 && merged.buttons[1]==local(1).buttons[0],
          "neutralize opposite buttons and assign ports");
    QTemporaryDir temp;
    check(temp.isValid(),"temp directory");
    const QString romPath=temp.filePath("netplay.md");
    QFile rom(romPath);
    check(rom.open(QIODevice::WriteOnly),"open fixture ROM");
    const auto data=fixture();
    check(rom.write(data)==data.size(),"write fixture ROM");
    rom.close();
    QString error;
    QVector<QByteArray> frames;
    QByteArray expectedAtFour, expectedFinal, initialState, initialDigest;
    {
        LibretroHost host(core,temp.filePath("host"));
        check(host.load(romPath,error),qPrintable(error));
        initialState=host.captureMovieState(false);
        check(!initialState.isEmpty(),"capture initial state");
        initialDigest=host.netplayDigest();
        for(int frame=0;frame<8;++frame) {
            const auto input=authoritative(frame);
            frames.append(NetplayTimeline::frameBytes(7,frame,input,host.netplayDigest()));
            NetplayTimeline::apply(host,input);
            host.runFrame(false,false);
            if(frame==3) expectedAtFour=host.netplayDigest();
        }
        expectedFinal=host.netplayDigest();
        host.unload();
    }
    {
        LibretroHost client(core,temp.filePath("client"));
        check(client.load(romPath,error),qPrintable(error));
        check(client.restoreMovieState(initialState,error,true),qPrintable(error));
        check(client.netplayDigest()==initialDigest,"initial state digest after restore");
        const auto roundtrip=client.captureMovieState(false);
        NetplayTimeline::apply(client,authoritative(0)); client.runFrame(false,false);
        check(client.restoreMovieState(roundtrip,error,true),qPrintable(error));
        check(client.netplayDigest()==initialDigest,"state digest after roundtrip");
        NetplayTimeline timeline;
        timeline.reset(7);
        MovieInput input;
        for(int frame=0;frame<4;++frame) {
            check(timeline.beforeFrame(client,local(frame),1,input,error)==NetplayTimeline::Run,
                  "predict delayed frame");
            NetplayTimeline::apply(client,input);
            client.runFrame(false,false);
            timeline.afterFrame();
        }
        for(int frame=0;frame<5;++frame) check(timeline.ingest(frames.at(frame)),"ingest delayed frame");
        const auto rollback=timeline.beforeFrame(client,local(4),1,input,error);
        if(rollback!=NetplayTimeline::Run)
            throw std::runtime_error(QStringLiteral("rollback result %1: %2")
                .arg(int(rollback)).arg(error).toStdString());
        check(client.netplayDigest()==expectedAtFour,"corrected rollback state");
        NetplayTimeline::apply(client,input); client.runFrame(false,false); timeline.afterFrame();
        for(int frame=5;frame<8;++frame) {
            check(timeline.ingest(frames.at(frame)),"ingest current frame");
            check(timeline.beforeFrame(client,local(frame),1,input,error)==NetplayTimeline::Run,
                  "run confirmed frame");
            NetplayTimeline::apply(client,input); client.runFrame(false,false); timeline.afterFrame();
        }
        check(client.netplayDigest()==expectedFinal,"same final emulation state");
        NetplayTimeline spectator;
        spectator.reset(8);
        check(spectator.beforeFrame(client,local(0),0,input,error)==NetplayTimeline::Wait,
              "spectator waits for authoritative input");
    }
}
}

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    if(argc!=2) { std::fprintf(stderr,"usage: netplay_timeline_tests CORE\n"); return 2; }
    try { run(QString::fromLocal8Bit(argv[1])); }
    catch(const std::exception& error) { std::fprintf(stderr,"FAIL: %s\n",error.what()); return 1; }
    std::puts("netplay timeline tests passed");
}
