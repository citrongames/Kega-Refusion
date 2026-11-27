#include "ScriptTimeline.h"
#include "ScriptTypes.h"

#include <lauxlib.h>
#include <QCryptographicHash>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

namespace {
constexpr int MaxIdBytes = 64;
constexpr int MaxValueBytes = 16 * 1024;
constexpr int MaxStringBytes = 4096;
constexpr int MaxDepth = 8;
constexpr int MaxNodes = 512;
constexpr int HashBytes = 32;
constexpr quint32 MaxSchema = 0x7fffffff;
constexpr char Magic[] = "KRLT";
constexpr quint8 Version = 1;

static_assert(std::numeric_limits<lua_Integer>::digits == 63);
static_assert(sizeof(lua_Number) == sizeof(quint64) && std::numeric_limits<lua_Number>::is_iec559);
static_assert(6 + ScriptLimits::MaxScripts * (1 + MaxIdBytes + HashBytes + 4 + 2 + MaxValueBytes)
              <= ScriptTimeline::MaxSnapshotBytes);

enum class Tag : quint8 { Nil, False, True, Integer, Number, String, Table };

template<typename Integer>
void appendInteger(QByteArray& bytes, Integer value) {
    char data[sizeof(value)];
    qToLittleEndian(value, data);
    bytes.append(data, sizeof(value));
}

struct Reader {
    QByteArrayView remaining;

    bool take(qsizetype size, QByteArrayView& value) {
        if (size > remaining.size()) return false;
        value = remaining.first(size);
        remaining = remaining.sliced(size);
        return true;
    }

    template<typename Integer>
    bool integer(Integer& value) {
        QByteArrayView bytes;
        if (!take(sizeof(value), bytes)) return false;
        value = qFromLittleEndian<Integer>(bytes.data());
        return true;
    }
};

struct Encoder {
    struct Member { QByteArray key, value; };
    struct Table {
        QByteArray bytes;
        int nodes = 0;
        int height = 0;
    };

    ScriptTimeline::Encoding& result;
    std::array<std::vector<Member>, MaxDepth> members;
    std::unordered_map<const void*, Table> tables;
    int nodes = 0;
    int bytes = 0;

    bool fail(const char* reason) {
        result.error = reason;
        return false;
    }

    bool countNodes(int count) {
        if (count > MaxNodes - nodes) return fail("Timeline value exceeds 512 nodes");
        nodes += count;
        return true;
    }

    bool countBytes(int count) {
        if (count > MaxValueBytes - bytes) return fail("Timeline value exceeds 16 KiB");
        bytes += count;
        return true;
    }

    bool writeString(lua_State* L, int index, QByteArray& output) {
        size_t size = 0;
        const auto data = lua_tolstring(L, index, &size);
        if (size > MaxStringBytes) return fail("Timeline strings cannot exceed 4096 bytes");
        if (!countBytes(3 + int(size))) return false;
        output.append(char(Tag::String));
        appendInteger(output, quint16(size));
        output.append(data, qsizetype(size));
        return true;
    }

    bool writeInteger(lua_State* L, int index, QByteArray& output) {
        if (!countBytes(9)) return false;
        output.append(char(Tag::Integer));
        appendInteger(output, quint64(lua_tointeger(L, index)));
        return true;
    }

