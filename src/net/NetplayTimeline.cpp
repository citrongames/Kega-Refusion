#include "NetplayTimeline.h"
#include "core/LibretroHost.h"
#include <QtEndian>
#include <QJsonArray>
#include "input/ControllerType.h"
namespace {
void append64(QByteArray& b,quint64 n) { n=qToBigEndian(n); b.append(reinterpret_cast<const char*>(&n),8); }
quint16 neutral(quint16 b) {
    if ((b & 0x30)==0x30) b &= ~0x30; // libretro UP/DOWN
    if ((b & 0xc0)==0xc0) b &= ~0xc0; // LEFT/RIGHT
    return b;
}

}
QByteArray NetplayTimeline::inputBytes(const MovieInput& in) {
    QByteArray b; b.reserve(40);
    for (unsigned i=0;i<MaxControllerPlayers;++i) {
        const auto n=qToBigEndian(in.buttons[i]); b.append(reinterpret_cast<const char*>(&n),2);
    }
    for (unsigned i=0;i<MaxGunPlayers;++i) append64(b,in.aim[i]);
    return b;
}
bool NetplayTimeline::parseInput(const QByteArray& b,MovieInput& in) {
    if (b.size()!=40) return false;
    MovieInput parsed;
    for (unsigned i=0;i<MaxControllerPlayers;++i)
        parsed.buttons[i]=qFromBigEndian<quint16>(b.constData()+2*i);
    for (unsigned i=0;i<MaxGunPlayers;++i) {
        parsed.aim[i]=qFromBigEndian<quint64>(b.constData()+16+8*i);
        if (parsed.aim[i]>>33) return false;
    }
    in=parsed; return true;
}
QByteArray NetplayTimeline::frameBytes(quint64 epoch,quint64 frame,const MovieInput& in,const QByteArray& digest) {
    QByteArray b; append64(b,epoch); append64(b,frame); b+=inputBytes(in);
    if (digest.size()==32) b+=digest;
    return b;
}
unsigned NetplayTimeline::localInputSource(int role,const QJsonObject& configuration) {
    if(role<1 || role>int(MaxGunPlayers) ||
       configuration.value("adapter").toInt(-1)!=int(ControllerAdapter::None))return 0;
    const auto controllers=configuration.value("controllers").toArray();
    const unsigned target=unsigned(role-1);
    if(target>=unsigned(controllers.size()))return 0;
    const int kind=controllers[int(target)].toObject().value("kind").toInt(-1);
    if(kind<=int(ControllerType::Pad) || kind>int(ControllerType::Justifier))return 0;
    for(unsigned source=0;source<MaxGunPlayers && source<unsigned(controllers.size());++source) {
        const auto device=controllers[int(source)].toObject();
        if(device.value("connected").toBool() && device.value("kind").toInt(-1)==kind)return source;
    }
    return target;
}
MovieInput NetplayTimeline::merge(const QList<QPair<int,MovieInput>>& inputs) {
    MovieInput result; result.aim.fill(quint64(1)<<32);
    for (const auto& p:inputs) {
        if (p.first<1 || p.first>int(MaxControllerPlayers)) continue;
        const int port=p.first-1;
        // Each participant uses their local first controller for the assigned role.
        result.buttons[port] |= p.second.buttons[0];
        // Stable peer-id order resolves simultaneous aiming, without averaging away targets.
        if (port<int(MaxGunPlayers) && !(p.second.aim[0]&(quint64(1)<<32))) result.aim[port]=p.second.aim[0];
    }
    for (auto& b:result.buttons) b=neutral(b);
    return result;
}
void NetplayTimeline::apply(LibretroHost& h,const MovieInput& in) {
    for (unsigned p=0;p<MaxControllerPlayers;++p) h.setButtons(p,in.buttons[p]);
    for (unsigned p=0;p<MaxGunPlayers;++p) h.setGunAim(p,qint16(in.aim[p]),qint16(in.aim[p]>>16),bool(in.aim[p]&(quint64(1)<<32)));
}
