#include "ScriptRuntime.h"

#include "core/LibretroHost.h"
#include <lauxlib.h>
#include <lualib.h>

#include <QByteArrayView>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace {
constexpr int LuaMemoryLimit = 4 * 1024 * 1024;
constexpr int HookInterval = 256;
constexpr int StartupInstructions = 200000;
constexpr int EventInstructions = 40000;
constexpr int NativeCalls = 192;
constexpr int NativeBytes = 64 * 1024;
constexpr int MaxRangeBytes = 4096;
constexpr int MaxDraws = 128;
constexpr int MaxLogs = 8;
constexpr int MaxLogsPerSecond = 32;
constexpr int MaxTextBytes = 1024;
constexpr auto EventTimeLimit = std::chrono::milliseconds(8);
constexpr auto StartupTimeLimit = std::chrono::milliseconds(50);
constexpr std::array<const char*, 5> EventNames{"frame", "reset", "loadstate", "rewind", "draw"};
using Clock = std::chrono::steady_clock;

struct Catalog {
    kr_inspect_info info{};
    QVector<kr_debug_target> cpus;
    quint32 hardware = 0;
    QString system;
    bool fresh = false;
};

struct Hardware {
    QString path, content;
    unsigned instances = 0;
    std::array<Catalog, 2> catalog;

    explicit Hardware(const LibretroHost& host)
        : path(host.gamePath()), content(host.contentName()),
          instances(host.inspectionInstanceCount()) {
        for (unsigned instance = 0; instance < instances; ++instance) {
            auto& console = catalog[instance];
            if (!host.inspectionCatalog(instance, console.info)) console.info = {};
            console.hardware = console.info.hardware;
            console.system = QString::fromLatin1(console.info.system);
            console.cpus = host.inspectionCpuTargets(instance);
            console.fresh = true;
        }
    }

    void beginEvent() {
        // An explicit catalog request or a video domain read can refresh
        // metadata. Reuse that refresh across scripts during this event.
        for (auto& console : catalog) console.fresh = false;
    }

    const kr_inspect_info* memoryCatalog(const LibretroHost& host, unsigned side) {
        if (side >= instances) return nullptr;
        auto& console = catalog[side];
        if (!console.fresh) {
            if (!host.inspectionCatalog(side, console.info)) console.info = {};
            console.fresh = true;
        }
        return &console.info;
    }

    bool matches(const Hardware& other) const {
        if (!instances || path != other.path || content != other.content || instances != other.instances)
            return false;
        for (unsigned instance = 0; instance < instances; ++instance) {
            const auto& a = catalog[instance];
            const auto& b = other.catalog[instance];
            if (a.hardware != b.hardware || a.system != b.system || a.cpus.size() != b.cpus.size())
                return false;
            for (qsizetype i = 0; i < a.cpus.size(); ++i) {
                if (a.cpus[i].cpu != b.cpus[i].cpu || a.cpus[i].address_bits != b.cpus[i].address_bits ||
                    a.cpus[i].address_shift != b.cpus[i].address_shift)
                    return false;
            }
        }
        return true;
    }
};

int unavailable(lua_State* L, const char* reason) {
    lua_pushnil(L);
    lua_pushstring(L, reason);
    return 2;
}

void integerField(lua_State* L, const char* field, lua_Integer value) {
    lua_pushinteger(L, value);
    lua_setfield(L, -2, field);
}

void stringField(lua_State* L, const char* field, const char* value) {
    lua_pushstring(L, value);
    lua_setfield(L, -2, field);
}

QByteArrayView checkedString(lua_State* L, int arg, int cap) {
    // Argument conversion of numbers is intentional (e.g. draw.text(x, y, score)).
    size_t size = 0;
    const char* str = luaL_checklstring(L, arg, &size);
    if (size > size_t(cap)) luaL_argerror(L, arg, "string is too long");
    return {str, qsizetype(size)};
}

quint32 unsigned32(lua_State* L, int arg) {
    if (lua_type(L, arg) == LUA_TSTRING) checkedString(L, arg, 32);
    const auto value = luaL_checkinteger(L, arg);
    if (value < 0 || value > std::numeric_limits<quint32>::max())
        luaL_argerror(L, arg, "expected an unsigned 32-bit integer");
    return quint32(value);
}

bool bigEndian(lua_State* L, int arg, bool native) {
    if (lua_isnoneornil(L, arg)) return native;
    const auto value = checkedString(L, arg, 6);
    if (value == "native") return native;
    if (value == "be") return true;
    if (value == "le") return false;
    return luaL_argerror(L, arg, "expected 'native', 'be' or 'le'") != 0;
}

