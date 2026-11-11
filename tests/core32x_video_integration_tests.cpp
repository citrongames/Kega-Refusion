#include "core/LibretroHost.h"
#include "core/Sega32xBios.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QTemporaryDir>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
void require(bool ok, const QString& error) {
    if (!ok) throw std::runtime_error(error.toStdString());
}
void putLong(QByteArray& bytes, int offset, quint32 value) {
    for (int i = 0; i < 4; ++i) bytes[offset+i] = char(value >> (24-8*i));
}
enum class Change { None, Mode, Priority, Shift };
constexpr unsigned adapter = 0xa15100, videoMode = 0xa15180, shift = 0xa15182;
constexpr unsigned frameSelect = 0xa1518a, palette = 0xa15200, framebuffer = 0x840000;
constexpr unsigned irqHandler = 0x4000, logBase = 0xff1000;
constexpr int framesPerCase = 12;

// Original 68000 instructions/data only. No game or BIOS bytes enter the ROM.
// SH2 remain reset (ADEN=1,NRES=0,FM=0); the real core/BIOS attachment supplies
// bus mapping, MD rendering and 32X composition, without SH2 bootstrap code.
QByteArray makeRom(bool h40, bool blankControl, Change change, bool afterLatch) {
    QByteArray rom(64*1024, char(0xff));
    putLong(rom,0,0x00fffff0); putLong(rom,4,0x200);
    rom.replace(0x100,16,"SEGA 32X        ");
    rom.replace(0x120,16,"KEGA VIDEO TEST ");
    rom.replace(0x150,16,"ORIGINAL FIXTURE");
    rom.replace(0x180,2,"GM");
    putLong(rom,0x1a0,0); putLong(rom,0x1a4,quint32(rom.size()-1));
    putLong(rom,0x1a8,0x00ff0000); putLong(rom,0x1ac,0x00ffffff);
    rom.replace(0x1f0,1,"U");
    int pc=0x200;
    const auto word=[&](quint16 v) {
        require(pc+2<=rom.size(),QStringLiteral("Fixture ROM overflow."));
        rom[pc++]=char(v>>8); rom[pc++]=char(v);
    };
    const auto address=[&](quint32 a) { word(quint16(a>>16)); word(quint16(a)); };
    const auto moveWord=[&](quint16 v,quint32 a) { word(0x33fc);word(v);address(a); };
    const auto moveLong=[&](quint32 v,quint32 a) { word(0x23fc);address(v);address(a); };
    const auto mdReg=[&](quint16 v) { moveWord(v,0xc00004); };
    const auto branch=[&](quint16 opcode,int target) {
        const int at=pc, delta=target-(at+2);
        require(delta>=-128 && delta<=127 && delta!=0,QStringLiteral("Fixture branch out of range."));
        word(quint16(opcode|(delta&255)));
    };
    word(0x46fc);word(0x2700); // Mask interrupts during setup.
    moveWord(1,adapter);       // M68K owns video; never release either SH2.
    moveWord(0,videoMode);moveWord(0,shift);
    mdReg(0x8004);mdReg(0x8104);mdReg(0x8700);mdReg(0x8f02);
    mdReg(0x8230);mdReg(0x8407);mdReg(0x8568);mdReg(0x8d3c);mdReg(0x9001);
    mdReg(h40?0x8c81:0x8c00);
    moveLong(0xc0000000,0xc00004); // CRAM: black, MD red, MD blue.
    moveWord(0,0xc00000);moveWord(0x000e,0xc00000);moveWord(0x0e00,0xc00000);
    moveLong(0x40000000,0xc00004); // Tile zero: R,B,transparent,transparent twice.
    for (int y=0;y<8;++y)moveLong(0x12001200,0xc00000);
    moveWord(0x83e0,palette+2); // 32X high-priority green marker.
    moveWord(0x03ff,palette+4); // 32X low-priority yellow background.
    // All lines reuse one 322-pixel packed row, including the SFT lookahead.
    // Bank 0 initially displays; only its hidden peer is written.
    for (unsigned y=0;y<256;++y)moveWord(0x0200,framebuffer+y*2);
    const auto index=[](unsigned x) { return (x%64<8 || x>=312)?1u:2u; };
    for (unsigned x=0;x<322;x+=2)
        moveWord(quint16((index(x)<<8)|index(x+1)),framebuffer+0x400+x);
    moveWord(1,frameSelect); // Mode blank: FS is immediate; filled bank becomes visible.
    moveWord(blankControl?0:1,videoMode);
    moveWord(0x32f0,0xff0002); // Setup-complete sentinel in work RAM.
    if (change==Change::None) {
        mdReg(0x8144);
        word(0x60fe);
    } else {
        moveLong(irqHandler,0x70); // Writable 32X HINT vector, code outside G shadow.
        word(0x43f9);address(0xc00004); // A1 = VDP status (short active-loop instructions).
        word(0x207c);address(logBase);  // A0 = bounded per-frame IRQ log.
        moveWord(0,0xff0000);
        mdReg(0x8a3f);mdReg(0x8014);mdReg(0x8144); // HINT each 64 active lines.
        word(0x46fc);word(0x2300); // Enable level-4 HINT, no VINT is enabled.
        const int waitBlank=pc;
        word(0x3011);word(0x0240);word(8);branch(0x6700,waitBlank);
        moveWord(0,0xff0000);word(0x207c);address(logBase);
        moveWord(change==Change::Shift?0:1,change==Change::Shift?shift:videoMode);
        const int waitActive=pc;
        word(0x3011);word(0x0240);word(8);branch(0x6600,waitActive);
        branch(0x6000,waitBlank);
        require(pc<int(irqHandler),QStringLiteral("Fixture setup overlaps its interrupt handler."));
        pc=int(irqHandler);
        word(0x3f00); // MOVE.W D0,-(SP).
        if (afterLatch)for(int i=0;i<10;++i)word(0x4e71);
        word(0x3039);address(0xc00008); // Sample HV immediately before the MMIO instruction.
        moveWord(change==Change::Priority?0x81:change==Change::Shift?1:0,
                 change==Change::Shift?shift:videoMode);
        word(0x30c0); // MOVE.W D0,(A0)+; work RAM holds only the current frame's 3 samples.
        word(0x5279);address(0xff0000); // ADDQ.W #1,IRQ count.
        word(0x301f);word(0x4e73); // Restore D0, RTE.
    }
    quint16 sum=0;
    for(int at=0x200;at<rom.size();at+=2)
        sum=quint16(sum+(quint16(quint8(rom[at]))<<8)+quint8(rom[at+1]));
    rom[0x18e]=char(sum>>8);rom[0x18f]=char(sum);
    return rom;
}
void writeRom(const QString& path,const QByteArray& rom) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly|QIODevice::NewOnly),file.errorString());
    require(file.write(rom)==rom.size(),QStringLiteral("Could not write original fixture ROM."));
}
QByteArray videoHash(const QImage& image) {
    const QImage rgba=image.convertToFormat(QImage::Format_RGBA8888);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for(int y=0;y<rgba.height();++y)
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(rgba.constScanLine(y)),qsizetype(rgba.width())*4));
    return hash.result().toHex();
}
bool green(QRgb p) { return qGreen(p)>220 && qRed(p)<32 && qBlue(p)<32; }
bool yellow(QRgb p) { return qRed(p)>220 && qGreen(p)>220 && qBlue(p)<32; }
bool red(QRgb p) { return qRed(p)>160 && qGreen(p)<32 && qBlue(p)<32; }
bool blue(QRgb p) { return qBlue(p)>160 && qGreen(p)<32 && qRed(p)<32; }
bool black(QRgb p) { return qRed(p)<8 && qGreen(p)<8 && qBlue(p)<8; }
void requireH40Row(const QImage& image,int y,bool blank) {
    require(image.width()==320 && y>=0 && y<image.height(),QStringLiteral("Unexpected H40 fixture geometry."));
    for(int x=0;x<320;++x) {
        const auto p=image.pixel(x,y);
        const bool marker=x%64<8 || x>=312;
        const bool ok=!blank && marker?green(p):
            x%4==0?red(p):x%4==1?blue(p):blank?black(p):yellow(p);
        require(ok,QStringLiteral("H40 marker mismatch at (%1,%2), pixel #%3.")
                    .arg(x).arg(y).arg(quint32(p),8,16,QLatin1Char('0')));
    }
}
void requireH32Row(const QImage& image,int y) {
    require(image.width()==320 && y>=0 && y<image.height(),QStringLiteral("Unexpected H32 fixture geometry."));
    for(int x=0;x<320;++x) {
        // Agreed H32 integration model: center sample in the common quarter-dot
        // grid; MD clocks span 5 ticks and 32X clocks 4, with a 13-tick delay.
        const int sample=4*x+2, md=sample/5;
        const int x32=sample>=13?(sample-13)/4:-1;
        const bool marker=x32>=0 && (x32%64<8 || x32>=312);
        const auto pixel=image.pixel(x,y);
        const bool ok=marker?green(pixel):md%4==0?red(pixel):md%4==1?blue(pixel):
                      x32<0?black(pixel):yellow(pixel);
        require(ok,QStringLiteral("H32 phase mismatch at (%1,%2): MD=%3 32X=%4 pixel #%5.")
                    .arg(x).arg(y).arg(md).arg(x32).arg(quint32(pixel),8,16,QLatin1Char('0')));
    }
}
struct Phase { unsigned first,last; };
Phase h40SamplePhase(unsigned hc) {
    // Counter facts from pinned GPX hvc.h cycle2hc40, relative to HINT origin.
    // This is an integration-clock oracle, not a claim of measured hardware phase.
    static constexpr unsigned starts[]={341,361,381,399,419,439,459,478,497,517,537,557,575,595,615,635};
    if(hc>=0xe8 && hc<0xf7)return {starts[hc-0xe8],starts[hc-0xe8+1]-1};
    if(hc==0xf7)return {635,651};
    if(hc>=0xf8)return {652+(hc-0xf8)*16,667+(hc-0xf8)*16};
    if(hc<=12)return {780+hc*16,795+hc*16};
    throw std::runtime_error("IRQ HV sample is outside the supported timing window.");
}
quint16 ramWord(const void* ram,unsigned offset) {
    // GPX work RAM stores numeric host-endian words (both supported targets are LE).
    quint16 value;std::memcpy(&value,static_cast<const char*>(ram)+offset,sizeof(value));return value;
}
QJsonObject checkTiming(const QImage& image,const void* ram,Change change,bool late) {
    const unsigned count=ramWord(ram,0);
    require(count==3,QStringLiteral("Expected 3 HINT samples in the final frame, got %1.").arg(count));
    QJsonArray writes;
    unsigned firstLine=256;
    for(unsigned i=0;i<count;++i) {
        const unsigned hv=ramWord(ram,0x1000+i*2), y=hv>>8;
        const Phase sample=h40SamplePhase(hv&255);
        // MOVE.W abs.l,D0 costs 16 CPU clocks. One GPX ROM-refresh penalty
        // of 2 CPU clocks can occur before the following immediate MMIO write.
        const Phase write{sample.first+112,sample.last+126};
        require(late?(write.first>772 && write.last<988):write.last<772,
                QStringLiteral("MMIO phase [%1,%2] does not bracket H40 latch/decode 772/988.")
                    .arg(write.first).arg(write.last));
        firstLine=std::min(firstLine,y);
        writes.append(QJsonObject{{"line",int(y)},{"hv",int(hv)},
                                  {"write_mclk_min",int(write.first)},{"write_mclk_max",int(write.last)}});
    }
    require(firstLine>0 && firstLine+1<unsigned(image.height()),QStringLiteral("HINT line is outside active video."));
    const int y=int(firstLine);
    const auto newPixel=[&](int row) {
        if(change==Change::Mode)return black(image.pixel(2,row));
        if(change==Change::Priority)return red(image.pixel(0,row)) && yellow(image.pixel(8,row));
        return yellow(image.pixel(7,row));
    };
    requireH40Row(image,y-1,false);
    require(newPixel(y+1),QStringLiteral("Register write was not visible on the following line."));
    require(late?green(image.pixel(change==Change::Shift?7:change==Change::Mode?2:0,y)):newPixel(y),
            QStringLiteral("Current line used the wrong side of the 32X latch."));
    return {{"latch_mclk",772},{"decode_mclk",988},{"writes",writes},{"current_line",y},
            {"after_latch",late},{"row_checks",true}};
}
}

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    const auto args=app.arguments();
    if(args.size()<3 || args.size()>5) {
        std::fprintf(stderr,"Usage: core32x_video_integration_tests CORE BIOS_SOURCE [OUTPUT_DIR] [--timing]\n");return 2;
    }
    bool timing=false;QString output;
    for(int i=3;i<args.size();++i) {
        if(args[i]==QLatin1String("--timing") && !timing)timing=true;
        else if(!args[i].startsWith(QLatin1String("--")) && output.isEmpty())output=QFileInfo(args[i]).absoluteFilePath();
        else {std::fprintf(stderr,"Invalid or duplicate fixture option.\n");return 2;}
    }
    try {
        QTemporaryDir temp;
        require(temp.isValid(),QStringLiteral("Could not create isolated fixture directory."));
        QString error;
        Sega32xBios::importDirectory(QFileInfo(args[2]).absoluteFilePath(),temp.path(),error);
        const bool biosReady=Sega32xBios::validate(temp.path(),error);
        require(biosReady,QStringLiteral("32X BIOS import failed: %1").arg(error));
        const QString core=QFileInfo(args[1]).absoluteFilePath();
        LibretroHost host(core,temp.path());
        require(host.setRegion(2,error),error);require(host.setBlarggFilter(0,error),error);
        QLibrary diagnostics(core);
        using Memory=void* (*)(unsigned);using MemorySize=size_t (*)(unsigned);
        const auto memory=reinterpret_cast<Memory>(diagnostics.resolve("retro_get_memory_data"));
        const auto memorySize=reinterpret_cast<MemorySize>(diagnostics.resolve("retro_get_memory_size"));
        require(memory && memorySize,QStringLiteral("Core memory inspection API is unavailable."));
        QJsonArray cases;
        const auto run=[&](const QString& name,bool h40,bool blank,Change change,bool late) {
            const QString path=temp.filePath(name+QStringLiteral(".32x"));
            writeRom(path,makeRom(h40,blank,change,late));
            const bool loaded=host.load(path,error);
            require(loaded,QStringLiteral("Fixture %1 load failed: %2").arg(name,error));
            for(int i=0;i<framesPerCase;++i)host.runFrame(true,true);
            require(memorySize(RETRO_MEMORY_SYSTEM_RAM)>=0x1200 && memory(RETRO_MEMORY_SYSTEM_RAM),
                    QStringLiteral("Core did not expose MD work RAM."));
            const void* ram=memory(RETRO_MEMORY_SYSTEM_RAM);
            require(ramWord(ram,2)==0x32f0,QStringLiteral("Original fixture program did not finish setup."));
            const QImage image=host.frame();
            require(!image.isNull() && image.width()<=640 && image.height()>=224 && image.height()<=480,
                    QStringLiteral("Fixture produced invalid video."));
            if(change==Change::None) {
                const QByteArray beforeCapture=host.serialize();
                require(host.setRecordingVideoEnabled(true),QStringLiteral("Native 32X capture enable failed."));
                require(host.serialize()==beforeCapture,QStringLiteral("Native capture changed 32X machine state."));
                for(const int filter:{1,2,3,4}) {
                    require(host.setBlarggFilter(filter,error),error);
                    for(int i=0;i<4;++i)host.runFrame();
                    const QImage presented=host.frame();
                    require(presented.width()>image.width(),QStringLiteral("32X diagnostic filter did not expand video."));
                    require(host.recordingFrame()==image,QStringLiteral("Native 32X capture retained filtered pixels or lost mixed detail."));
                    require(host.frame()==presented,QStringLiteral("Reading native 32X capture changed presentation."));
                    if (h40 && !blank && filter==3) {
                        require(!host.rewindPreviewNeedsPair(),QStringLiteral("Progressive attached 32X requested two-field replay."));
                        const QByteArray predecessor=host.captureRewindState();
                        host.runFrame();
                        const QImage landed=host.frame();const double landedAspect=host.aspectRatio();
                        const QByteArray target=host.captureRewindState();
                        // Control: the existing picture-based rewind restores this
                        // same canonical checkpoint before its next normal frame.
                        require(host.restoreRewindState(target),QStringLiteral("32X control restore failed."));
                        host.restoreFramePreview(landed,landedAspect);
                        host.runFrame();
                        const QImage next=host.frame();const QByteArray nextAudio=host.audio();
                        const QByteArray nextState=host.captureRewindState();
                        require(host.restoreRewindState(predecessor),QStringLiteral("32X replay predecessor restore failed."));
                        host.runRewindPreviewFrame();
                        require(host.frame()==landed && host.aspectRatio()==landedAspect && host.audio().isEmpty(),
                                QStringLiteral("32X S-Video replay did not regenerate the exact landed picture silently."));
                        require(host.restoreRewindState(target) && host.captureRewindState()==target,
                                QStringLiteral("32X replay did not restore its canonical post-frame state."));
                        host.runFrame();
                        require(host.frame()==next,QStringLiteral("32X replay changed subsequent video."));
                        require(host.audio()==nextAudio,QStringLiteral("32X replay changed subsequent PCM."));
                        require(host.captureRewindState()==nextState,QStringLiteral("32X replay changed subsequent native state."));
                    }
                }
                require(host.setRecordingVideoEnabled(false),QStringLiteral("Native 32X capture stop failed."));
                require(host.setBlarggFilter(0,error),error);host.runFrame();
            }
            QJsonObject result{{"case",name},{"frames",framesPerCase},{"width",image.width()},{"height",image.height()},
                               {"video_sha256_rgba",QString::fromLatin1(videoHash(image))}};
            if(!output.isEmpty()) {
                require(QDir().mkpath(output),QStringLiteral("Could not create fixture image directory."));
                const QString png=QDir(output).filePath(name+QStringLiteral(".png"));
                require(image.save(png,"PNG"),QStringLiteral("Could not save fixture PNG."));result["output_png"]=png;
            }
            if(change!=Change::None)result["timing"]=checkTiming(image,ram,change,late);
            else if(h40){requireH40Row(image,32,blank);requireH40Row(image,200,blank);result["marker_checks"]=true;}
            else {
                requireH32Row(image,32);requireH32Row(image,200);
                result["h32_phase_checks"]=true;result["h32_quarter_tick_offset"]=13;
            }
            cases.append(result);
        };
        run(QStringLiteral("h40-md-control"),true,true,Change::None,false);
        run(QStringLiteral("h40-32x-markers"),true,false,Change::None,false);
        run(QStringLiteral("h32-32x-markers"),false,false,Change::None,false);
        if(timing)for(const auto change:{Change::Mode,Change::Priority,Change::Shift}) {
            const QString name=change==Change::Mode?QStringLiteral("mode"):change==Change::Priority?QStringLiteral("pri"):QStringLiteral("sft");
            run(name+QStringLiteral("-before-latch"),true,false,change,false);
            run(name+QStringLiteral("-after-latch"),true,false,change,true);
        }
        const auto json=QJsonDocument(QJsonObject{{"original_fixture",true},{"sh2_held_reset",true},
            {"isolated_temporary_bios",true},{"cases",cases}}).toJson(QJsonDocument::Compact);
        std::printf("%s\n",json.constData());return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"32X video fixture failed: %s\n",error.what());return 1;
    }
}
