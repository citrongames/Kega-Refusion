#pragma once
#include "core/MovieTape.h"
#include <QJsonObject>
#include <QByteArray>
#include <QList>
class LibretroHost;
// Logical controller-port input merging. Prediction and rollback belong to GGPO.
class NetplayTimeline final {
public:
    static QByteArray inputBytes(const MovieInput&);
    static bool parseInput(const QByteArray&,MovieInput&);
    static QByteArray frameBytes(quint64 epoch,quint64 frame,const MovieInput&,const QByteArray& digest={});
    static MovieInput merge(const QList<QPair<int,MovieInput>>& inputs);
    static unsigned localInputSource(int role, const QJsonObject& configuration);
    static void apply(LibretroHost&,const MovieInput&);
};