bool flag(lua_State* L, int arg) {
    if (lua_isnoneornil(L, arg)) return false;
    luaL_checktype(L, arg, LUA_TBOOLEAN);
    return lua_toboolean(L, arg);
}

int readWidth(lua_State* L, int arg) {
    const auto width = lua_isnoneornil(L, arg) ? 1u : unsigned32(L, arg);
    if (width != 1 && width != 2 && width != 4) luaL_argerror(L, arg, "expected width 1, 2 or 4");
    return int(width);
}

int decodeValue(lua_State* L, const uint8_t* bytes, int size, bool big, bool sign) {
    quint32 value = 0;
    for (int i = 0; i < size; ++i)
        value = (value << 8) | bytes[big ? i : size - 1 - i];
    lua_Integer result = value;
    const auto limit = lua_Integer(1) << (size * 8);
    if (sign && result >= limit / 2) result -= limit;
    lua_pushinteger(L, result);
    return 1;
}

double coordinate(lua_State* L, int arg, double minimum = -8192.0) {
    if (lua_type(L, arg) == LUA_TSTRING) checkedString(L, arg, 32);
    const double value = luaL_checknumber(L, arg);
    if (!std::isfinite(value) || value < minimum || value > 8192.0)
        luaL_argerror(L, arg, "coordinate outside supported range");
    return value;
}

QColor color(lua_State* L, int arg, Qt::GlobalColor fallback) {
    if (lua_isnoneornil(L, arg)) return fallback;
    const auto value = unsigned32(L, arg);
    return QColor::fromRgb(int(value >> 24), int(value >> 16) & 255,
                           int(value >> 8) & 255, int(value) & 255);
}

ScriptSpace space(lua_State* L, int arg) {
    if (lua_isnoneornil(L, arg)) return ScriptSpace::Game;
    const auto name = checkedString(L, arg, 4);
    if (name == "game") return ScriptSpace::Game;
    if (name == "hud") return ScriptSpace::Hud;
    luaL_argerror(L, arg, "expected 'game' or 'hud'");
    return ScriptSpace::Game;
}
}

struct ScriptRuntime::Script {
    ScriptRuntime& owner;
    quint64 id;
    QString path;
    QHash<QString, QImage> images;
    size_t allocated = 0;
    lua_State* L = nullptr;
    std::array<int, EventNames.size()> callbacks{LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF};
    const LibretroHost* host = nullptr;
    Hardware* hardware = nullptr;
    QVector<ScriptDraw>* draws = nullptr;
    ScriptEvent currentEvent = ScriptEvent::Frame;
    quint64 frame = 0;
    unsigned instance = 0;
    int instructions = 0, calls = 0, nativeBytes = 0, logs = 0, overlayStart = 0;
    Clock::time_point deadline, logWindow;
    int logsThisSecond = 0;
    bool logTruncated = false;
    bool starting = false;

    Script(ScriptRuntime& owner, const ScriptCommand& command)
        : owner(owner), id(command.id), path(command.path.left(4096)), images(command.images) {
        L = lua_newstate(allocate, this, luaL_makeseed(nullptr));
        if (L) *static_cast<Script**>(lua_getextraspace(L)) = this;
    }
    ~Script() { if (L) lua_close(L); }

    static Script& self(lua_State* L) { return **static_cast<Script**>(lua_getextraspace(L)); }

    static void* allocate(void* ud, void* ptr, size_t oldSize, size_t newSize) {
        auto& script = *static_cast<Script*>(ud);
        // Lua uses oldSize as an object type tag when allocating a new block.
        if (!ptr) oldSize = 0;
        if (!newSize) {
            script.allocated -= oldSize;
            std::free(ptr);
            return nullptr;
        }
        const size_t retained = script.allocated - oldSize;
        if (newSize > LuaMemoryLimit - retained) return nullptr;
        void* result = std::realloc(ptr, newSize);
        if (result) script.allocated = retained + newSize;
        return result;
    }

    static void instructionHook(lua_State* L, lua_Debug*) {
        auto& script = self(L);
        script.instructions -= HookInterval;
        if (script.instructions <= 0) luaL_error(L, "instruction budget exceeded");
        if (Clock::now() > script.deadline) luaL_error(L, "callback time budget exceeded");
    }

    void begin(const LibretroHost& core, Hardware& catalog, bool startup) {
        host = &core;
        hardware = &catalog;
        starting = startup;
        instructions = startup ? StartupInstructions : EventInstructions;
        calls = NativeCalls;
        nativeBytes = NativeBytes;
        logs = 0;
        deadline = Clock::now() + (startup ? StartupTimeLimit : EventTimeLimit);
        lua_sethook(L, instructionHook, LUA_MASKCOUNT, HookInterval);
    }

