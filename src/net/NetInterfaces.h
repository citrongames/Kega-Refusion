#pragma once

#include <QList>
#include <QString>
#include <QtGlobal>

namespace NetInterfaces {
struct Connection {
    QString id;       // Stable native adapter name/GUID, never a display label.
    QString name;     // UI label.
    QString ipv4;     // One usable local IPv4 on this adapter.
    uint index = 0;   // Current OS interface index for socket routing.
    bool physical = false;
    bool defaultRoute = false;
    int metric = 0x7fffffff;
};

QList<Connection> available();
Connection choose(const QString& id = {});
Connection chooseFrom(const QList<Connection>& connections, const QString& id = {});
// The caller must already bind the socket to choice.ipv4. This adds an
// interface constraint; failure is reported and must prevent connect/listen.
bool bindSocket(qintptr fd, const Connection& choice, QString* error = nullptr);
}