    bool writeTable(lua_State* L, int index, int depth, QByteArray& output, int& height) {
        const auto identity = lua_topointer(L, index);
        auto [position, inserted] = tables.try_emplace(identity);
        auto& table = position->second;
        if (!inserted) {
            if (table.bytes.isEmpty()) return fail("Timeline tables cannot contain cycles");
            if (depth + table.height - 1 > MaxDepth) return fail("Timeline value exceeds depth 8");
            if (!countNodes(table.nodes - 1) || !countBytes(int(table.bytes.size()))) return false;
            output.append(table.bytes);
            height = table.height;
            return true;
        }
        if (lua_getmetatable(L, index)) return fail("Timeline tables cannot have metatables");
        if (!countBytes(3)) return false;
        const auto before = nodes;
        auto& fields = members[size_t(depth - 1)];
        lua_pushnil(L);
        while (lua_next(L, index)) {
            if (depth == MaxDepth) return fail("Timeline value exceeds depth 8");
            if (!countNodes(1)) return false;
            auto& field = fields.emplace_back();
            if (lua_isinteger(L, -2)) {
                if (!writeInteger(L, -2, field.key)) return false;
            } else if (lua_type(L, -2) == LUA_TSTRING) {
                if (!writeString(L, -2, field.key)) return false;
            } else {
                return fail("Timeline table keys must be integers or strings");
            }
            int memberHeight = 0;
            if (!writeValue(L, lua_gettop(L), depth + 1, field.value, memberHeight)) return false;
            height = std::max(height, 1 + memberHeight);
            lua_pop(L, 1);
        }
        // Encoded keys have distinct tags for integers and strings, and the
        // byte order gives a canonical sort even for embedded NUL and UTF-8.
        std::sort(fields.begin(), fields.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
        table.bytes.append(char(Tag::Table));
        appendInteger(table.bytes, quint16(fields.size()));
        for (const auto& field : fields) {
            table.bytes.append(field.key);
            table.bytes.append(field.value);
        }
        fields.clear();
        table.nodes = 1 + nodes - before;
        table.height = height;
        output.append(table.bytes);
        return true;
    }

    bool writeValue(lua_State* L, int index, int depth, QByteArray& output, int& height) {
        height = 1;
        if (!countNodes(1)) return false;
        switch (lua_type(L, index)) {
        case LUA_TNIL:
            if (!countBytes(1)) return false;
            output.append(char(Tag::Nil));
            return true;
        case LUA_TBOOLEAN:
            if (!countBytes(1)) return false;
            output.append(char(lua_toboolean(L, index) ? Tag::True : Tag::False));
            return true;
        case LUA_TNUMBER: {
            if (lua_isinteger(L, index)) return writeInteger(L, index, output);
            const auto number = lua_tonumber(L, index);
            if (!std::isfinite(number)) return fail("Timeline numbers must be finite");
            if (!countBytes(9)) return false;
            output.append(char(Tag::Number));
            quint64 bits;
            std::memcpy(&bits, &number, sizeof(bits));
            appendInteger(output, bits);
            return true;
        }
        case LUA_TSTRING:
            return writeString(L, index, output);
        case LUA_TTABLE:
            return writeTable(L, index, depth, output, height);
        default:
            return fail("Unsupported timeline value type");
        }
    }
};

int encode(lua_State* L) {
    auto& encoder = *static_cast<Encoder*>(lua_touserdata(L, 2));
    if (!lua_checkstack(L, 2 * MaxDepth + 8)) return luaL_error(L, "Timeline stack limit reached");
    int height;
    encoder.writeValue(L, 1, 1, encoder.result.value, height);
    encoder.result.work = encoder.bytes + 8 * encoder.nodes;
    return 0;
}

enum class ValueKind { Root, Key, Member };

bool readValue(Reader& reader, int depth, int& nodes, lua_State* L, ValueKind kind) {
    quint8 type;
    if (depth > MaxDepth || --nodes < 0 || !reader.integer(type)) return false;
    const auto tag = Tag(type);
    if (kind == ValueKind::Key && tag != Tag::Integer && tag != Tag::String) return false;
    switch (tag) {
    case Tag::Nil:
        if (kind != ValueKind::Root) return false;
        if (L) lua_pushnil(L);
        return true;
    case Tag::False:
    case Tag::True:
        if (L) lua_pushboolean(L, tag == Tag::True);
        return true;
    case Tag::Integer: {
        qint64 number;
        if (!reader.integer(number)) return false;
        if (L) lua_pushinteger(L, lua_Integer(number));
        return true;
    }
    case Tag::Number: {
        quint64 bits;
        if (!reader.integer(bits)) return false;
        lua_Number number;
        std::memcpy(&number, &bits, sizeof(number));
        if (!std::isfinite(number)) return false;
        if (L) lua_pushnumber(L, number);
        return true;
    }
    case Tag::String: {
        quint16 size;
        QByteArrayView data;
        if (!reader.integer(size) || size > MaxStringBytes || !reader.take(size, data)) return false;
        if (L) lua_pushlstring(L, data.data(), size);
        return true;
    }
    case Tag::Table: {
        quint16 count;
        if (!reader.integer(count) || 2 * count > nodes) return false;
        if (L) lua_createtable(L, 0, count);
        QByteArrayView previous;
        for (int i = 0; i < count; ++i) {
            const auto before = reader.remaining;
            if (!readValue(reader, depth + 1, nodes, L, ValueKind::Key)) return false;
            const auto key = before.first(before.size() - reader.remaining.size());
            if (!previous.isEmpty() && previous.compare(key) >= 0) return false;
            previous = key;
            if (!readValue(reader, depth + 1, nodes, L, ValueKind::Member)) return false;
            if (L) lua_rawset(L, -3);
        }
        return true;
    }
    }
    return false;
}

struct SnapshotEntry {
    QByteArrayView id, sourceHash, value;
    quint32 schema;
};

using SnapshotEntries = std::array<SnapshotEntry, ScriptLimits::MaxScripts>;

bool readSnapshot(QByteArrayView bytes, SnapshotEntries& entries, quint8& count) {
    count = 0;
    if (bytes.isEmpty()) return true;
    if (bytes.size() > ScriptTimeline::MaxSnapshotBytes || !bytes.startsWith(Magic)) return false;
    Reader reader{bytes.sliced(sizeof(Magic) - 1)};
    quint8 version;
    if (!reader.integer(version) || version != Version || !reader.integer(count) ||
        count == 0 || count > entries.size()) return false;
    for (int i = 0; i < count; ++i) {
        auto& entry = entries[size_t(i)];
        quint8 idSize;
        quint16 valueSize;
        if (!reader.integer(idSize) || !reader.take(idSize, entry.id) ||
            !ScriptTimeline::validId(entry.id) ||
            (i && entries[size_t(i - 1)].id.compare(entry.id) >= 0) ||
            !reader.take(HashBytes, entry.sourceHash) ||
            !reader.integer(entry.schema) || entry.schema == 0 || entry.schema > MaxSchema ||
            !reader.integer(valueSize) || valueSize > MaxValueBytes || !reader.take(valueSize, entry.value))
            return false;
        Reader value{entry.value};
        int nodes = MaxNodes;
        if (!readValue(value, 1, nodes, nullptr, ValueKind::Root) || !value.remaining.isEmpty()) return false;
    }
    return reader.remaining.isEmpty();
}
}

bool ScriptTimeline::validId(QByteArrayView id) {
    return !id.isEmpty() && id.size() <= MaxIdBytes &&
           std::all_of(id.begin(), id.end(), [](char byte) { return byte >= 0x20 && byte <= 0x7e; });
}

ScriptTimeline::Encoding ScriptTimeline::encodeValue(lua_State* L, int index) {
    lua_pushcfunction(L, encode);
    lua_pushvalue(L, index);
    Encoding result;
    Encoder encoder{result, {}, {}};
    lua_pushlightuserdata(L, &encoder);
    result.luaStatus = lua_pcall(L, 2, 0, 0);
    return result;
}

int ScriptTimeline::pushValue(lua_State* L, QByteArrayView bytes) {
    if (!lua_checkstack(L, MaxDepth * 2 + 8)) return luaL_error(L, "Timeline stack limit reached");
    Reader reader{bytes};
    int nodes = MaxNodes;
    if (!readValue(reader, 1, nodes, L, ValueKind::Root) || !reader.remaining.isEmpty())
        return luaL_error(L, "Invalid timeline value");
    return 1;
}

void ScriptTimeline::define(const QByteArray& id, quint32 schema, QByteArrayView source, QByteArray value) {
    entries_.insert(id, Entry{schema, QCryptographicHash::hash(source, QCryptographicHash::Sha256), value, value});
    dirty_ = true;
}

void ScriptTimeline::remove(const QByteArray& id) {
    entries_.remove(id);
    dirty_ = hasData();
    if (!dirty_) cache_.clear();
}

const QByteArray& ScriptTimeline::value(const QByteArray& id) const {
    return entries_.constFind(id)->current;
}

void ScriptTimeline::set(const QByteArray& id, QByteArray value) {
    assign(*entries_.find(id), value, Status::Current);
}

const char* ScriptTimeline::status(const QByteArray& id) const {
    constexpr const char* Names[]{"current", "restored", "missing", "incompatible", "reset"};
    return Names[int(entries_.constFind(id)->status)];
}

QByteArray ScriptTimeline::capture() {
    if (!dirty_) return cache_;
    QByteArray bytes(Magic, sizeof(Magic) - 1);
    bytes.append(char(Version));
    bytes.append(char(entries_.size()));
    for (auto it = entries_.cbegin(); it != entries_.cend(); ++it) {
        bytes.append(char(it.key().size()));
        bytes.append(it.key());
        bytes.append(it->sourceHash);
        appendInteger(bytes, it->schema);
        appendInteger(bytes, quint16(it->current.size()));
        bytes.append(it->current);
    }
    cache_ = std::move(bytes);
    dirty_ = false;
    return cache_;
}

bool ScriptTimeline::validate(const QByteArray& bytes) {
    SnapshotEntries entries;
    quint8 count;
    return readSnapshot(bytes, entries, count);
}

bool ScriptTimeline::restore(const QByteArray& bytes) {
    if (bytes.isEmpty()) {
        defaults(Status::Missing);
        return true;
    }
    if (!dirty_ && bytes.constData() == cache_.constData() && bytes.size() == cache_.size()) {
        for (auto& entry : entries_) entry.status = Status::Restored;
        return true;
    }
    SnapshotEntries entries;
    quint8 count;
    if (!readSnapshot(bytes, entries, count)) return false;
    if (entries_.isEmpty()) return true;

    int compatible = 0;
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        const auto end = entries.begin() + count;
        const auto match = std::find_if(entries.begin(), end, [&](const auto& entry) { return entry.id == it.key(); });
        if (match == end) {
            assign(*it, it->initial, Status::Missing);
        } else if (match->schema != it->schema || match->sourceHash != it->sourceHash) {
            assign(*it, it->initial, Status::Incompatible);
        } else {
            if (QByteArrayView(it->current) != match->value) {
                it->current = match->value.toByteArray();
                dirty_ = true;
            }
            it->status = Status::Restored;
            ++compatible;
        }
    }
    if (compatible == count && compatible == entries_.size()) {
        cache_ = bytes;
        dirty_ = false;
    }
    return true;
}

void ScriptTimeline::reset() {
    defaults(Status::Reset);
}

void ScriptTimeline::assign(Entry& entry, const QByteArray& value, Status status) {
    if (entry.current != value) {
        entry.current = value;
        dirty_ = true;
    }
    entry.status = status;
}

void ScriptTimeline::defaults(Status status) {
    if (entries_.isEmpty()) return;
    for (auto& entry : entries_) assign(entry, entry.initial, status);
}