    void end() {
        lua_sethook(L, nullptr, 0, 0);
        host = nullptr;
        hardware = nullptr;
        draws = nullptr;
        starting = false;
    }

    void charge(int bytes = 0) {
        nativeBytes -= bytes;
        if (--calls < 0 || nativeBytes < 0) luaL_error(L, "native work budget exceeded");
        if (Clock::now() > deadline) luaL_error(L, "callback time budget exceeded");
    }

    void report(ScriptStatus status, const QString& message = {}, int line = 0) {
        owner.update({id, path, status, message, line});
    }

    void reportLuaError() {
        const auto type = lua_type(L, -1);
        size_t size = 0;
        const char* str = type == LUA_TSTRING ? lua_tolstring(L, -1, &size) : nullptr;
        QString message = str ? QString::fromUtf8(str, qsizetype(std::min<size_t>(size, MaxTextBytes)))
                              : QStringLiteral("Script raised an error without a message");
        int line = 0;
        if (message.startsWith(QStringLiteral("script:"))) {
            const auto end = message.indexOf(QLatin1Char(':'), 7);
            if (end > 7) line = QStringView(message).sliced(7, end - 7).toInt();
        }
        report(ScriptStatus::Error, message, line);
        lua_pop(L, 1);
    }

    static int print(lua_State* L) {
        auto& script = self(L);
        script.charge();
        if (!script.starting && (script.currentEvent == ScriptEvent::Rewind ||
                                 script.currentEvent == ScriptEvent::Draw)) return 0;
        const auto now = Clock::now();
        if (now - script.logWindow >= std::chrono::seconds(1)) {
            script.logWindow = now;
            script.logsThisSecond = 0;
            script.logTruncated = false;
        }
        if (script.owner.updates_.size() >= ScriptLimits::MaxUpdates) return 0;
        if (script.logs >= MaxLogs || script.logsThisSecond >= MaxLogsPerSecond) {
            if (!script.logTruncated) {
                script.logTruncated = true;
                script.report(ScriptStatus::Log, QStringLiteral("Script log truncated"));
            }
            return 0;
        }
        QByteArray output;
        for (int i = 1; i <= lua_gettop(L) && output.size() < MaxTextBytes; ++i) {
            if (i > 1) output.append('\t');
            size_t size = 0;
            const char* str = nullptr;
            switch (lua_type(L, i)) {
            case LUA_TSTRING: case LUA_TNUMBER: str = lua_tolstring(L, i, &size); break;
            case LUA_TBOOLEAN: str = lua_toboolean(L, i) ? "true" : "false"; break;
            default: str = luaL_typename(L, i); break;
            }
            if (!str) continue;
            if (lua_type(L, i) != LUA_TSTRING) size = std::strlen(str);
            output.append(str, qsizetype(std::min<size_t>(size, MaxTextBytes - output.size())));
        }
        ++script.logs;
        ++script.logsThisSecond;
        script.report(ScriptStatus::Log, QString::fromUtf8(output));
        return 0;
    }

    static int on(lua_State* L) {
        auto& script = self(L);
        script.charge();
        const auto name = checkedString(L, 1, 12);
        const auto found = std::find_if(EventNames.cbegin(), EventNames.cend(),
                                       [&](const char* event) { return name == event; });
        if (found == EventNames.cend()) return luaL_argerror(L, 1, "unknown event");
        auto& callback = script.callbacks[size_t(found - EventNames.cbegin())];
        if (!lua_isnil(L, 2)) luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_pushvalue(L, 2);
        const int replacement = luaL_ref(L, LUA_REGISTRYINDEX);
        luaL_unref(L, LUA_REGISTRYINDEX, callback);
        callback = replacement;
        return 0;
    }

    static int instances(lua_State* L) {
        auto& script = self(L);
        script.charge();
        lua_pushinteger(L, script.hardware->instances);
        return 1;
    }

    static int screen(lua_State* L) {
        auto& script = self(L);
        script.charge();
        const auto raster = script.host->nativeVideoSize();
        const auto viewport = script.host->inputViewport();
        lua_createtable(L, 0, 2);
        integerField(L, "width", viewport.isEmpty() ? raster.width() : qRound(viewport.width() * raster.width()));
        integerField(L, "height", viewport.isEmpty() ? raster.height() : qRound(viewport.height() * raster.height()));
        return 1;
    }

    unsigned instanceFor(lua_State* L, int arg) const {
        return lua_isnoneornil(L, arg) ? instance : unsigned32(L, arg);
    }

    const Catalog* console(unsigned instanceId) const {
        return instanceId < hardware->instances ? &hardware->catalog[instanceId] : nullptr;
    }

