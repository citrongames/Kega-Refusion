#include "core/LibretroHost.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QtEndian>
#include <cstdio>
#include <stdexcept>

namespace {
void check(bool ok,const QString& message) {
    if(!ok)throw std::runtime_error(message.toStdString());
}
QString saveRom(const QString& path,const QByteArray& bytes) {
    QFile file(path);check(file.open(QIODevice::WriteOnly),"fixture open");
    check(file.write(bytes)==bytes.size(),"fixture write");return path;
}
QByteArray mdRom() {
    QByteArray data(65536,char(0));
    qToBigEndian<quint32>(0xfffff0,data.data());
    qToBigEndian<quint32>(0x200,data.data()+4);
    data.replace(0x100,16,"SEGA GENESIS    ");
    data[0x1f0]='U';
    qToBigEndian<quint32>(65535,data.data()+0x1a4);
    unsigned pc=0x200;
    const auto w=[&](quint16 word){qToBigEndian(word,data.data()+pc);pc+=2;};
    w(0x46fc);w(0x2700); // supervisor, mask IRQ
    w(0x33fc);w(0x8004);w(0xc0);w(4);
    w(0x33fc);w(0x8144);w(0xc0);w(4);
    w(0x5279);w(0xff);w(0x20); // count in work RAM
    w(0x60f8);
    return data;
}
DebugTraceCommand start(unsigned cpu=0,unsigned filters=KR_TRACE_INSTRUCTION) {
    DebugTraceCommand command;command.kind=DebugTraceCommand::Start;
    command.config.cpu=cpu;command.config.filters=filters;command.config.memoryMiB=1;
    return command;
}
bool boundary(const DebugTraceSnapshot& snapshot,unsigned reason) {
    for(const auto& row:snapshot.rows)
        if(row.record.native.kind==KR_TRACE_DISCONTINUITY && row.record.native.value==reason)return true;
    return false;
}
}
int main(int argc,char**argv) {
    QCoreApplication app(argc,argv);
    if(argc!=2)return 2;
    try {
        QTemporaryDir dir;check(dir.isValid(),"temp");
        const auto path=saveRom(dir.path()+"/trace.md",mdRom());
        QString error;
        QByteArray expected;
        QImage picture;QByteArray sound;
        {
            LibretroHost host(argv[1],dir.path()+"/baseline");
            check(host.load(path,error),error);host.runFrame();
            expected=host.serialize();picture=host.frame();sound=host.audio();
        }
        {
            LibretroHost host(argv[1],dir.path()+"/capture");
            check(host.load(path,error),error);
            check(host.traceCommand(start(),error),error);
            host.runFrame();
            check(host.serialize()==expected && host.frame()==picture && host.audio()==sound,
                  "capture changes machine/video/audio");
            auto snapshot=host.traceSnapshot();
            check(snapshot.available && snapshot.active && snapshot.rows.size()==256,"bounded capture");
            quint64 sequence=0;
            for(const auto& row:snapshot.rows) {
                check(row.record.sequence>sequence && row.record.frame==1,"sequence/frame");
                check(row.record.native.cpu==KR_DEBUG_M68K && !row.instruction.isEmpty(),"captured-byte decode");
                sequence=row.record.sequence;
            }
            DebugTraceCommand clear;clear.kind=DebugTraceCommand::Clear;
            check(host.traceCommand(clear,error),error);
            host.runFrame();
            check(host.traceSnapshot().rows.front().record.sequence>sequence,"clear must preserve sequence");
            host.reset();
            check(boundary(host.traceSnapshot(),KR_TRACE_RESET),"reset marker");
            host.runFrame();
            check(host.traceActive(),"reset keeps tracer");
            check(host.unserialize(expected),"restore fixture");
            check(boundary(host.traceSnapshot(),KR_TRACE_RESTORE),"restore marker");
            check(host.hardReset(error),error);
            check(boundary(host.traceSnapshot(),KR_TRACE_POWER),"power marker");
            host.runFrame();check(host.traceActive(),"power rebind");
            host.unload();snapshot=host.traceSnapshot();
            check(!snapshot.active && !snapshot.rows.isEmpty() && boundary(snapshot,KR_TRACE_UNLOAD),
                  "unload detach and retain");
            check(!snapshot.rows.front().instruction.isEmpty(),"cold decode after unload");
        }
        // A/B namespaces are host-only. Selecting B must not accidentally arm
        // instruction tracing on A or be rejected for its zero CPU mask.
        {
            QByteArray gg(32768,char(0));
            gg[0]=char(0xc3);gg[1]=0;gg[2]=0; // JP 0
            gg.replace(0x7ff0,8,"TMR SEGA");gg[0x7fff]=char(0x6c);
            const auto ggPath=saveRom(dir.path()+"/trace.gg",gg);
            LibretroHost host(argv[1],dir.path()+"/link");
            check(host.load(ggPath,error),error);
            check(host.setLinkCableEnabled(true,error),error);
            check(host.traceCommand(start(0x100|KR_DEBUG_Z80),error),error);
            host.runFrame();
            auto snapshot=host.traceSnapshot();
            check(snapshot.targets.size()==2 && !snapshot.rows.isEmpty(),"link trace targets");
            for(const auto& row:snapshot.rows)
                check(row.record.instance==1 && row.record.native.cpu==KR_DEBUG_Z80,"selected B only");
            check(host.setLinkCableEnabled(false,error),error);
            check(!host.traceActive(),"topology stops previous capture");
        }
        std::puts("debug_trace_host_tests: PASS");return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"%s\n",e.what());return 1;}
}
