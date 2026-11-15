#include "core/LibretroHost.h"
#include "core/VideoFrameGeometry.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
#include <stdexcept>
#include <cmath>

namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void save(const QString& path,const QByteArray& bytes){QFile f(path);check(f.open(QIODevice::WriteOnly)&&f.write(bytes)==bytes.size(),"fixture write");}
void longAt(QByteArray& b,int at,quint32 v){for(int i=0;i<4;++i)b[at+i]=char(v>>(24-i*8));}
// Original 68000 fixture: independently colored A, B, window and two overlapping
// sprites, plus PSG tone. No external ROM/BIOS bytes or private save data.
QByteArray mdRom(bool firecore=false,bool bitmap=false){
    QByteArray b(65536,char(0));longAt(b,0,0xfffff0);longAt(b,4,0x200);
    b.replace(0x100,16,"SEGA GENESIS    ");b.replace(0x120,16,"DEBUG MASK TEST ");
    longAt(b,0x1a4,b.size()-1);longAt(b,0x1a8,0xff0000);longAt(b,0x1ac,0xffffff);b[0x1f0]='U';
    int pc=0x200;auto w=[&](quint16 v){b[pc++]=char(v>>8);b[pc++]=char(v);};
    auto addr=[&](quint32 a){w(a>>16);w(a);};
    auto mw=[&](quint16 v,quint32 a){w(0x33fc);w(v);addr(a);};
    auto ml=[&](quint32 v,quint32 a){w(0x23fc);addr(v);addr(a);};
    auto reg=[&](unsigned i,unsigned v){mw(0x8000|(i<<8)|v,0xc00004);};
    auto vram=[&](unsigned a){ml(0x40000000|((a&0x3fff)<<16)|(a>>14),0xc00004);};
    auto fill=[&](unsigned a,unsigned count,quint16 value){vram(a);w(0x303c);w(count-1);int loop=pc;mw(value,0xc00000);w(0x51c8);w(quint16(loop-pc));};
    w(0x46fc);w(0x2700);reg(0,4);reg(1,4);reg(15,2);reg(12,0);
    if(firecore)reg(24,bitmap?0xc0:0x80);
    reg(2,firecore?0x20:0x30);reg(3,0x2c);reg(4,firecore?5:7);
    reg(5,firecore?0x60:0x68);reg(7,0);reg(13,0x3c);reg(16,0);reg(17,0);reg(18,1);
    fill(0,32768,0);
    for(unsigned tile=1;tile<=4;++tile)
        fill(tile*(firecore?48:32),firecore?24:16,firecore?0xffff:quint16(tile*0x1111));
    ml(0xc0000000,0xc00004);
    if(firecore){
        for(unsigned i=0;i<512;++i){
            const quint32 rgb=i==63?0xff0000:i==127?0x00ff00:i==191?0x0000ff:i==255?0xffffff:0;
            mw(quint16((rgb>>16)|(rgb&0xff00)),0xc00000);
            mw(quint16(rgb),0xc00000);
        }
        fill(0x8000,1024,1);fill(0xa000,1024,0x2002);fill(0xb000,1024,0x4003);
        if(bitmap)fill(0x4e00,1280,0xffff);
        vram(0x12000);for(auto v:{0xa0,1,0x6004,0xa0,0xa0,0,0x6004,0xa0})mw(v,0xc00000);
    }else{
        for(auto color:{0,0x000e,0x00e0,0x0e00,0x0eee})mw(color,0xc00000);
        fill(0xc000,1024,1);fill(0xe000,1024,2);fill(0xb000,1024,3);
        vram(0xd000);for(auto v:{0xa0,1,4,0xa0,0xa0,0,4,0xa0})mw(v,0xc00000);
    }
    // PSG tone 1, other channels silent after reset.
    for(auto v:{0x80,0x10,0x90}){w(0x13fc);w(v);addr(0xc00011);}
    reg(1,0x44);w(0x60fe);return b;
}
QByteArray smsRom(){
    QByteArray b(32768,char(0));int pc=0;
    auto byte=[&](unsigned v){b[pc++]=char(v);};
    auto out=[&](unsigned v,unsigned port){byte(0x3e);byte(v);byte(0xd3);byte(port);};
    byte(0xf3);byte(0x31);byte(0xf0);byte(0xdf);
    out(0x80,0x7f);out(0x10,0x7f);out(0x90,0x7f);
    out(4,0xbf);out(0x80,0xbf);out(0x40,0xbf);out(0x81,0xbf);
    byte(0x18);byte(0xfe);
    b.replace(0x7ff0,8,"TMR SEGA");b[0x7fff]=char(0x4c);return b;
}
void frames(LibretroHost& host,int n){for(int i=0;i<n;++i)host.runFrame();}
double energy(LibretroHost& host) {
    double sum=0;qsizetype samples=0;
    for(int frame=0;frame<8;++frame){host.runFrame();if(frame<3)continue;
        const auto& pcm=host.audio();const auto* data=reinterpret_cast<const qint16*>(pcm.constData());
        for(qsizetype i=0;i<pcm.size()/2;++i){sum+=std::abs(int(data[i]));++samples;}}
    return samples?sum/double(samples):0;
}
bool audible(const QByteArray& pcm){const auto* s=reinterpret_cast<const qint16*>(pcm.constData());for(qsizetype i=0;i<pcm.size()/2;++i)if(std::abs(int(s[i]))>8)return true;return false;}
void firecoreVideo(const QString& core){
    QTemporaryDir dir;check(dir.isValid(),"temp directory");QString error;
    const auto path=dir.filePath("original.atf");save(path,mdRom(true));
    LibretroHost h(core,dir.path());check(h.load(path,error),qPrintable(error));
    check(h.isFirecore(),"fixture Firecore profile");frames(h,40);
    check(h.debugCapabilities().video==15,"Firecore layer capabilities");
    const auto initial=h.captureMovieState();check(!initial.isEmpty(),"capture Firecore baseline");
    const auto sample=[&](quint32 mask){
        check(h.restoreMovieState(initial,error),"restore Firecore baseline");
        h.setDebugMasks(mask,0);h.runFrame();return h.frame();
    };
    const auto normal=sample(0);
    const auto nativeState=h.serialize(),digest=h.netplayDigest();
    const auto checked=[&](quint32 mask){
        const auto frame=sample(mask);
        check(h.serialize()==nativeState && h.netplayDigest()==digest,"Firecore mask altered emulated state");
        return frame;
    };
    const auto noA=checked(1),noAB=checked(3),noWindow=checked(4),noSprites=checked(8),allOff=checked(15);
    check(normal.pixelColor(100,40)==QColor(255,0,0),"Firecore plane A fixture");
    check(noA.pixelColor(100,40)==QColor(0,255,0),"Firecore plane A hide reveals B");
    check(noAB.pixelColor(100,40)==QColor(255,0,255),"Firecore plane B hide exposes backdrop");
    check(normal.pixelColor(100,2)==QColor(0,0,255),"Firecore window fixture");
    check(noWindow.pixelColor(100,2)==QColor(0,255,0),"Firecore window hide reveals B");
    check(normal.pixelColor(33,33)==QColor(255,255,255),"Firecore sprite fixture");
    check(noSprites.pixelColor(33,33)==QColor(255,0,0),"Firecore sprite hide reveals A");
    check(allOff.pixelColor(100,2)==QColor(255,0,255) &&
          allOff.pixelColor(33,33)==QColor(255,0,255),"Firecore all layers hidden");
    check(checked(0)==normal,"Firecore layers restore");
    const auto bitmapPath=dir.filePath("original-bitmap.atf");save(bitmapPath,mdRom(true,true));
    check(h.load(bitmapPath,error) && h.isFirecore(),"load Firecore bitmap fixture");frames(h,40);
    const auto rasterState=h.captureMovieState();check(!rasterState.isEmpty(),"capture Firecore bitmap baseline");
    const auto raster=[&](quint32 mask){
        check(h.restoreMovieState(rasterState,error),"restore Firecore bitmap baseline");
        h.setDebugMasks(mask,0);h.runFrame();return h.frame();
    };
    const auto rasterNormal=raster(0);
    const auto rasterNativeState=h.serialize(),rasterDigest=h.netplayDigest();
    const auto rasterChecked=[&](quint32 mask){
        const auto frame=raster(mask);
        check(h.serialize()==rasterNativeState && h.netplayDigest()==rasterDigest,
              "Firecore bitmap mask altered emulated state");
        return frame;
    };
    const auto rasterNoA=rasterChecked(1),rasterNoB=rasterChecked(2),
               rasterNoWindow=rasterChecked(4),rasterNoSprites=rasterChecked(8);
    check(rasterNormal.pixelColor(100,2)!=QColor(255,0,255),"Firecore bitmap fixture");
    check(rasterNoA.pixelColor(100,2)==QColor(255,0,255),"Firecore bitmap hide");
    check(rasterNoB==rasterNormal && rasterNoWindow==rasterNormal,
          "inactive Firecore tile planes changed bitmap output");
    check(rasterNoSprites.pixelColor(33,33)!=rasterNormal.pixelColor(33,33),
          "Firecore bitmap sprite hide");
    check(rasterChecked(0)==rasterNormal,"Firecore bitmap layers restore");
    std::puts("PASS: Firecore A/B/window/bitmap/sprites, backdrop, state invariance and restore");
}
void run(const QString& core,bool videoOnly){
    QTemporaryDir dir;check(dir.isValid(),"temp directory");QString error;
    const auto md=dir.filePath("original.md"),sms=dir.filePath("original.sms");save(md,mdRom());save(sms,smsRom());
    LibretroHost h(core,dir.path());check(h.load(md,error),qPrintable(error));frames(h,30);
    auto caps=h.debugCapabilities();check(caps.video==15 && (caps.audio&15)==15 && (caps.audio&(quint64(1)<<36)),"MD capabilities");
    const auto initial=h.captureMovieState();check(!initial.isEmpty(),"capture baseline");
    check(h.restoreMovieState(initial,error),"control restore");h.runFrame();const auto digest=h.netplayDigest();const auto nativeState=h.serialize();const auto normal=h.frame();check(!normal.isNull(),"fixture video");
    const auto sample=[&](quint32 mask,quint32 color=0xff00ff){check(h.restoreMovieState(initial,error),"restore baseline");h.setDebugBackdrop(color);h.setDebugMasks(mask,0);h.runFrame();check(h.netplayDigest()==digest,"debug layer altered game memory");check(h.serialize()==nativeState,"debug layer altered emulated native state");return h.frame();};
    const auto noA=sample(1),noAB=sample(3),noWindow=sample(4),noSprites=sample(8),allOff=sample(15);
    check(normal.pixelColor(100,40).red()>normal.pixelColor(100,40).green(),"plane A fixture red");
    check(noA.pixelColor(100,40).green()>noA.pixelColor(100,40).red(),"plane A hide reveals B");
    check(noAB.pixelColor(100,40)==QColor(255,0,255),"default magenta backdrop");
    check(normal.pixelColor(100,2).blue()>normal.pixelColor(100,2).red(),"window fixture blue");
    check(noWindow.pixelColor(100,2).green()>noWindow.pixelColor(100,2).blue(),"window hide reveals B");
    check(normal.pixelColor(33,33)!=noSprites.pixelColor(33,33),"sprite hiding changes output");
    check(allOff.pixelColor(33,33)==QColor(255,0,255),"all layers hidden");
    const auto custom=sample(15,0x00ffff);check(custom.pixelColor(100,40)==QColor(0,255,255),"custom backdrop");
    check(sample(0)==normal,"restore layers restores video");
    if(videoOnly){std::puts("PASS: actual-core video masks, backdrop, native-state invariance");return;}
    // Audio mute is output-only; oscillator advances while muted and resumes.
    h.setDebugMasks(0,0);frames(h,4);check(audible(h.audio()),"PSG tone fixture audible");
    h.setDebugMasks(0,1);frames(h,6);check(!audible(h.audio()),"PSG1 mute");
    h.setDebugMasks(0,0);frames(h,4);check(audible(h.audio()),"PSG1 unmute");
    const double fullVolume=energy(h);
    h.setDebugAudioGain(0,50);const double halfVolume=energy(h);
    check(halfVolume>fullVolume*.40 && halfVolume<fullVolume*.60,"PSG channel gain is not 50 percent");
    h.setDebugAudioGain(0,0);frames(h,8);check(!audible(h.audio()) && h.debugAudioMask()==0,"zero volume altered checkbox or leaked sound");
    h.setDebugAudioGain(0,100);check(energy(h)>fullVolume*.85,"PSG volume did not restore");
    h.setDebugBackdrop(0x00ffff);
    check(h.load(sms,error),qPrintable(error));frames(h,8);caps=h.debugCapabilities();
    check(caps.video==24 && (caps.audio&15)==15 && !(caps.audio&(quint64(63)<<4)),"SMS capabilities");
    check(h.debugVideoMask()==0 && h.debugAudioMask()==0,"new game resets masks");
    check(audible(h.audio()),"SMS PSG fixture audible");h.setDebugMasks(24,15);frames(h,6);check(!audible(h.audio()),"SMS PSG mute");
    const auto smsColor=h.frame().pixelColor(120,80);
    if(smsColor!=QColor(0,255,255))std::fprintf(stderr,"SMS color=%06x mask=%u size=%dx%d\n",smsColor.rgb()&0xffffff,h.debugVideoMask(),h.frame().width(),h.frame().height());
    check(smsColor==QColor(0,255,255),"SMS hidden background color");
    // Recorded/remote presentation options must not stack Blargg on a local shader.
    auto recording=h.movieConfiguration();
    auto options=recording.value("options").toObject();options.insert("genesis_plus_gx_blargg_ntsc_filter","composite");recording.insert("options",options);
    check(h.setBlarggFilter(0,error) && h.prepareMovie(recording,error),"prepare recorded display options");
    check(h.movieConfiguration().value("options").toObject().value("genesis_plus_gx_blargg_ntsc_filter").toString()=="disabled","recording overrode local output choice");
    check(h.setBlarggFilter(4,error) && h.restoreMovieConfiguration(error),"change local filter during recording");
    check(h.movieConfiguration().value("options").toObject().value("genesis_plus_gx_blargg_ntsc_filter").toString()=="rgb","ending recording lost local output choice");
    h.unload();check(h.debugCapabilities().video==0 && h.debugCapabilities().audio==0,"unload clears capabilities");
    std::puts("PASS: actual-core MD A/B/window/sprites, collision state invariance, custom backdrop, PSG mute/resume/volume, SMS capabilities, load/unload reset and local display choice across recording");
}
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{check(argc>=2,"core path required");if(argc>2 && QByteArray(argv[2])=="--firecore-video")firecoreVideo(QString::fromLocal8Bit(argv[1]));else run(QString::fromLocal8Bit(argv[1]),argc>2 && QByteArray(argv[2])=="--video");return 0;}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