    const kr_inspect_domain* domain(unsigned side, quint32 id, quint32 offset, int length) const {
        const auto* catalog = console(side);
        if (!catalog) return nullptr;
        auto find = [id](const kr_inspect_info& info) -> const kr_inspect_domain* {
            const auto* end = info.domains + info.domain_count;
            const auto* found = std::find_if(info.domains, end, [id](const auto& d) { return d.id == id; });
            return found == end ? nullptr : found;
        };
        const auto* cached = find(catalog->info);
        // VDP mode changes can alter video domain layout/order. Common RAM and
        // ROM reads reuse their metadata and validate the byte count natively.
        const bool video = id >= KR_MEM_VRAM && id <= KR_MEM_VDP_REGS;
        const bool restore = !starting && (currentEvent == ScriptEvent::Reset ||
                                           currentEvent == ScriptEvent::LoadState);
        if (video || restore || !cached || quint64(offset) + length > cached->size)
            return find(*hardware->memoryCatalog(*host, side));
        return cached;
    }

    const kr_debug_target* cpu(unsigned side, quint32 id) const {
        const auto* info = console(side);
        if (!info) return nullptr;
        const auto found = std::find_if(info->cpus.cbegin(), info->cpus.cend(),
                                       [id](const auto& c) { return c.cpu == id; });
        return found == info->cpus.cend() ? nullptr : &*found;
    }

    static int domains(lua_State* L) {
        auto& script = self(L);
        script.charge();
        const auto* catalog = script.hardware->memoryCatalog(*script.host, script.instanceFor(L, 1));
        if (!catalog) return unavailable(L, "console unavailable");
        const auto& info = *catalog;
        lua_createtable(L, info.domain_count, 0);
        for (unsigned i = 0; i < info.domain_count; ++i) {
            const auto& domain = info.domains[i];
            lua_createtable(L, 0, 5);
            integerField(L, "id", domain.id);
            integerField(L, "size", domain.size);
            integerField(L, "base", domain.address_base);
            stringField(L, "name", domain.name);
            stringField(L, "endian", domain.big_endian ? "be" : "le");
            lua_rawseti(L, -2, i + 1);
        }
        return 1;
    }

    static int cpus(lua_State* L) {
        auto& script = self(L);
        script.charge();
        const auto* console = script.console(script.instanceFor(L, 1));
        if (!console) return unavailable(L, "console unavailable");
        lua_createtable(L, int(console->cpus.size()), 0);
        for (qsizetype i = 0; i < console->cpus.size(); ++i) {
            const auto& cpu = console->cpus[i];
            lua_createtable(L, 0, 5);
            integerField(L, "id", cpu.cpu);
            integerField(L, "addressBits", cpu.address_bits);
            integerField(L, "addressUnit", 1u << cpu.address_shift);
            stringField(L, "name", cpu.name);
            stringField(L, "endian", cpu.cpu != KR_DEBUG_Z80 ? "be" : "le");
            lua_rawseti(L, -2, i + 1);
        }
        return 1;
    }

    static int read(lua_State* L) {
        auto& script = self(L);
        const auto id = unsigned32(L, 1);
        const auto offset = unsigned32(L, 2);
        const auto width = readWidth(L, 3);
        const auto side = script.instanceFor(L, 6);
        script.charge(width);
        const auto* domain = script.domain(side, id, offset, width);
        if (!domain) return unavailable(L, "memory domain unavailable");
        if (quint64(offset) + width > domain->size) return unavailable(L, "offset outside memory domain");
        const auto big = bigEndian(L, 4, domain->big_endian != 0);
        const auto sign = flag(L, 5);
        uint8_t bytes[4];
        if (!script.host->inspectionRead(side, id, offset, bytes, size_t(width)))
            return unavailable(L, "memory bytes unavailable");
        return decodeValue(L, bytes, width, big, sign);
    }

    static int range(lua_State* L) {
        auto& script = self(L);
        const auto id = unsigned32(L, 1);
        const auto offset = unsigned32(L, 2);
        const auto length = unsigned32(L, 3);
        if (length > MaxRangeBytes) return luaL_argerror(L, 3, "range exceeds 4096 bytes");
        const auto side = script.instanceFor(L, 4);
        script.charge(int(length));
        const auto* domain = script.domain(side, id, offset, int(length));
        if (!domain) return unavailable(L, "memory domain unavailable");
        if (quint64(offset) + length > domain->size)
            return unavailable(L, "offset outside memory domain");
        char bytes[MaxRangeBytes];
        if (length && !script.host->inspectionRead(side, id, offset, bytes, length))
            return unavailable(L, "memory bytes unavailable");
        lua_pushlstring(L, bytes, length);
        return 1;
    }

