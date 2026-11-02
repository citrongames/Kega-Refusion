#pragma once
#include "core/MovieTape.h"
#include <QByteArray>
#include <deque>
#include <map>
class LibretroHost;
// Worker-owned, bounded speculation. The host is authoritative; clients replay
// only unconfirmed frames and never repeat GUI/audio/achievement/disk effects.
class NetplayTimeline final {
public:
    static constexpr int PredictionFrames = 6;
    static constexpr qsizetype MemoryLimit = 64*1024*1024;
    void reset(quint64 epoch);
    bool ingest(const QByteArray& packet);
    enum Result { Run, Wait, Resync };
    Result beforeFrame(LibretroHost&,const MovieInput& local,int role,MovieInput& input,QString& error);
    void afterFrame() { ++position_; }
    quint64 position() const { return position_; }
    quint64 epoch() const { return epoch_; }
    static QByteArray inputBytes(const MovieInput&);
    static bool parseInput(const QByteArray&,MovieInput&);
    static QByteArray frameBytes(quint64 epoch,quint64 frame,const MovieInput&,const QByteArray& digest={});
    static MovieInput merge(const QList<QPair<int,MovieInput>>& inputs);
    static void apply(LibretroHost&,const MovieInput&);
private:
    struct Frame { MovieInput input; QByteArray digest; };
    struct Prediction {
        quint64 frame;
        MovieInput input;
        MovieInput local;
        int role;
        QByteArray state;
        QByteArray digest;
    };
    quint64 epoch_=0, position_=0, confirmed_=0;
    MovieInput previous_;
    std::map<quint64,Frame> authoritative_;
    std::deque<Prediction> predicted_;
    qsizetype bytes_=0;
};
