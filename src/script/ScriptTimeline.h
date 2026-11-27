#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QMap>

struct lua_State;

class ScriptTimeline final {
public:
    static constexpr qsizetype MaxSnapshotBytes = 256 * 1024;

    static bool validId(QByteArrayView id);

    struct Encoding {
        QByteArray value;
        const char* error = nullptr;
        int work = 0;
        int luaStatus = 0;
    };
    // On Lua failure the error object remains on the stack. All encoder
    // allocations are released before the caller propagates that error.
    static Encoding encodeValue(lua_State* L, int index);
    static int pushValue(lua_State* L, QByteArrayView value);

    bool hasData() const { return !entries_.isEmpty(); }
    bool contains(const QByteArray& id) const { return entries_.contains(id); }
    void define(const QByteArray& id, quint32 schema, QByteArrayView source, QByteArray value);
    void remove(const QByteArray& id);
    const QByteArray& value(const QByteArray& id) const;
    void set(const QByteArray& id, QByteArray value);
    const char* status(const QByteArray& id) const;

    QByteArray capture();
    static bool validate(const QByteArray& bytes);
    bool restore(const QByteArray& bytes);
    void reset();

private:
    enum class Status { Current, Restored, Missing, Incompatible, Reset };
    struct Entry {
        quint32 schema;
        QByteArray sourceHash, initial, current;
        Status status = Status::Current;
    };

    void assign(Entry& entry, const QByteArray& value, Status status);
    void defaults(Status status);

    QMap<QByteArray, Entry> entries_;
    QByteArray cache_;
    bool dirty_ = false;
};