    static int peek(lua_State* L) {
        auto& script = self(L);
        const auto id = unsigned32(L, 1);
        const auto address = unsigned32(L, 2);
        const auto width = readWidth(L, 3);
        const auto side = script.instanceFor(L, 6);
        const auto* cpu = script.cpu(side, id);
        script.charge(width);
        if (!cpu) return unavailable(L, "CPU unavailable");
        const auto end = (quint64(address) << cpu->address_shift) + width;
        if (end > (quint64(1) << (cpu->address_bits + cpu->address_shift)))
            return unavailable(L, "address outside CPU range");
        const auto big = bigEndian(L, 4, id != KR_DEBUG_Z80);
        const auto sign = flag(L, 5);
        uint8_t bytes[4];
        if (!script.host->inspectionPeek(side, id, address, bytes, size_t(width)))
            return unavailable(L, "logical memory bytes unavailable");
        return decodeValue(L, bytes, width, big, sign);
    }

    static int registers(lua_State* L) {
        auto& script = self(L);
        const auto id = unsigned32(L, 1);
        const auto side = script.instanceFor(L, 2);
        script.charge(sizeof(kr_debug_cpu));
        kr_debug_cpu state{};
        if (!script.cpu(side, id) || !script.host->inspectionRegisters(side, id, state))
            return unavailable(L, "CPU registers unavailable");
        lua_createtable(L, 0, 4);
        integerField(L, "pc", state.pc);
        integerField(L, "sp", state.sp);
        integerField(L, "cycles", lua_Integer(state.cycles));
        lua_createtable(L, 0, state.count);
        for (unsigned i = 0; i < state.count; ++i)
            integerField(L, state.regs[i].name, state.regs[i].value);
        lua_setfield(L, -2, "values");
        return 1;
    }

    bool shouldDraw() {
        charge();
        if (!draws) return false;
        if (draws->size() - overlayStart >= MaxDraws) luaL_error(L, "drawing budget exceeded");
        return true;
    }

    static int drawText(lua_State* L) {
        auto& script = self(L);
        if (!script.shouldDraw()) return 0;
        const auto x = coordinate(L, 1), y = coordinate(L, 2);
        const auto text = checkedString(L, 3, MaxTextBytes);
        ScriptDraw command;
        command.primitive = ScriptPrimitive::Text;
        command.space = space(L, 5);
        command.bounds = QRectF(x, y, 0, 0);
        command.color = color(L, 4, Qt::white);
        script.charge(int(text.size()));
        command.text = QString::fromUtf8(text);
        script.draws->push_back(std::move(command));
        return 0;
    }

    static int drawLine(lua_State* L) {
        auto& script = self(L);
        if (!script.shouldDraw()) return 0;
        const auto x = coordinate(L, 1), y = coordinate(L, 2);
        const auto x2 = coordinate(L, 3), y2 = coordinate(L, 4);
        ScriptDraw command;
        command.primitive = ScriptPrimitive::Line;
        command.space = space(L, 6);
        command.bounds = QRectF(QPointF(x, y), QPointF(x2, y2));
        command.color = color(L, 5, Qt::white);
        script.draws->push_back(std::move(command));
        return 0;
    }

    static int drawRect(lua_State* L) {
        auto& script = self(L);
        if (!script.shouldDraw()) return 0;
        const auto x = coordinate(L, 1), y = coordinate(L, 2);
        const auto width = coordinate(L, 3, 0), height = coordinate(L, 4, 0);
        ScriptDraw command;
        command.primitive = ScriptPrimitive::Rectangle;
        command.space = space(L, 7);
        command.bounds = QRectF(x, y, width, height);
        command.color = color(L, 5, Qt::white);
        command.fill = color(L, 6, Qt::transparent);
        script.draws->push_back(std::move(command));
        return 0;
    }

    static int drawCircle(lua_State* L) {
        auto& script = self(L);
        if (!script.shouldDraw()) return 0;
        const auto x = coordinate(L, 1), y = coordinate(L, 2);
        const auto radius = coordinate(L, 3, 0);
        ScriptDraw command;
        command.primitive = ScriptPrimitive::Circle;
        command.space = space(L, 6);
        command.bounds = QRectF(x - radius, y - radius, radius * 2, radius * 2);
        command.color = color(L, 4, Qt::white);
        command.fill = color(L, 5, Qt::transparent);
        script.draws->push_back(std::move(command));
        return 0;
    }

    static int drawImage(lua_State* L) {
        auto& script = self(L);
        const auto name = QString::fromUtf8(checkedString(L, 1, ScriptLimits::MaxImageNameBytes));
        const auto image = script.images.constFind(name);
        if (image == script.images.cend()) return unavailable(L, "image is not prepared");
        if (script.shouldDraw()) {
            const auto x = coordinate(L, 2), y = coordinate(L, 3);
            ScriptDraw command;
            command.primitive = ScriptPrimitive::Image;
            command.space = space(L, 4);
            command.bounds = QRectF(QPointF(x, y), image->deviceIndependentSize());
            command.image = *image;
            script.draws->push_back(std::move(command));
        }
        lua_pushboolean(L, true);
        return 1;
    }

