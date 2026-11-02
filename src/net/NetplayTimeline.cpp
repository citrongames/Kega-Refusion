#include "NetplayTimeline.h"
#include "core/LibretroHost.h"
#include <QtEndian>
namespace {
void append64(QByteArray& b,quint64 n) { n=qToBigEndian(n); b.append(reinterpret_cast<const char*>(&n),8); }
quint16 neutral(quint16 b) {
    if ((b & 0x30)==0x30) b &= ~0x30; // libretro UP/DOWN
    if ((b & 0xc0)==0xc0) b &= ~0xc0; // LEFT/RIGHT
    return b;
}

}
QByteArray NetplayTimeline::inputBytes(const MovieInput& in) {
    QByteArray b; b.reserve(30);
    for (int i=0;i<3;++i) { const auto n=qToBigEndian(in.buttons[i]); b.append(reinterpret_cast<const char*>(&n),2); append64(b,in.aim[i]); }
    return b;
}
bool NetplayTimeline::parseInput(const QByteArray& b,MovieInput& in) {
    if (b.size()!=30) return false;
    for (int i=0;i<3;++i) {
        in.buttons[i]=qFromBigEndian<quint16>(b.constData()+10*i);
        in.aim[i]=qFromBigEndian<quint64>(b.constData()+10*i+2);
        if (in.aim[i]>>33) return false;
    }
    in.commands=0; return true;
}
QByteArray NetplayTimeline::frameBytes(quint64 epoch,quint64 frame,const MovieInput& in,const QByteArray& digest) {
    QByteArray b; append64(b,epoch); append64(b,frame); b+=inputBytes(in);
    if (digest.size()==32) b+=digest;
    return b;
}
MovieInput NetplayTimeline::merge(const QList<QPair<int,MovieInput>>& inputs) {
    MovieInput result; result.aim.fill(quint64(1)<<32);
    for (const auto& p:inputs) {
        if (p.first<1 || p.first>2) continue;
        const int port=p.first-1;
        // Each participant uses their local first controller for the assigned role.
        result.buttons[port] |= p.second.buttons[0];
        // Stable peer-id order resolves simultaneous aiming, without averaging away targets.
        if (!(p.second.aim[0]&(quint64(1)<<32))) result.aim[port]=p.second.aim[0];
    }
    for (auto& b:result.buttons) b=neutral(b);
    return result;
}
void NetplayTimeline::apply(LibretroHost& h,const MovieInput& in) {
    for (unsigned p=0;p<3;++p) { h.setButtons(p,in.buttons[p]); h.setGunAim(p,qint16(in.aim[p]),qint16(in.aim[p]>>16),bool(in.aim[p]&(quint64(1)<<32))); }
}
