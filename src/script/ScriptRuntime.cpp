#include "ScriptRuntime.h"

#include "core/LibretroHost.h"
#include "core/MovieTape.h"
#include "i18n/Localization.h"
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
#include <optional>
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
constexpr int MaxControls = 32;
constexpr int AccessCallbacksPerScript = 32;
constexpr int AccessCallbacksPerFrame = 128;
constexpr auto AccessTimeLimit = std::chrono::milliseconds(4);
constexpr auto EventTimeLimit = std::chrono::milliseconds(8);
constexpr auto StartupTimeLimit = std::chrono::milliseconds(50);
constexpr std::array<quint32, 3> AccessEvents{KR_DEBUG_EXEC, KR_DEBUG_READ, KR_DEBUG_WRITE};
constexpr std::array<const char*, 3> AccessNames{"exec", "read", "write"};
constexpr std::array<const char*, 6> EventNames{
    "frame", "reset", "loadstate", "rewind", "draw", "beforeframe"
};
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
    std::optional<InputDisplayState> topology;

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
        topology.reset();
    }

    const InputDisplayState& controllers(const LibretroHost& host) {
        if (!topology) topology = host.inputDisplay(false);
        return *topology;
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

enum Axis : unsigned { AxisX = 1, AxisY = 2, Offscreen = 4 };

struct Controller {
    const char* kind;
    std::array<const char*, 12> buttons;
    unsigned axes = 0;
};

// Names in logical device order, mapped to their canonical MovieInput bits.
constexpr Controller Controllers[] = {
    {"none", {}},
    {"md3", {"b", "a", nullptr, "start", "up", "down", "left", "right", "c"}},
    {"md6", {"b", "a", "mode", "start", "up", "down", "left", "right", "c", "y", "x", "z"}},
    {"sms", {"button1", nullptr, nullptr, "pause", "up", "down", "left", "right", "button2"}},
    {"gg", {"button1", nullptr, nullptr, "start", "up", "down", "left", "right", "button2"}},
    {"sg1000", {"button1", nullptr, nullptr, nullptr, "up", "down", "left", "right", "button2"}},
    {"lightphaser", {nullptr, "trigger", "reload"}, AxisX | AxisY | Offscreen},
    {"menacer", {"auxA", "trigger", "reload", "start", nullptr, nullptr, nullptr, nullptr, "auxB"},
                 AxisX | AxisY | Offscreen},
    {"justifier", {nullptr, "trigger", "reload", "start"}, AxisX | AxisY | Offscreen},
    {"mouse", {"right", "left", nullptr, "middle", nullptr, nullptr, nullptr, nullptr, "wheelDown"},
               AxisX | AxisY},
    {"paddle", {"button"}, AxisX}
};
static_assert(std::size(Controllers) == size_t(InputDisplayDevice::Paddle) + 1);

struct InputOverride {
    InputDisplayDevice device = InputDisplayDevice::None;
    quint16 mask = 0, buttons = 0;
    quint64 aimMask = 0, aim = 0;

    bool active() const { return mask || aimMask; }

    void apply(MovieInput& input, unsigned player) const {
        input.buttons[player] = (input.buttons[player] & ~mask) | buttons;
        if (aimMask && player < input.aim.size())
            input.aim[player] = (input.aim[player] & ~aimMask) | aim;
    }
};

struct Snapshot {
    std::shared_ptr<qsizetype> used;
    QByteArray bytes;

    Snapshot(std::shared_ptr<qsizetype> budget, QByteArray data)
        : used(std::move(budget)), bytes(std::move(data)) { *used += bytes.size(); }
    ~Snapshot() { *used -= bytes.size(); }
};

int unavailable(lua_State* L, const char* reason) {
    lua_pushnil(L);
    lua_pushstring(L, reason);
    return 2;
}

void booleanField(lua_State* L, const char* field, bool value) {
    lua_pushboolean(L, value);
    lua_setfield(L, -2, field);
}

int accepted(lua_State* L) {
    lua_pushboolean(L, true);
    return 1;
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

quint32 writeValue(lua_State* L, int arg, int width) {
    if (lua_type(L, arg) == LUA_TSTRING) checkedString(L, arg, 32);
    const auto value = luaL_checkinteger(L, arg);
    const auto limit = lua_Integer(1) << (width * 8);
    if (value < -limit / 2 || value >= limit)
        luaL_argerror(L, arg, "value does not fit the selected width");
    return quint32(value);
}

QByteArrayView annotationName(lua_State* L, int arg) {
    luaL_checktype(L, arg, LUA_TSTRING);
    const auto name = checkedString(L, arg, ScriptLimits::MaxAnnotationNameBytes);
    if (name.trimmed().isEmpty() || std::any_of(name.begin(), name.end(), [](unsigned char c) {
            return c < 32 || c == 127;
        }))
        luaL_argerror(L, arg, "expected a nonempty name without control characters");
    return name;
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
    quint64 id, run;
    QString path;
    QHash<QString, QImage> images;
    size_t allocated = 0;
    lua_State* L = nullptr;
    lua_State* topLevel = nullptr;
    int topLevelRef = LUA_NOREF;
    std::array<int, EventNames.size()> callbacks;
    std::array<InputOverride, MaxControllerPlayers> overrides{};
    struct Save {
        lua_Integer handle = 0;
        std::shared_ptr<const QByteArray> snapshot;
    };
    std::array<Save, ScriptLimits::MaxSaveObjects> saves{};
    lua_Integer nextSave = 0;
    struct AccessHook {
        DebugAccessSubscription subscription;
        quint32 addressMask = 0;
        lua_Integer handle = 0;
        int callback = LUA_NOREF;
    };
    QVector<AccessHook> accessHooks;
    QVector<ScriptDebugLabel> labels;
    QVector<DebugWatchEntry> watches;
    const DebugAccessEvent* accessEvent = nullptr;
    int accessCallbackRef = LUA_NOREF;
    int accessCallbacks = 0;
    lua_Integer nextAccessHook = 0;
    bool accessFailed = false, accessTruncated = false, accessFramePending = false;
    bool accessTruncationReported = false;
    std::optional<quint64> accessStatsFrame;
    QString saveError;
    const LibretroHost* host = nullptr;
    Hardware* hardware = nullptr;
    const MovieInput* input = nullptr;
    QVector<ScriptDraw>* draws = nullptr;
    ScriptEvent currentEvent = ScriptEvent::Frame;
    quint64 frame = 0;
    unsigned instance = 0;
    struct Budget {
        int instructions = 0, calls = 0, nativeBytes = 0, logs = 0;
        Clock::time_point deadline;
    };
    Budget eventBudget, accessBudget;
    Budget& budget() { return accessEvent ? accessBudget : eventBudget; }
    int overlayStart = 0;
    Clock::time_point logWindow;
    int logsThisSecond = 0;
    bool logTruncated = false;
    bool starting = false;
    bool capturedSave = false;

    Script(ScriptRuntime& owner, const ScriptCommand& command)
        : owner(owner), id(command.id), run(++owner.nextRun_),
          path(command.path.left(4096)), images(command.images) {
        callbacks.fill(LUA_NOREF);
        L = lua_newstate(allocate, this, luaL_makeseed(nullptr));
        if (L) *static_cast<Script**>(lua_getextraspace(L)) = this;
    }
    ~Script() {
        if (!accessHooks.isEmpty()) {
            owner.accessCount_ -= accessHooks.size();
            ++owner.accessRevision_;
        }
        if (!labels.isEmpty() || !watches.isEmpty()) {
            owner.labelCount_ -= labels.size();
            owner.watchCount_ -= watches.size();
            ++owner.annotationRevision_;
        }
        auto& queue = owner.controls_;
        queue.erase(std::remove_if(queue.begin(), queue.end(), [this](const auto& control) {
            return control.run == run;
        }), queue.end());
        if (L) lua_close(L);
    }

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
        auto& budget = self(L).budget();
        budget.instructions -= HookInterval;
        if (budget.instructions <= 0) luaL_error(L, "instruction budget exceeded");
        if (Clock::now() > budget.deadline) luaL_error(L, "callback time budget exceeded");
    }

    void begin(lua_State* runner, const LibretroHost& core, Hardware& catalog, bool startup) {
        host = &core;
        hardware = &catalog;
        starting = startup;
        eventBudget = {startup ? StartupInstructions : EventInstructions, NativeCalls, NativeBytes, 0,
                       Clock::now() + (startup ? StartupTimeLimit : EventTimeLimit)};
        lua_sethook(runner, instructionHook, LUA_MASKCOUNT, HookInterval);
    }

    void end(lua_State* runner) {
        lua_sethook(runner, nullptr, 0, 0);
        host = nullptr;
        hardware = nullptr;
        starting = false;
    }

    void charge(lua_State* runner, int bytes = 0) {
        auto& active = budget();
        active.nativeBytes -= bytes;
        if (--active.calls < 0 || active.nativeBytes < 0) luaL_error(runner, "native work budget exceeded");
        if (Clock::now() > active.deadline) luaL_error(runner, "callback time budget exceeded");
    }

    void report(ScriptStatus status, const QString& message = {}, int line = 0) {
        owner.update({id, path, status, message, line});
    }

    void reportLuaError(lua_State* L) {
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
        script.charge(L);
        if (!script.starting && !script.accessEvent &&
            (script.currentEvent == ScriptEvent::Rewind || script.currentEvent == ScriptEvent::Draw))
            return 0;
        const auto now = Clock::now();
        if (now - script.logWindow >= std::chrono::seconds(1)) {
            script.logWindow = now;
            script.logsThisSecond = 0;
            script.logTruncated = false;
        }
        if (script.owner.updates_.size() >= ScriptLimits::MaxUpdates) return 0;
        auto& logs = script.budget().logs;
        if (logs >= MaxLogs || script.logsThisSecond >= MaxLogsPerSecond) {
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
        ++logs;
        ++script.logsThisSecond;
        script.report(ScriptStatus::Log, QString::fromUtf8(output));
        return 0;
    }

    static int on(lua_State* L) {
        auto& script = self(L);
        script.charge(L);
        if (script.accessEvent) return unavailable(L, "Event registration unavailable in an access hook");
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
        script.charge(L);
        lua_pushinteger(L, script.hardware->instances);
        return 1;
    }

    static int screen(lua_State* L) {
        auto& script = self(L);
        script.charge(L);
        const auto raster = script.host->nativeVideoSize();
        const auto viewport = script.host->inputViewport();
        lua_createtable(L, 0, 2);
        integerField(L, "width", viewport.isEmpty() ? raster.width() : qRound(viewport.width() * raster.width()));
        integerField(L, "height", viewport.isEmpty() ? raster.height() : qRound(viewport.height() * raster.height()));
        return 1;
    }

    bool atControlBoundary() const {
        return !accessEvent && (starting || currentEvent == ScriptEvent::Frame);
    }

    ScriptControl* queueControl(ScriptControlType type, bool value = false,
                                std::shared_ptr<const QByteArray> snapshot = {}) {
        if (owner.controls_.size() >= MaxControls) return nullptr;
        ScriptControl control{};
        control.type = type;
        control.owner = id;
        control.run = run;
        control.value = value;
        control.snapshot = std::move(snapshot);
        control.saveRevision = owner.saveRevision_;
        owner.controls_.push_back(std::move(control));
        return &owner.controls_.last();
    }

    static int pause(lua_State* L) {
        auto& script = self(L);
        script.charge(L);
        const bool paused = lua_isnoneornil(L, 1) || flag(L, 1);
        if (!script.atControlBoundary() || !script.owner.services_.controlAllowed)
            return unavailable(L, "Pause control unavailable here");
        return script.queueControl(ScriptControlType::Pause, paused)
            ? accepted(L) : unavailable(L, "Control queue is full");
    }

    static int reset(lua_State* L) {
        auto& script = self(L);
        script.charge(L);
        const bool hard = flag(L, 1);
        if (!script.atControlBoundary() || !script.owner.services_.controlAllowed)
            return unavailable(L, "Reset control unavailable here");
        return script.queueControl(ScriptControlType::Reset, hard)
            ? accepted(L) : unavailable(L, "Control queue is full");
    }

    static int frameAdvanced(lua_State* L, int, lua_KContext) {
        return accepted(L);
    }

    static int frameAdvance(lua_State* L) {
        auto& script = self(L);
        script.charge(L);
        if (L != script.topLevel || !lua_isyieldable(L))
            return unavailable(L, "frameadvance is only available from the top-level script");
        if (!script.owner.services_.controlAllowed)
            return unavailable(L, "Frame advance unavailable here");
        if (!script.queueControl(ScriptControlType::FrameAdvance))
            return unavailable(L, "Control queue is full");
        return lua_yieldk(L, 0, 0, frameAdvanced);
    }

    static int frameCount(lua_State* L) {
        auto& script = self(L);
        script.charge(L);
        // The native host increments its clock before entering the CPU.
        const auto completed = script.accessEvent ? script.frame - 1 : script.host->runClock().frames;
        lua_pushinteger(L, lua_Integer(completed));
        return 1;
    }

    static int framesPerSecond(lua_State* L) {
        auto& script = self(L);
        script.charge(L);
        lua_pushnumber(L, script.host->fps());
        return 1;
    }

    static int lagged(lua_State* L) {
        auto& script = self(L);
        script.charge(L);
        if (script.starting || script.accessEvent || script.currentEvent != ScriptEvent::Frame ||
            !script.host->supportsInputPollMetrics())
            return unavailable(L, "Input-poll metrics require a supported completed frame");
        lua_pushboolean(L, !script.host->frameInputPolled());
        return 1;
    }

    static unsigned playerNumber(lua_State* L, int arg) {
        const unsigned player = unsigned32(L, arg);
        if (player < 1 || player > MaxControllerPlayers)
            luaL_argerror(L, arg, "invalid logical player number");
        return player - 1;
    }

    InputDisplayDevice inputDevice(unsigned player) {
        return hardware->controllers(*host).players[player].device;
    }

    static int devices(lua_State* L) {
        auto& script = self(L);
        script.charge(L);
        const auto& topology = script.hardware->controllers(*script.host);
        int row = 0;
        lua_createtable(L, MaxControllerPlayers, 0);
        for (unsigned player = 0; player < topology.players.size(); ++player) {
            const auto kind = topology.players[player].device;
            if (kind == InputDisplayDevice::None) continue;
            const auto& controller = Controllers[size_t(kind)];
            lua_createtable(L, 0, 4);
            integerField(L, "player", player + 1);
            integerField(L, "instance", script.hardware->instances == 2 && player == 1 ? 1 : 0);
            stringField(L, "kind", controller.kind);
            lua_createtable(L, int(controller.buttons.size()), 0);
            int buttonIndex = 0;
            for (const auto* name : controller.buttons) {
                if (!name) continue;
                lua_pushstring(L, name);
                lua_rawseti(L, -2, ++buttonIndex);
            }
            lua_setfield(L, -2, "buttons");
            lua_rawseti(L, -2, ++row);
        }
        return 1;
    }

    static int getInput(lua_State* L) {
        auto& script = self(L);
        script.charge(L);
        const auto player = playerNumber(L, 1);
        if (!script.input) return unavailable(L, "No input snapshot at this boundary");
        const auto kind = script.inputDevice(player);
        if (kind == InputDisplayDevice::None) return unavailable(L, "Player is disconnected");
        const auto& controller = Controllers[size_t(kind)];
        const auto buttons = script.input->buttons[player];
        lua_createtable(L, 0, int(controller.buttons.size()) + 3);
        for (unsigned bit = 0; bit < controller.buttons.size(); ++bit) {
            if (const auto* name = controller.buttons[bit])
                booleanField(L, name, (buttons & (1u << bit)) != 0);
        }
        if (controller.axes && player < script.input->aim.size()) {
            const auto aim = script.input->aim[player];
            if (controller.axes & AxisX) integerField(L, "x", qint16(aim & 0xffff));
            if (controller.axes & AxisY) integerField(L, "y", qint16((aim >> 16) & 0xffff));
            if (controller.axes & Offscreen) booleanField(L, "offscreen", (aim >> 32) & 1u);
        }
        return 1;
    }

    bool maySetInput() const {
        return !accessEvent && owner.services_.inputAllowed &&
               (atControlBoundary() || currentEvent == ScriptEvent::BeforeFrame);
    }

    static int setInput(lua_State* L) {
        auto& script = self(L);
        script.charge(L);
        const auto player = playerNumber(L, 1);
        luaL_checktype(L, 2, LUA_TTABLE);
        if (!script.maySetInput()) return unavailable(L, "Input override unavailable here");
        const auto kind = script.inputDevice(player);
        if (kind == InputDisplayDevice::None) return unavailable(L, "Player is disconnected");
        const auto& controller = Controllers[size_t(kind)];
        auto patch = script.overrides[player];
        if (patch.device != kind) {
            patch = {};
            patch.device = kind;
        }
        lua_pushnil(L);
        while (lua_next(L, 2)) {
            script.charge(L);
            if (lua_type(L, -2) != LUA_TSTRING)
                return luaL_argerror(L, 2, "expected named controller fields");
            const auto field = checkedString(L, -2, 16);
            const auto button = std::find_if(controller.buttons.begin(), controller.buttons.end(),
                                             [field](const char* name) { return name && field == name; });
            if (button != controller.buttons.end()) {
                const auto bit = quint16(1u << (button - controller.buttons.begin()));
                patch.mask |= bit;
                patch.buttons = (patch.buttons & ~bit) | (flag(L, -1) ? bit : 0);
            } else if ((field == "x" && (controller.axes & AxisX)) ||
                       (field == "y" && (controller.axes & AxisY))) {
                if (lua_type(L, -1) == LUA_TSTRING) checkedString(L, -1, 32);
                const auto value = luaL_checkinteger(L, -1);
                const int minimum = (controller.axes & Offscreen) ? -32767 : -32768;
                if (value < minimum || value > 32767)
                    return luaL_argerror(L, 2, "controller coordinate out of range");
                const int shift = field == "x" ? 0 : 16;
                const auto mask = quint64(0xffff) << shift;
                patch.aimMask |= mask;
                patch.aim = (patch.aim & ~mask) | (quint64(quint16(value)) << shift);
            } else if (field == "offscreen" && (controller.axes & Offscreen)) {
                constexpr auto mask = quint64(1) << 32;
                patch.aimMask |= mask;
                patch.aim = (patch.aim & ~mask) | (flag(L, -1) ? mask : 0);
            } else {
                return luaL_argerror(L, 2, "field is unsupported for this controller");
            }
            lua_pop(L, 1);
        }
        script.overrides[player] = patch;
        return accepted(L);
    }

    static int clearInput(lua_State* L) {
        auto& script = self(L);
        script.charge(L);
        if (!script.maySetInput()) return unavailable(L, "Input override unavailable here");
        if (lua_isnoneornil(L, 1)) script.overrides.fill({});
        else script.overrides[playerNumber(L, 1)] = {};
        return accepted(L);
    }

    bool maySave() const {
        return atControlBoundary() && owner.services_.saveAllowed;
    }

    Save* saveObject(lua_State* L) {
        if (lua_type(L, 1) == LUA_TSTRING) checkedString(L, 1, 32);
        const auto handle = luaL_checkinteger(L, 1);
        const auto found = std::find_if(saves.begin(), saves.end(), [handle](const Save& save) {
            return save.handle != 0 && save.handle == handle;
        });
        return found != saves.end() ? &*found : nullptr;
    }

    static int createSave(lua_State* L) {
        auto& script = self(L);
        script.charge(L);
        if (!script.maySave()) return unavailable(L, "Save objects unavailable here");
        const auto found = std::find_if(script.saves.begin(), script.saves.end(),
                                        [](const Save& save) { return !save.handle; });
        if (found == script.saves.end() || script.nextSave == std::numeric_limits<lua_Integer>::max())
            return unavailable(L, "Save object limit reached");
        found->handle = ++script.nextSave;
        lua_pushinteger(L, found->handle);
        return 1;
    }

    bool captureSave(Save& save) {
        capturedSave = true;
        if (!owner.saveBytes_) owner.saveBytes_ = std::make_shared<qsizetype>(0);
        const auto remaining = std::min(ScriptLimits::MaxSaveObjectBytes,
                                        ScriptLimits::MaxSaveBytes - *owner.saveBytes_);
        if (remaining <= 0) {
            saveError = QStringLiteral("Script snapshot byte limit reached");
            return false;
        }
        saveError.clear();
        auto bytes = owner.services_.captureSave(remaining, saveError);
        if (bytes.isEmpty()) {
            if (saveError.isEmpty()) saveError = QStringLiteral("Unable to capture an in-memory state");
            return false;
        }
        if (bytes.size() > remaining) {
            saveError = QStringLiteral("Script snapshot byte limit reached");
            return false;
        }
        const auto storage = std::make_shared<Snapshot>(owner.saveBytes_, std::move(bytes));
        save.snapshot = std::shared_ptr<const QByteArray>(storage, &storage->bytes);
        return true;
    }

    static int saveState(lua_State* L) {
        auto& script = self(L);
        script.charge(L);
        if (!script.maySave() || !script.owner.services_.captureSave)
            return unavailable(L, "State capture unavailable here");
        auto* save = script.saveObject(L);
        if (!save) return unavailable(L, "Unknown save object");
        if (script.capturedSave) return unavailable(L, "Only one capture per script at this boundary");
        if (script.captureSave(*save)) return accepted(L);

        char message[MaxTextBytes + 1]{};
        {
            auto utf8 = script.saveError.left(MaxTextBytes).toUtf8();
            std::memcpy(message, utf8.constData(), size_t(std::min<qsizetype>(utf8.size(), MaxTextBytes)));
        }
        return unavailable(L, message);
    }

    static int loadState(lua_State* L) {
        auto& script = self(L);
        script.charge(L);
        if (!script.maySave() || !script.owner.services_.controlAllowed)
            return unavailable(L, "State restore unavailable here");
        const auto* save = script.saveObject(L);
        if (!save || !save->snapshot) return unavailable(L, "Save object is empty or unknown");
        return script.queueControl(ScriptControlType::LoadState, false, save->snapshot)
            ? accepted(L) : unavailable(L, "Control queue is full");
    }

    static int freeSave(lua_State* L) {
        auto& script = self(L);
        script.charge(L);
        if (!script.maySave()) return unavailable(L, "Save objects unavailable here");
        auto* save = script.saveObject(L);
        if (!save) return unavailable(L, "Unknown save object");
        *save = {};
        return accepted(L);
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
        script.charge(L);
        const auto side = script.instanceFor(L, 1);
        const auto* catalog = script.hardware->memoryCatalog(*script.host, side);
        if (!catalog) return unavailable(L, "console unavailable");
        const auto& info = *catalog;
        lua_createtable(L, info.domain_count, 0);
        for (unsigned i = 0; i < info.domain_count; ++i) {
            const auto& domain = info.domains[i];
            lua_createtable(L, 0, 6);
            booleanField(L, "writable", script.host->inspectionDomainWritable(side, domain.id));
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
        script.charge(L);
        const auto* console = script.console(script.instanceFor(L, 1));
        if (!console) return unavailable(L, "console unavailable");
        lua_createtable(L, int(console->cpus.size()), 0);
        for (qsizetype i = 0; i < console->cpus.size(); ++i) {
            const auto& cpu = console->cpus[i];
            lua_createtable(L, 0, 6);
            lua_createtable(L, 0, int(AccessEvents.size()));
            for (size_t event = 0; event < AccessEvents.size(); ++event)
                booleanField(L, AccessNames[event], cpu.capabilities & AccessEvents[event]);
            lua_setfield(L, -2, "hooks");
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
        script.charge(L, width);
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
        script.charge(L, int(length));
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
        script.charge(L, width);
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
        script.charge(L, sizeof(kr_debug_cpu));
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

    static int write(lua_State* L) {
        auto& script = self(L);
        const auto id = unsigned32(L, 1);
        const auto offset = unsigned32(L, 2);
        const auto width = readWidth(L, 4);
        const auto value = writeValue(L, 3, width);
        const auto side = script.instanceFor(L, 6);
        script.charge(L, width);
        if (!script.atControlBoundary() || !script.owner.services_.mutationAllowed)
            return unavailable(L, "Memory writes unavailable here");
        const auto* domain = script.domain(side, id, offset, width);
        const auto big = bigEndian(L, 5, domain && domain->big_endian != 0);
        if (!domain || quint64(offset) + width > domain->size)
            return unavailable(L, "Memory write outside an available domain");
        if (!script.host->inspectionDomainWritable(side, id))
            return unavailable(L, "Memory domain is read-only");
        auto* request = script.queueControl(ScriptControlType::MemoryWrite);
        if (!request) return unavailable(L, "Control queue is full");
        request->instance = side;
        request->target = id;
        request->offset = offset;
        request->bytes.resize(width);
        for (int i = 0; i < width; ++i) {
            const int shift = (big ? width - 1 - i : i) * 8;
            request->bytes[i] = char((value >> shift) & 0xff);
        }
        return accepted(L);
    }

    static int writableRegisters(lua_State* L) {
        auto& script = self(L);
        const auto id = unsigned32(L, 1);
        const auto side = script.instanceFor(L, 2);
        script.charge(L, sizeof(kr_debug_cpu));
        kr_debug_cpu state{};
        if (!script.cpu(side, id) || !script.host->inspectionRegisters(side, id, state))
            return unavailable(L, "CPU registers unavailable");
        lua_createtable(L, 0, state.count);
        for (unsigned i = 0; i < state.count; ++i) {
            const auto& reg = state.regs[i];
            if (script.host->inspectionRegisterWritable(side, id, reg.name))
                integerField(L, reg.name, reg.digits);
        }
        return 1;
    }

    static int setRegister(lua_State* L) {
        auto& script = self(L);
        const auto id = unsigned32(L, 1);
        luaL_checktype(L, 2, LUA_TSTRING);
        const auto name = checkedString(L, 2, sizeof(kr_debug_reg::name) - 1);
        if (name.isEmpty() || name.contains('\0'))
            return luaL_argerror(L, 2, "expected a native register name");
        const auto value = unsigned32(L, 3);
        const auto side = script.instanceFor(L, 4);
        script.charge(L, sizeof(kr_debug_cpu));
        if (!script.atControlBoundary() || !script.owner.services_.mutationAllowed)
            return unavailable(L, "Register writes unavailable here");
        kr_debug_cpu state{};
        if (!script.cpu(side, id) || !script.host->inspectionRegisters(side, id, state))
            return unavailable(L, "CPU registers unavailable");
        const auto* end = state.regs + state.count;
        const auto* reg = std::find_if(std::as_const(state).regs, end, [&](const auto& entry) { return name == entry.name; });
        if (reg == end || !script.host->inspectionRegisterWritable(side, id, reg->name))
            return unavailable(L, "Register is unavailable or read-only");
        if (reg->digits < 8 && (quint64(value) >> (reg->digits * 4)))
            return luaL_argerror(L, 3, "value does not fit the register");
        auto* request = script.queueControl(ScriptControlType::RegisterWrite);
        if (!request) return unavailable(L, "Control queue is full");
        request->instance = side;
        request->target = id;
        request->registerName = reg->name;
        request->valueBits = value;
        return accepted(L);
    }

    bool mayEditObservers() const { return atControlBoundary() && owner.services_.accessAllowed; }

    static int onAccess(lua_State* L) {
        auto& script = self(L);
        const auto id = unsigned32(L, 1);
        const auto name = checkedString(L, 2, 5);
        const auto first = unsigned32(L, 3);
        const auto last = lua_isnil(L, 4) ? first : unsigned32(L, 4);
        luaL_checktype(L, 5, LUA_TFUNCTION);
        const auto side = script.instanceFor(L, 6);
        const auto eventName = std::find_if(AccessNames.cbegin(), AccessNames.cend(),
                                           [name](const char* text) { return name == text; });
        if (eventName == AccessNames.cend()) return luaL_argerror(L, 2, "expected 'exec', 'read' or 'write'");
        if (last < first) return luaL_argerror(L, 4, "range end precedes its start");
        const auto event = AccessEvents[size_t(eventName - AccessNames.cbegin())];
        script.charge(L);
        if (!script.mayEditObservers()) return unavailable(L, "Access hooks unavailable here");
        const auto* cpu = script.cpu(side, id);
        if (!cpu || !(cpu->capabilities & event))
            return unavailable(L, "CPU or access event unavailable");
        const auto mask = debugAccessAddressMask(*cpu, event);
        if (last > mask) return unavailable(L, "Address outside CPU event range");
        if (script.accessHooks.size() >= ScriptLimits::MaxAccessHooksPerScript ||
            script.owner.accessCount_ >= ScriptLimits::MaxAccessHooks ||
            script.nextAccessHook == std::numeric_limits<lua_Integer>::max())
            return unavailable(L, "Access hook limit reached");
        lua_pushvalue(L, 5);
        const int callback = luaL_ref(L, LUA_REGISTRYINDEX);
        const auto handle = ++script.nextAccessHook;
        script.accessHooks.push_back({{side, id, event, first, last}, mask, handle, callback});
        ++script.owner.accessCount_;
        ++script.owner.accessRevision_;
        lua_pushinteger(L, handle);
        return 1;
    }

    static int offAccess(lua_State* L) {
        auto& script = self(L);
        if (lua_type(L, 1) == LUA_TSTRING) checkedString(L, 1, 32);
        const auto handle = luaL_checkinteger(L, 1);
        script.charge(L);
        if (!script.mayEditObservers()) return unavailable(L, "Access hook removal unavailable here");
        auto& hooks = script.accessHooks;
        const auto found = std::find_if(hooks.begin(), hooks.end(), [handle](const auto& hook) {
            return hook.handle == handle;
        });
        if (found == hooks.end()) return unavailable(L, "Unknown access hook");
        luaL_unref(L, LUA_REGISTRYINDEX, found->callback);
        hooks.erase(found);
        --script.owner.accessCount_;
        ++script.owner.accessRevision_;
        return accepted(L);
    }

    static int hookStats(lua_State* L) {
        auto& script = self(L);
        script.charge(L);
        if (script.starting || script.accessEvent || script.currentEvent != ScriptEvent::Frame ||
            script.accessStatsFrame != script.frame)
            return unavailable(L, "Access hook statistics require an observed completed frame");
        lua_createtable(L, 0, 2);
        integerField(L, "callbacks", script.accessCallbacks);
        booleanField(L, "truncated", script.accessTruncated);
        return 1;
    }

    static int label(lua_State* L) {
        auto& script = self(L);
        const auto id = unsigned32(L, 1);
        const auto address = unsigned32(L, 2);
        const auto name = annotationName(L, 3);
        const auto side = script.instanceFor(L, 4);
        script.charge(L, int(name.size()));
        if (!script.atControlBoundary()) return unavailable(L, "Labels unavailable here");
        const auto* cpu = script.cpu(side, id);
        if (!cpu || address > debugAccessAddressMask(*cpu, KR_DEBUG_EXEC))
            return unavailable(L, "CPU or address unavailable");
        if (script.labels.size() >= ScriptLimits::MaxAnnotationsPerKind ||
            script.owner.labelCount_ >= ScriptLimits::MaxLabels ||
            script.owner.nextLabel_ == 0x7fffffff)
            return unavailable(L, "Label limit reached");
        const auto handle = ++script.owner.nextLabel_;
        script.labels.push_back({handle, side, id, address, QString::fromUtf8(name)});
        ++script.owner.labelCount_;
        ++script.owner.annotationRevision_;
        lua_pushinteger(L, handle);
        return 1;
    }

    static int watch(lua_State* L) {
        auto& script = self(L);
        const auto id = unsigned32(L, 1);
        const auto offset = unsigned32(L, 2);
        const auto width = readWidth(L, 3);
        const auto name = annotationName(L, 4);
        const auto side = script.instanceFor(L, 5);
        script.charge(L, int(name.size()));
        if (!script.atControlBoundary()) return unavailable(L, "RAM watches unavailable here");
        const auto* domain = script.domain(side, id, offset, width);
        if (!domain || quint64(offset) + width > domain->size)
            return unavailable(L, "Watch outside an available domain");
        if (script.watches.size() >= ScriptLimits::MaxAnnotationsPerKind ||
            script.owner.watchCount_ >= DebugScriptWatchLimit ||
            script.owner.nextWatch_ == std::numeric_limits<quint32>::max())
            return unavailable(L, "Watch limit reached");
        const auto handle = ++script.owner.nextWatch_;
        script.watches.push_back({handle, side, id, offset, quint32(width),
                                  DebugWatchEntry::Unsigned, DebugWatchEntry::Native,
                                  QString::fromUtf8(name)});
        ++script.owner.watchCount_;
        ++script.owner.annotationRevision_;
        lua_pushinteger(L, handle);
        return 1;
    }

    template<bool Watch>
    static int removeAnnotation(lua_State* L) {
        auto& script = self(L);
        const auto handle = unsigned32(L, 1);
        script.charge(L);
        if (!script.atControlBoundary()) return unavailable(L, "Annotation removal unavailable here");
        auto& rows = [&]() -> auto& {
            if constexpr (Watch) return script.watches;
            else return script.labels;
        }();
        const auto found = std::find_if(rows.begin(), rows.end(), [handle](const auto& row) {
            return row.id == handle;
        });
        if (found == rows.end()) return unavailable(L, "Unknown annotation");
        rows.erase(found);
        --(Watch ? script.owner.watchCount_ : script.owner.labelCount_);
        ++script.owner.annotationRevision_;
        return accepted(L);
    }

    bool shouldDraw(lua_State* L) {
        charge(L);
        if (!draws) return false;
        if (draws->size() - overlayStart >= MaxDraws) luaL_error(L, "drawing budget exceeded");
        return true;
    }

    static int drawText(lua_State* L) {
        auto& script = self(L);
        if (!script.shouldDraw(L)) return 0;
        const auto x = coordinate(L, 1), y = coordinate(L, 2);
        const auto text = checkedString(L, 3, MaxTextBytes);
        ScriptDraw command;
        command.primitive = ScriptPrimitive::Text;
        command.space = space(L, 5);
        command.bounds = QRectF(x, y, 0, 0);
        command.color = color(L, 4, Qt::white);
        script.charge(L, int(text.size()));
        command.text = QString::fromUtf8(text);
        script.draws->push_back(std::move(command));
        return 0;
    }

    static int drawLine(lua_State* L) {
        auto& script = self(L);
        if (!script.shouldDraw(L)) return 0;
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
        if (!script.shouldDraw(L)) return 0;
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
        if (!script.shouldDraw(L)) return 0;
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
        if (script.shouldDraw(L)) {
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
        script.charge(L, cost);
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
        self(L).charge(L);
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_settop(L, 2);
        if (lua_next(L, 1)) return 2;
        lua_pushnil(L);
        return 1;
    }

    static int pairs(lua_State* L) {
        self(L).charge(L);
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
            {"on", on}, {"instances", instances}, {"screen", screen},
            {"pause", pause}, {"reset", reset}, {"frameadvance", frameAdvance},
            {"framecount", frameCount}, {"fps", framesPerSecond}, {"lagged", lagged},
            {nullptr, nullptr}
        };
        luaL_newlib(L, emu);
        integerField(L, "apiVersion", 1);
        lua_setglobal(L, "emu");
        const luaL_Reg input[] = {
            {"devices", devices}, {"get", getInput}, {"set", setInput}, {"clear", clearInput},
            {nullptr, nullptr}
        };
        luaL_newlib(L, input);
        lua_setglobal(L, "input");
        const luaL_Reg savestate[] = {
            {"create", createSave}, {"save", saveState}, {"load", loadState}, {"free", freeSave},
            {nullptr, nullptr}
        };
        luaL_newlib(L, savestate);
        lua_setglobal(L, "savestate");
        const luaL_Reg memory[] = {
            {"domains", domains}, {"cpus", cpus}, {"read", read}, {"range", range},
            {"peek", peek}, {"registers", registers}, {"writableregisters", writableRegisters},
            {"write", write}, {"setregister", setRegister},
            {"on", onAccess}, {"off", offAccess}, {"hookstats", hookStats},
            {"label", label}, {"unlabel", removeAnnotation<false>},
            {"watch", watch}, {"unwatch", removeAnnotation<true>}, {nullptr, nullptr}
        };
        luaL_newlib(L, memory);
        lua_setglobal(L, "memory");
        const luaL_Reg draw[] = {
            {"text", drawText}, {"line", drawLine}, {"rect", drawRect},
            {"circle", drawCircle}, {"image", drawImage}, {nullptr, nullptr}
        };
        luaL_newlib(L, draw);
        lua_setglobal(L, "draw");
        auto& script = self(L);
        script.topLevel = lua_newthread(L);
        script.topLevelRef = luaL_ref(L, LUA_REGISTRYINDEX);
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
        instance = core.inspectionPresentedInstance();
        frame = core.runClock().frames;
        lua_pushcfunction(L, initialize);
        int result = lua_pcall(L, 0, 0, 0);
        if (result == LUA_OK)
            result = luaL_loadbufferx(L, source.data() ? source.data() : "", size_t(source.size()), "=script", "t");
        if (result != LUA_OK) {
            reportLuaError(L);
            return false;
        }
        lua_xmove(L, topLevel, 1);
        if (!resumeTopLevel(core, catalog, true)) return false;
        report(ScriptStatus::Running);
        return true;
    }

    bool resumeTopLevel(const LibretroHost& core, Hardware& catalog, bool startup) {
        int results = 0;
        begin(topLevel, core, catalog, startup);
        const auto status = lua_resume(topLevel, L, 0, &results);
        end(topLevel);
        if (status == LUA_YIELD) return true;
        if (status != LUA_OK) {
            reportLuaError(topLevel);
            return false;
        }
        luaL_unref(L, LUA_REGISTRYINDEX, topLevelRef);
        topLevel = nullptr;
        topLevelRef = LUA_NOREF;
        return true;
    }

    static int dispatchAccess(lua_State* L) {
        const auto& script = self(L);
        const auto& access = *script.accessEvent;
        const char* name = access.event == KR_DEBUG_EXEC ? "exec"
                         : access.event == KR_DEBUG_READ ? "read" : "write";
        lua_rawgeti(L, LUA_REGISTRYINDEX, script.accessCallbackRef);
        lua_createtable(L, 0, 7);
        stringField(L, "event", name);
        integerField(L, "frame", lua_Integer(script.frame));
        integerField(L, "instance", access.instance);
        integerField(L, "cpu", access.cpu);
        integerField(L, "address", access.address);
        integerField(L, "value", access.value);
        integerField(L, "size", access.size);
        lua_call(L, 1, 0);
        return 0;
    }

    void beginAccess() {
        accessCallbacks = 0;
        accessBudget = {EventInstructions, NativeCalls, NativeBytes, 0, {}};
        accessFailed = false;
        accessTruncated = false;
        accessFramePending = true;
    }

    bool accessBudgetExhausted() const {
        return accessCallbacks >= AccessCallbacksPerScript ||
               accessBudget.instructions <= HookInterval || accessBudget.calls <= 0;
    }

    void handleAccess(const DebugAccessEvent& access, int callback, const LibretroHost& core,
                      Hardware& catalog, quint64 number, Clock::time_point limit) {
        host = &core;
        hardware = &catalog;
        // Video modes can change between two bus events in a frame.
        catalog.beginEvent();
        frame = number;
        instance = access.instance;
        accessEvent = &access;
        accessCallbackRef = callback;
        accessBudget.deadline = limit;
        // Lua restarts its count timer on each call. Charge the final partial interval too.
        accessBudget.instructions -= HookInterval;
        ++accessCallbacks;
        lua_sethook(L, instructionHook, LUA_MASKCOUNT, HookInterval);
        lua_pushcfunction(L, dispatchAccess);
        accessFailed = lua_pcall(L, 0, 0, 0) != LUA_OK;
        end(L);
        accessEvent = nullptr;
        accessCallbackRef = LUA_NOREF;
        if (accessFailed) reportLuaError(L);
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
                quint64 number, unsigned side, QVector<ScriptDraw>* overlay,
                const MovieInput* sampledInput) {
        const bool resume = event == ScriptEvent::Frame && topLevel;
        const auto callback = callbacks[size_t(event)];
        const bool dispatchCallback = callback != LUA_NOREF && callback != LUA_REFNIL;
        if (!resume && !dispatchCallback) return true;
        currentEvent = event;
        frame = number;
        instance = side;
        draws = overlay;
        input = sampledInput;
        overlayStart = overlay ? int(overlay->size()) : 0;
        if (event == ScriptEvent::Frame) capturedSave = false;
        bool ok = true;
        if (dispatchCallback) {
            begin(L, core, catalog, false);
            lua_pushcfunction(L, dispatch);
            ok = lua_pcall(L, 0, 0, 0) == LUA_OK;
            end(L);
            if (!ok) reportLuaError(L);
        }
        if (ok && resume) ok = resumeTopLevel(core, catalog, false);
        draws = nullptr;
        input = nullptr;
        if (!ok && overlay) overlay->resize(overlayStart);
        return ok;
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

void ScriptRuntime::setHostServices(ScriptHostServices services) {
    setHostPolicy(services.inputAllowed, services.controlAllowed, services.saveAllowed,
                  services.mutationAllowed, services.accessAllowed);
    services_ = std::move(services);
}

void ScriptRuntime::setHostPolicy(bool inputAllowed, bool controlAllowed, bool saveAllowed,
                                  bool mutationAllowed, bool accessAllowed) {
    if (services_.inputAllowed && !inputAllowed && hasScripts()) {
        for (const auto& script : state_->scripts) script->overrides.fill({});
    }
    services_.inputAllowed = inputAllowed;
    services_.controlAllowed = controlAllowed;
    services_.saveAllowed = saveAllowed;
    services_.mutationAllowed = mutationAllowed;
    if (services_.accessAllowed != accessAllowed && accessCount_) ++accessRevision_;
    services_.accessAllowed = accessAllowed;
}

QVector<ScriptControl> ScriptRuntime::takeControls() {
    return std::exchange(controls_, {});
}

bool ScriptRuntime::ownsControl(const ScriptControl& control) const {
    if (control.type == ScriptControlType::LoadState && control.saveRevision != saveRevision_)
        return false;
    return hasScripts() && std::any_of(state_->scripts.cbegin(), state_->scripts.cend(),
        [&](const auto& script) { return script->id == control.owner && script->run == control.run; });
}

void ScriptRuntime::controlFailed(const ScriptControl& control, const QString& error) {
    if (!hasScripts()) return;
    auto& scripts = state_->scripts;
    const auto found = std::find_if(scripts.begin(), scripts.end(), [&](const auto& script) {
        return script->id == control.owner && script->run == control.run;
    });
    if (found == scripts.end()) return;
    (*found)->report(ScriptStatus::Error, error);
    scripts.erase(found);
    if (scripts.empty()) state_.reset();
}

void ScriptRuntime::discardSavedStates() {
    ++saveRevision_;
    controls_.erase(std::remove_if(controls_.begin(), controls_.end(), [](const auto& request) {
        return request.type == ScriptControlType::LoadState;
    }), controls_.end());
    if (!hasScripts()) return;
    for (const auto& script : state_->scripts) {
        for (auto& save : script->saves) save.snapshot.reset();
    }
}

void ScriptRuntime::invalidateTimeline(bool discardSaves, quint64 preserveRun) {
    controls_.clear();
    if (discardSaves) discardSavedStates();
    if (!hasScripts()) return;
    auto& scripts = state_->scripts;
    scripts.erase(std::remove_if(scripts.begin(), scripts.end(), [&](const auto& script) {
        script->overrides.fill({});
        if (!script->topLevel || script->run == preserveRun) return false;
        script->report(ScriptStatus::Stopped, QStringLiteral("Frame advance interrupted by a timeline change"));
        return true;
    }), scripts.end());
    if (scripts.empty()) state_.reset();
}

QVector<DebugAccessSubscription> ScriptRuntime::accessSubscriptions() const {
    QVector<DebugAccessSubscription> result;
    if (!services_.accessAllowed || !accessCount_ || !hasScripts()) return result;
    result.reserve(accessCount_);
    for (const auto& script : state_->scripts) {
        for (const auto& hook : script->accessHooks) result.push_back(hook.subscription);
    }
    return result;
}

void ScriptRuntime::beginAccessFrame(const LibretroHost& host, quint64 frame) {
    if (!services_.accessAllowed || !accessCount_ || !hasScripts()) return;
    accessHost_ = &host;
    accessFrame_ = frame;
    accessCallbacks_ = 0;
    accessActiveScripts_ = 0;
    accessNanoseconds_ = 0;
    for (const auto& script : state_->scripts) {
        if (script->accessHooks.isEmpty()) continue;
        script->beginAccess();
        ++accessActiveScripts_;
    }
}

void ScriptRuntime::dispatchMemoryAccess(const DebugAccessEvent& event) {
    constexpr auto limit = std::chrono::duration_cast<std::chrono::nanoseconds>(AccessTimeLimit).count();
    if (!accessHost_ || !services_.accessAllowed || !accessActiveScripts_ || !hasScripts()) return;
    for (const auto& script : state_->scripts) {
        if (!script->accessFramePending || script->accessFailed || script->accessTruncated) continue;
        for (const auto& hook : script->accessHooks) {
            const auto& sub = hook.subscription;
            if (sub.instance != event.instance || sub.cpu != event.cpu || sub.event != event.event ||
                !debugAccessOverlaps(sub.first, sub.last, event, hook.addressMask))
                continue;

            if (script->accessBudgetExhausted() || accessCallbacks_ >= AccessCallbacksPerFrame ||
                accessNanoseconds_ >= limit) {
                script->accessTruncated = true;
                --accessActiveScripts_;
                break;
            }
            const auto started = Clock::now();
            script->handleAccess(event, hook.callback, *accessHost_, state_->hardware, accessFrame_,
                                 started + std::chrono::nanoseconds(limit - accessNanoseconds_));
            accessNanoseconds_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - started).count();
            ++accessCallbacks_;
            if (!script->accessFailed) continue;
            accessFailed_ = true;
            --accessActiveScripts_;
            break;
        }
    }
}

void ScriptRuntime::endAccessFrame() {
    const auto* host = std::exchange(accessHost_, nullptr);
    const bool failed = std::exchange(accessFailed_, false);
    if (!host || !hasScripts()) return;
    auto& scripts = state_->scripts;
    for (const auto& script : scripts) {
        if (!std::exchange(script->accessFramePending, false)) continue;
        script->accessStatsFrame = host->runClock().frames;
        if (script->accessTruncated && !script->accessTruncationReported) {
            script->accessTruncationReported = true;
            script->report(ScriptStatus::Log, Localization::text(
                QStringLiteral("Вызовы обработчиков Lua ограничены бюджетом кадра.")));
        }
    }
    if (!failed) return;
    scripts.erase(std::remove_if(scripts.begin(), scripts.end(), [](const auto& script) {
        return script->accessFailed;
    }), scripts.end());
    if (scripts.empty()) state_.reset();
}

ScriptDebugAnnotations ScriptRuntime::annotations() const {
    ScriptDebugAnnotations result;
    if (!hasScripts()) return result;
    result.labels.reserve(labelCount_);
    result.watches.reserve(watchCount_);
    for (const auto& script : state_->scripts) {
        result.labels += script->labels;
        result.watches += script->watches;
    }
    return result;
}

void ScriptRuntime::beforeFrame(const LibretroHost& host, quint64, quint64 frame, MovieInput& input) {
    if (!hasScripts() || !services_.inputAllowed) return;
    auto& hardware = state_->hardware;
    hardware.beginEvent();
    auto& scripts = state_->scripts;
    scripts.erase(std::remove_if(scripts.begin(), scripts.end(), [&](const auto& script) {
        return !script->handle(ScriptEvent::BeforeFrame, host, hardware, frame,
                               host.inspectionPresentedInstance(), nullptr, &input);
    }), scripts.end());
    if (scripts.empty()) {
        state_.reset();
        return;
    }
    for (const auto& script : scripts) {
        for (unsigned player = 0; player < script->overrides.size(); ++player) {
            const auto& patch = script->overrides[player];
            if (patch.active() && patch.device == hardware.controllers(host).players[player].device)
                patch.apply(input, player);
        }
    }
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

    const auto position = found - scripts.begin();
    if (found != scripts.end()) scripts.erase(found);
    state_->hardware.beginEvent();
    auto script = std::make_unique<Script>(*this, command);
    if (script->launch(host, state_->hardware, command.source))
        scripts.insert(scripts.begin() + position, std::move(script));
    if (scripts.empty()) state_.reset();
}

ScriptOverlayPtr ScriptRuntime::event(ScriptEvent event, const LibretroHost& host,
                                     quint64 generation, quint64 frame, int instance, bool draw,
                                     const MovieInput* input) {
    if (!hasScripts()) return {};
    QVector<ScriptDraw> commands;
    state_->hardware.beginEvent();
    auto& scripts = state_->scripts;
    scripts.erase(std::remove_if(scripts.begin(), scripts.end(), [&](const auto& script) {
        return !script->handle(event, host, state_->hardware, frame, unsigned(instance),
                               draw ? &commands : nullptr, input);
    }), scripts.end());
    if (scripts.empty()) state_.reset();
    if (commands.isEmpty()) return {};
    return std::make_shared<const ScriptOverlay>(ScriptOverlay{
        generation, frame, instance, std::move(commands)
    });
}
