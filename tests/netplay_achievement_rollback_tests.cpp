#include "core/LibretroHost.h"
#include "net/NetRollbackSession.h"
#include "net/NetplayTimeline.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void check(bool value,const char* why) { if (!value) throw std::runtime_error(why); }
void reply(const QJsonObject& value) {
    const QByteArray line=QJsonDocument(value).toJson(QJsonDocument::Compact)+'\n';
    std::fwrite(line.constData(),1,size_t(line.size()),stdout);
    std::fflush(stdout);
}
QByteArray fixture() {
    QByteArray rom(64*1024,char(0xff));
    qToBigEndian(quint32(0x00ff0000),rom.data());
    qToBigEndian(quint32(0x00000200),rom.data()+4);
    rom.replace(0x100,16,"SEGA MEGA DRIVE ");
    rom.replace(0x180,2,"GM");
    rom.replace(0x1f0,3,"USA");
    int pc=0x200;
    const auto word=[&](quint16 value) {
        rom[pc++]=char(value>>8); rom[pc++]=char(value);
    };
    const auto address=[&](quint32 value) {
        word(quint16(value>>16)); word(quint16(value));
    };
    const auto putByte=[&](quint8 value,quint32 target) {
        word(0x13fc); word(value); address(target);
    };
    word(0x46fc); word(0x2700); // Mask interrupts.
    putByte(0x40,0xa1000b);    // Port 2 TH output.
    putByte(0x40,0xa10005);    // TH high: B is active-low bit 4.
    word(0x33fc); word(0); address(0xff0000);
    const int loop=pc;
    word(0x1039); address(0xa10005); // MOVE.B P2,D0
    word(0x0200); word(0x0010);      // ANDI.B #$10,D0
    word(0x0a00); word(0x0010);      // EORI.B #$10,D0
    word(0x13c0); address(0xff0000); // Store 0x10 only when B is pressed.
    word(0x4ef9); address(quint32(loop));
    return rom;
}
int marker(const QByteArray& snapshot) {
    uint8_t bytes[2]{};
    if (LibretroHost::readAchievementSnapshot(snapshot,0,bytes,2)!=2) return -1;
    return ((bytes[0]|bytes[1])&0x10) ? 1 : 0;
}
bool pressed(quint64 frame) {
    return (frame>=20 && frame<27) || (frame>=38 && frame<43) ||
           (frame>=58 && frame<66) || (frame>=87 && frame<95);
}
int worker(const QString& core,const QString& rom,const QString& data,quint64 id) {
    LibretroHost host(core,data);
    QString error;
    check(host.load(rom,error),qPrintable(error));
    host.setControllerConnected(1,true);
    NetRollbackSession net;
    QJsonArray roster{QJsonObject{{"id","0"},{"role",1}},
                      QJsonObject{{"id","1"},{"role",2}}};
    QJsonArray outgoing;
    const auto send=[&](quint64 peer,const QByteArray& packet) {
        check(outgoing.size()<256,"outgoing packet queue overflow");
        outgoing.append(QJsonObject{{"to",QString::number(peer)},
                                    {"data",QString::fromLatin1(packet.toBase64())}});
    };
    check(net.start(host,1,roster,id,send,error),qPrintable(error));
    net.setAchievementsEnabled(true);
    reply(QJsonObject{{"ready",true}});
    std::string line;
    while (std::getline(std::cin,line)) {
        const auto command=QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        for (const auto& packet:command.value("packets").toArray()) {
            const auto item=packet.toObject();
            net.receive(item.value("from").toString().toULongLong(),
                        QByteArray::fromBase64(item.value("data").toString().toLatin1()));
        }
        check(net.idle(error),qPrintable(error));
        QJsonObject result;
        MovieInput local,merged;
        if (id==1 && pressed(net.position())) local.buttons[0]=1; // P2 B.
        const auto decision=net.beforeFrame(local,merged,error);
        check(decision!=NetRollbackSession::Failed,qPrintable(error));
        if (decision==NetRollbackSession::Run) {
            const quint64 frame=net.position();
            NetplayTimeline::apply(host,merged);
            host.runFrame(false,false);
            check(net.afterFrame(error),qPrintable(error));
            QByteArray live;
            check(host.captureAchievementMemory(live),"live RAM capture failed");
            result.insert("frame",int(frame));
            result.insert("live",marker(live));
        }
        QJsonArray confirmed;
        net.processConfirmedAchievements([&](const QByteArray& memory) {
            confirmed.append(marker(memory));
        });
        result.insert("confirmed",confirmed);
        result.insert("out",outgoing);
        outgoing=QJsonArray{};
        reply(result);
    }
    return 0;
}
QJsonObject readReply(QProcess& process) {
    QElapsedTimer timer; timer.start();
    while (!process.canReadLine()) {
        check(process.state()==QProcess::Running,"worker exited early");
        check(timer.elapsed()<10000,"worker reply timeout");
        process.waitForReadyRead(100);
    }
    QJsonParseError error;
    const auto doc=QJsonDocument::fromJson(process.readLine(),&error);
    check(error.error==QJsonParseError::NoError && doc.isObject(),"invalid worker reply");
    return doc.object();
}
struct Packet { int to,from,due; QByteArray data; };
void run(const QString& executable,const QString& core) {
    QTemporaryDir temp;
    check(temp.isValid(),"temporary directory failed");
    const QString rom=temp.filePath("probe.md");
    QFile romFile(rom);
    const QByteArray contents=fixture();
    check(romFile.open(QIODevice::WriteOnly) &&
          romFile.write(contents)==contents.size(),"probe ROM write failed");
    romFile.close();
    std::array<QProcess,2> workers;
    struct Cleanup {
        std::array<QProcess,2>& workers;
        ~Cleanup() {
            for (auto& worker:workers) if (worker.state()!=QProcess::NotRunning) {
                worker.closeWriteChannel();
                if (!worker.waitForFinished(2000)) { worker.kill(); worker.waitForFinished(); }
            }
        }
    } cleanup{workers};
    for (int i=0;i<2;++i) {
        const QString copy=temp.filePath(QStringLiteral("core%1.so").arg(i));
        check(QFile::copy(core,copy),"isolated core copy failed");
        workers[i].setProcessChannelMode(QProcess::ForwardedErrorChannel);
        workers[i].start(executable,{"--worker",copy,rom,
                         temp.filePath(QStringLiteral("data%1").arg(i)),QString::number(i)});
        check(workers[i].waitForStarted(10000),"worker failed to start");
        check(readReply(workers[i]).value("ready").toBool(),"worker not ready");
    }
    std::deque<Packet> delayed;
    std::array<std::vector<int>,2> live,confirmed;
    constexpr int ticks=220;
    for (int tick=0;tick<ticks;++tick) {
        std::array<QJsonArray,2> inbox;
        for (auto it=delayed.begin();it!=delayed.end();) {
            if (it->due>tick) {++it;continue;}
            inbox[it->to].append(QJsonObject{
                {"from",QString::number(it->from)},
                {"data",QString::fromLatin1(it->data.toBase64())}});
            it=delayed.erase(it);
        }
        for (int i=0;i<2;++i) {
            const auto command=QJsonDocument(QJsonObject{{"packets",inbox[i]}})
                                   .toJson(QJsonDocument::Compact)+'\n';
            check(workers[i].write(command)==command.size(),"worker command failed");
        }
        for (int i=0;i<2;++i) {
            const auto result=readReply(workers[i]);
            if (result.contains("frame")) {
                check(result.value("frame").toInt()==int(live[i].size()),"live frame gap");
                live[i].push_back(result.value("live").toInt(-1));
            }
            for (const auto& value:result.value("confirmed").toArray())
                confirmed[i].push_back(value.toInt(-1));
            for (const auto& value:result.value("out").toArray()) {
                const auto packet=value.toObject();
                const int to=packet.value("to").toString().toInt();
                check(to==1-i && delayed.size()<256,"packet relay overflow");
                delayed.push_back({to,i,tick+4,
                    QByteArray::fromBase64(packet.value("data").toString().toLatin1())});
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    const size_t count=std::min(confirmed[0].size(),confirmed[1].size());
    check(count>=80 && count<=live[0].size() && count<=live[1].size(),
          "insufficient confirmed frames");
    int predictedWrong=0,corrected=0,pressedFrames=0;
    for (size_t frame=0;frame<count;++frame) {
        check(confirmed[0][frame]>=0 && confirmed[1][frame]>=0,
              "missing confirmed memory snapshot");
        check(confirmed[0][frame]==confirmed[1][frame] &&
              confirmed[0][frame]==live[1][frame],
              "confirmed callback saw predicted or incorrect RAM");
        if (confirmed[0][frame]) ++pressedFrames;
        if (!live[0][frame] && live[1][frame]) {
            ++predictedWrong;
            if (confirmed[0][frame]) ++corrected;
        }
    }
    check(pressedFrames>=8 && predictedWrong>0 && corrected>0,
          "test did not exercise rollback correction of P2 RAM");
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        if (argc==6 && std::strcmp(argv[1],"--worker")==0)
            return worker(QString::fromLocal8Bit(argv[2]),QString::fromLocal8Bit(argv[3]),
                          QString::fromLocal8Bit(argv[4]),quint64(std::strtoull(argv[5],nullptr,10)));
        check(argc==2,"usage: netplay_achievement_rollback_tests <core>");
        run(QCoreApplication::applicationFilePath(),QString::fromLocal8Bit(argv[1]));
        std::puts("netplay_achievement_rollback_tests PASS");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr,"netplay_achievement_rollback_tests FAIL: %s\n",e.what());
        return 1;
    }
}