    enum class Work { Arguments, Repeat, Table, Concat };

    static int guardedLibraryCall(lua_State* L) {
        auto& script = self(L);
        const int count = lua_gettop(L);
        if (count > 64) return luaL_error(L, "too many library arguments");
        const auto work = Work(lua_tointeger(L, lua_upvalueindex(2)));
        int cost = 0;
        for (int i = 1; i <= count; ++i) {
            if (lua_type(L, i) != LUA_TSTRING) continue;
            const auto size = lua_rawlen(L, i);
            if (size > MaxRangeBytes) return luaL_argerror(L, i, "library string exceeds 4096 bytes");
            cost += int(size);
        }
        if (work == Work::Repeat) {
            const auto text = checkedString(L, 1, MaxRangeBytes);
            const auto repetitions = luaL_checkinteger(L, 2);
            const auto separator = lua_isnoneornil(L, 3) ? QByteArrayView{} : checkedString(L, 3, MaxRangeBytes);
            if (repetitions > MaxRangeBytes || (repetitions > 0 &&
                repetitions * (text.size() + separator.size()) > MaxRangeBytes))
                return luaL_error(L, "repeat result exceeds 4096 bytes");
            if (repetitions > 0) cost += int(repetitions * (text.size() + separator.size()));
        } else if (work == Work::Table || work == Work::Concat) {
            luaL_checktype(L, 1, LUA_TTABLE);
            const auto length = lua_rawlen(L, 1);
            if (length > 256) return luaL_argerror(L, 1, "table has more than 256 sequence entries");
            cost += int(length);
            if (work == Work::Concat) {
                const auto first = luaL_optinteger(L, 3, 1);
                const auto last = luaL_optinteger(L, 4, lua_Integer(length));
                if (first < 1 || last > lua_Integer(length))
                    return luaL_error(L, "concat range outside the table sequence");
                const auto separator = lua_isnoneornil(L, 2) ? QByteArrayView{} : checkedString(L, 2, MaxRangeBytes);
                for (auto i = first; i <= last; ++i) {
                    lua_rawgeti(L, 1, i);
                    if (lua_type(L, -1) == LUA_TSTRING) {
                        const auto size = lua_rawlen(L, -1);
                        if (size > MaxRangeBytes) return luaL_error(L, "concat element exceeds 4096 bytes");
                        cost += int(size);
                    } else {
                        cost += 32; // A number converted to text within the library.
                    }
                    lua_pop(L, 1);
                    cost += int(separator.size());
                    if (cost > NativeBytes) return luaL_error(L, "concat exceeds native work budget");
                }
            }
        }
        script.charge(cost);
        lua_pushvalue(L, lua_upvalueindex(1));
        lua_insert(L, 1);
        lua_call(L, count, LUA_MULTRET);
        return lua_gettop(L);
    }

    static void guard(lua_State* L, const char* field, Work work = Work::Arguments) {
        lua_getfield(L, -1, field);
        lua_pushinteger(L, lua_Integer(work));
        lua_pushcclosure(L, guardedLibraryCall, 2);
        lua_setfield(L, -2, field);
    }

    static void erase(lua_State* L, const char* field) {
        lua_pushnil(L);
        lua_setfield(L, -2, field);
    }

    static int next(lua_State* L) {
        self(L).charge();
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_settop(L, 2);
        if (lua_next(L, 1)) return 2;
        lua_pushnil(L);
        return 1;
    }

    static int pairs(lua_State* L) {
        self(L).charge();
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_pushcfunction(L, next);
        lua_pushvalue(L, 1);
        lua_pushnil(L);
        return 3;
    }

