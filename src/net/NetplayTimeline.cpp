#include "NetplayTimeline.h"
#include "core/LibretroHost.h"
#include <QtEndian>
#include <algorithm>
namespace {
void append64(QByteArray& b,quint64 n) { n=qToBigEndian(n); b.append(reinterpret_cast<const char*>(&n),8); }
quint16 neutral(quint16 b) {
    if ((b & 0x30)==0x30) b &= ~0x30; // libretro UP/DOWN
    if ((b & 0xc0)==0xc0) b &= ~0xc0; // LEFT/RIGHT
    return b;
}
MovieInput withLocal(const MovieInput& previous,const MovieInput& local,int role) {
    MovieInput result=previous;
    if(role>=1 && role<=2) {
        const int port=role-1;
        result.buttons[port]=neutral(local.buttons[0]);
        result.aim[port]=local.aim[0];
    }
    result.commands=0;
    return result;
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
void NetplayTimeline::reset(quint64 epoch) { epoch_=epoch; position_=confirmed_=0; previous_={}; previous_.aim.fill(quint64(1)<<32); authoritative_.clear(); predicted_.clear(); bytes_=0; }
bool NetplayTimeline::ingest(const QByteArray& b) {
    if (b.size()!=46 && b.size()!=78) return false;
    const auto epoch=qFromBigEndian<quint64>(b.constData());
    if (epoch<epoch_) return true;
    if (epoch!=epoch_) return false;
    const auto frame=qFromBigEndian<quint64>(b.constData()+8);
    if (frame<confirmed_) return true;
    if (frame>position_+240 || authoritative_.size()>=256) return false;
    Frame f; if (!parseInput(b.mid(16,30),f.input)) return false;
    if (b.size()==78) f.digest=b.mid(46);
    authoritative_.insert_or_assign(frame,std::move(f)); return true;
}
NetplayTimeline::Result NetplayTimeline::beforeFrame(LibretroHost& h,const MovieInput& local,int role,MovieInput& input,QString& error) {
    bool replay=false;
    quint64 replayFrom=position_;
    for (auto& p:predicted_) {
        auto it=authoritative_.find(p.frame);
        if (it==authoritative_.end()) break;
        if (p.input!=it->second.input) { replay=true; replayFrom=p.frame; break; }
        if (!it->second.digest.isEmpty() && p.digest!=it->second.digest) { error=QStringLiteral("Network state mismatch"); return Resync; }
    }
    if (replay) {
        auto first=std::find_if(predicted_.begin(),predicted_.end(),[&](const Prediction& p){return p.frame==replayFrom;});
        if (first==predicted_.end() || !h.restoreMovieState(first->state,error,true)) return Resync;
        MovieInput last=first==predicted_.begin()?previous_:std::prev(first)->input;
        for (auto it=first;it!=predicted_.end();++it) {
            auto a=authoritative_.find(it->frame);
            const auto corrected=a==authoritative_.end()
                ?withLocal(last,it->local,it->role):a->second.input;
            bytes_-=it->state.size(); it->state=h.captureMovieState(false); bytes_+=it->state.size();
            it->digest=h.netplayDigest();
            if (it->state.isEmpty() || bytes_>MemoryLimit) {
                error=QStringLiteral("Network rollback snapshot unavailable"); return Resync;
            }
            if (a!=authoritative_.end() && !a->second.digest.isEmpty() && a->second.digest!=it->digest) {
                error=QStringLiteral("Network rollback state mismatch at frame %1").arg(it->frame);
                return Resync;
            }
            it->input=corrected; apply(h,corrected); h.runFrame(false,false); last=corrected;
        }
    }
    while (!predicted_.empty()) {
        auto it=authoritative_.find(predicted_.front().frame);
        if (it==authoritative_.end()) break;
        previous_=it->second.input; confirmed_=it->first+1;
        authoritative_.erase(it); bytes_-=predicted_.front().state.size(); predicted_.pop_front();
    }
    auto a=authoritative_.find(position_);
    if (a!=authoritative_.end() && predicted_.empty()) {
        if (!a->second.digest.isEmpty() && h.netplayDigest()!=a->second.digest) {
            error=QStringLiteral("Network state mismatch at frame %1").arg(position_);
            return Resync;
        }
        input=previous_=a->second.input; confirmed_=position_+1; authoritative_.erase(a); return Run;
    }
    if (role==0 && a==authoritative_.end()) return Wait;
    if (position_>=confirmed_+PredictionFrames) return Wait;
    input=a!=authoritative_.end()?a->second.input:withLocal(previous_,local,role);
    auto state=h.captureMovieState(false);
    if (state.isEmpty()) { error=QStringLiteral("Network snapshot unavailable"); return Resync; }
    if (bytes_+state.size()>MemoryLimit) return Wait;
    bytes_+=state.size(); predicted_.push_back({position_,input,local,role,std::move(state),h.netplayDigest()});
    return Run;
}
