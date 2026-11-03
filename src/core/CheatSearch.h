#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVector>

namespace Cheats {

// A manual, emulation-thread-owned search over one normalized CPU memory region.
class Search final {
public:
    enum Comparison { Exact = 0, Changed = 1, Unchanged = 2,
                      Increased = 3, Decreased = 4, Unknown = 5 };
    struct Match {
        quint32 address = 0;
        quint32 previous = 0;
        quint32 value = 0;
    };

    bool filter(const QByteArray& memory, quint32 base, bool bigEndian, int width,
                Comparison comparison, quint32 value, bool restart, QString& error);
    QList<Match> matches(int limit = 256) const;
    qsizetype count() const { return candidates_.size(); }
    bool started() const { return started_; }
    void clear();

private:
    static quint32 readValue(const QByteArray& memory, quint32 offset,
                             bool bigEndian, int width);
    QByteArray latest_;
    QByteArray prior_;
    QVector<quint32> candidates_;
    quint32 base_ = 0;
    bool bigEndian_ = false;
    int width_ = 0;
    bool started_ = false;
};

} // namespace Cheats