    static int initialize(lua_State* L) {
        // No Lua finalizers, nested protected calls, coroutine hooks, binary
        // chunks, module lookup, environment loading or OS/file access.
        luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
        for (const char* name : {"collectgarbage", "dofile", "getmetatable", "load", "loadfile",
                                 "pcall", "setmetatable", "warn", "xpcall"})
            erase(L, name);
        guard(L, "tonumber");
        guard(L, "tostring");
        lua_pushcfunction(L, print);
        lua_setfield(L, -2, "print");
        lua_pushcfunction(L, next);
        lua_setfield(L, -2, "next");
        lua_pushcfunction(L, pairs);
        lua_setfield(L, -2, "pairs");
        lua_pop(L, 1);

        luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
        for (const char* name : {"dump", "find", "gmatch", "gsub", "match", "pack", "packsize", "unpack"})
            erase(L, name);
        for (const char* name : {"byte", "char", "format", "len", "lower", "reverse", "sub", "upper"})
            guard(L, name);
        guard(L, "rep", Work::Repeat);
        lua_pushliteral(L, "");
        if (lua_getmetatable(L, -1)) {
            // Numeric-string arithmetic in the string library would bypass
            // the input size checks on explicit numeric conversions.
            for (const char* name : {"__add", "__sub", "__mul", "__mod", "__pow",
                                     "__div", "__idiv", "__unm"})
                erase(L, name);
            lua_pop(L, 1);
        }
        lua_pop(L, 2);

        luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
        for (const char* name : {"move", "pack", "unpack"}) erase(L, name);
        for (const char* name : {"insert", "remove", "sort"}) guard(L, name, Work::Table);
        guard(L, "concat", Work::Concat);
        lua_pop(L, 1);

        luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
        // Copy the names before replacing functions, leaving the table's
        // constant values and iteration order intact.
        std::vector<const char*> mathFunctions;
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_isfunction(L, -1)) mathFunctions.push_back(lua_tostring(L, -2));
            lua_pop(L, 1);
        }
        for (const auto* name : mathFunctions) guard(L, name);
        lua_pop(L, 1);

        luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
        for (const char* name : {"char", "codepoint", "codes", "len", "offset"}) guard(L, name);
        lua_pop(L, 1);

        const luaL_Reg emu[] = {
            {"on", on}, {"instances", instances}, {"screen", screen}, {nullptr, nullptr}
        };
        luaL_newlib(L, emu);
        integerField(L, "apiVersion", 1);
        lua_setglobal(L, "emu");
        const luaL_Reg memory[] = {
            {"domains", domains}, {"cpus", cpus}, {"read", read}, {"range", range},
            {"peek", peek}, {"registers", registers}, {nullptr, nullptr}
        };
        luaL_newlib(L, memory);
        lua_setglobal(L, "memory");
        const luaL_Reg draw[] = {
            {"text", drawText}, {"line", drawLine}, {"rect", drawRect},
            {"circle", drawCircle}, {"image", drawImage}, {nullptr, nullptr}
        };
        luaL_newlib(L, draw);
        lua_setglobal(L, "draw");
        return 0;
    }

    bool launch(const LibretroHost& core, Hardware& catalog, QByteArrayView source) {
        if (!L) {
            report(ScriptStatus::Error, QStringLiteral("Unable to create Lua environment"));
            return false;
        }
        if (source.startsWith("\xef\xbb\xbf")) source = source.sliced(3);
        if (source.startsWith("#!")) {
            const auto newline = source.indexOf('\n');
            source = newline < 0 ? QByteArrayView{} : source.sliced(newline);
        }
        begin(core, catalog, true);
        instance = core.inspectionPresentedInstance();
        lua_pushcfunction(L, initialize);
        int result = lua_pcall(L, 0, 0, 0);
        if (result == LUA_OK)
            result = luaL_loadbufferx(L, source.data() ? source.data() : "", size_t(source.size()), "=script", "t");
        if (result == LUA_OK) result = lua_pcall(L, 0, 0, 0);
        end();
        if (result == LUA_OK) {
            report(ScriptStatus::Running);
            return true;
        }
        reportLuaError();
        return false;
    }

    static int dispatch(lua_State* L) {
        const auto& script = self(L);
        const auto index = size_t(script.currentEvent);
        lua_rawgeti(L, LUA_REGISTRYINDEX, script.callbacks[index]);
        lua_createtable(L, 0, 3);
        stringField(L, "event", EventNames[index]);
        integerField(L, "frame", lua_Integer(script.frame));
        integerField(L, "instance", script.instance);
        lua_call(L, 1, 0);
        return 0;
    }

    bool handle(ScriptEvent event, const LibretroHost& core, Hardware& catalog,
                quint64 number, unsigned side, QVector<ScriptDraw>* overlay) {
        if (callbacks[size_t(event)] == LUA_NOREF || callbacks[size_t(event)] == LUA_REFNIL) return true;
        begin(core, catalog, false);
        currentEvent = event;
        frame = number;
        instance = side;
        draws = overlay;
        overlayStart = overlay ? int(overlay->size()) : 0;
        lua_pushcfunction(L, dispatch);
        const int result = lua_pcall(L, 0, 0, 0);
        end();
        if (result != LUA_OK) {
            if (overlay) overlay->resize(overlayStart);
            reportLuaError();
        }
        return result == LUA_OK;
    }
};

struct ScriptRuntime::State {
    Hardware hardware;
    std::vector<std::unique_ptr<Script>> scripts;

    explicit State(const LibretroHost& host) : hardware(host) {}
};

ScriptRuntime::ScriptRuntime() = default;
ScriptRuntime::~ScriptRuntime() = default;

bool ScriptRuntime::hasScripts() const {
    return state_ && !state_->scripts.empty();
}

bool ScriptRuntime::compatible(const LibretroHost& host) const {
    return !hasScripts() || state_->hardware.matches(Hardware(host));
}

void ScriptRuntime::update(ScriptUpdate update) {
    if (updates_.size() >= ScriptLimits::MaxUpdates) {
        if (update.status == ScriptStatus::Log) return;
        const auto firstLog = std::find_if(updates_.cbegin(), updates_.cend(),
                                          [](const auto& item) { return item.status == ScriptStatus::Log; });
        updates_.removeAt(firstLog == updates_.cend() ? 0 : firstLog - updates_.cbegin());
    }
    updates_.push_back(std::move(update));
}

QVector<ScriptUpdate> ScriptRuntime::takeUpdates() {
    return std::exchange(updates_, {});
}

void ScriptRuntime::stopAll(const QString& reason) {
    if (!state_) return;
    for (const auto& script : state_->scripts) script->report(ScriptStatus::Stopped, reason);
    state_.reset();
}

void ScriptRuntime::command(const ScriptCommand& command, const LibretroHost& host) {
    if (command.action == ScriptAction::StopAll) {
        stopAll();
        return;
    }
    if (!command.id) return;
    if (!state_ && command.action == ScriptAction::Stop) {
        update({command.id, command.path.left(4096), ScriptStatus::Stopped, {}});
        return;
    }
    if (!state_) state_ = std::make_unique<State>(host);
    auto& scripts = state_->scripts;
    auto found = std::find_if(scripts.begin(), scripts.end(),
                              [&](const auto& script) { return script->id == command.id; });
    if (command.action == ScriptAction::Stop) {
        if (found != scripts.end()) {
            (*found)->report(ScriptStatus::Stopped);
            scripts.erase(found);
        } else {
            update({command.id, command.path.left(4096), ScriptStatus::Stopped, {}});
        }
        if (scripts.empty()) state_.reset();
        return;
    }

    auto problem = [&](const QString& message) {
        if (found != scripts.end()) scripts.erase(found);
        update({command.id, command.path.left(4096), ScriptStatus::Error, message});
        if (scripts.empty()) state_.reset();
    };
    if (!state_->hardware.instances) {
        problem(QStringLiteral("No game is loaded"));
        return;
    }
    if (found == scripts.end() && scripts.size() >= ScriptLimits::MaxScripts) {
        problem(QStringLiteral("Too many active scripts"));
        return;
    }
    if (command.source.size() > ScriptLimits::MaxSourceBytes ||
        command.images.size() > ScriptLimits::MaxImages) {
        problem(QStringLiteral("Script source or image count exceeds its limit"));
        return;
    }
    qsizetype imageBytes = 0;
    for (auto image = command.images.cbegin(); image != command.images.cend(); ++image) {
        if (image.key().isEmpty() || image.key().toUtf8().size() > ScriptLimits::MaxImageNameBytes ||
            image->isNull() || image->width() > ScriptLimits::MaxImageSide ||
            image->height() > ScriptLimits::MaxImageSide || image->devicePixelRatio() != 1 ||
            image->sizeInBytes() > ScriptLimits::MaxImageBytes - imageBytes) {
            problem(QStringLiteral("Prepared images exceed their limits"));
            return;
        }
        imageBytes += image->sizeInBytes();
    }

    if (!scripts.empty()) state_->hardware.beginEvent();
    auto script = std::make_unique<Script>(*this, command);
    if (script->launch(host, state_->hardware, command.source)) {
        if (found == scripts.end()) scripts.push_back(std::move(script));
        else *found = std::move(script);
    } else if (found != scripts.end()) {
        scripts.erase(found);
    }
    if (scripts.empty()) state_.reset();
}

ScriptOverlayPtr ScriptRuntime::event(ScriptEvent event, const LibretroHost& host,
                                     quint64 generation, quint64 frame, int instance, bool draw) {
    if (!hasScripts()) return {};
    QVector<ScriptDraw> commands;
    state_->hardware.beginEvent();
    auto& scripts = state_->scripts;
    scripts.erase(std::remove_if(scripts.begin(), scripts.end(), [&](const auto& script) {
        return !script->handle(event, host, state_->hardware, frame, unsigned(instance),
                               draw ? &commands : nullptr);
    }), scripts.end());
    if (scripts.empty()) state_.reset();
    if (commands.isEmpty()) return {};
    return std::make_shared<const ScriptOverlay>(ScriptOverlay{
        generation, frame, instance, std::move(commands)
    });
}
