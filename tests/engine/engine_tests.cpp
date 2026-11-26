// Unit tests of the portable engine (src/engine) and the PS5 pad mapping. No game, no console.
#include "engine/host.h"
#include "pad_map.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace shvi::engine;

static int g_failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

static std::uint64_t g_now = 0;
static std::vector<std::string> g_lines;
static HostConfig Config(NativeBackend* n = nullptr) {
    HostConfig c;
    c.clock = [] { return g_now; };
    c.log = [](const char* l) { g_lines.emplace_back(l); };
    c.natives = n;
    c.ownerOf = [](void* a) { return a; };  // tests use plain addresses as owners
    return c;
}
static bool Logged(const char* text) {
    for (auto& l : g_lines)
        if (l.find(text) != std::string::npos) return true;
    return false;
}

// ---- scheduler
static std::vector<int> g_trace;
static void ScriptA(void*) {
    for (int i = 0; i < 3; ++i) {
        g_trace.push_back(1);
        Scheduler::Wait(0);
    }
}
static void ScriptB(void*) {
    for (;;) {
        g_trace.push_back(2);
        Scheduler::Wait(100);
    }
}
static void Thrower(void*) { throw std::runtime_error("boom"); }

static void TestScheduler() {
    g_now = 0;
    g_trace.clear();
    Scheduler s([] { return g_now; });
    int owner = 0;
    s.Add(&owner, ScriptA, nullptr);
    s.Add(&owner, ScriptB, nullptr);
    CHECK(s.Live() == 2);
    for (int f = 0; f < 5; ++f) {
        s.Tick();
        g_now += 50;
    }
    // A runs frames 0,1,2 then ends at frame 3; B runs at t=0, then every >=100 ms (t=100, 200).
    int a = 0, b = 0;
    for (int v : g_trace) (v == 1 ? a : b)++;
    CHECK(a == 3);
    CHECK(b == 3);
    CHECK(s.Live() == 1);
    CHECK(!Scheduler::InScript());
    s.RemoveOwner(&owner);
    s.Tick();
    CHECK(s.Live() == 0);

    s.Add(&owner, Thrower, nullptr);
    s.Tick();
    CHECK(s.Crashed() == 1);
    CHECK(s.Live() == 0);

    s.Add(&owner, ScriptB, nullptr);
    s.KillAll();
    s.Tick();
    CHECK(s.Live() == 0);
}

// ---- host ABI contract
class CountingNatives final : public NativeBackend {
public:
    int calls = 0;
    std::uint64_t lastHash = 0, ret[4] = {};
    std::vector<std::uint64_t> args;
    void Begin(std::uint64_t h) override { lastHash = h; args.clear(); }
    void Push(std::uint64_t v) override { args.push_back(v); }
    std::uint64_t* Call() override {
        ++calls;
        ret[0] = 42;
        return ret;
    }
    bool Exists(std::uint64_t h) override { return h == shvi_engine_hash("PLAYER_PED_ID"); }
};

static const shvi_api* g_api;
static std::uint64_t g_seen = 0;
static void NativeScript(void*) {
    g_api->native_init(shvi_engine_hash("PLAYER_PED_ID"));
    g_api->native_push64(7);
    g_seen = g_api->native_call()[0];
    for (;;) g_api->script_wait(0);
}
static int g_ticks = 0;
static void TickFn(void*) { ++g_ticks; }
static std::vector<std::uint32_t> g_keys;
static void KeyFn(std::uint32_t vk, int down, void*) {
    if (down) g_keys.push_back(vk);
}

static void TestHost() {
    g_lines.clear();
    CountingNatives n;
    Host h(Config(&n));
    g_api = h.Api();
    CHECK(Host::Current() == &h);
    CHECK(shvi_api_compatible(g_api));
    CHECK(g_api->abi_major == SHVI_ABI_MAJOR);
    CHECK(g_api->native_exists(shvi_engine_hash("PLAYER_PED_ID")) == 1);
    CHECK(g_api->native_exists(shvi_engine_hash("NOPE")) == 0);

    // Outside a script, native_call is refused with zeros and does not reach the backend.
    g_api->native_init(1);
    CHECK(g_api->native_call()[0] == 0);
    CHECK(n.calls == 0);
    CHECK(Logged("refused"));

    int owner = 0;
    CHECK(g_api->script_register(&owner, NativeScript, nullptr) == SHVI_OK);
    CHECK(g_api->tick_register(&owner, TickFn, nullptr) == SHVI_OK);
    CHECK(g_api->keyboard_register(KeyFn, nullptr) == SHVI_OK);
    h.Frame();
    CHECK(g_seen == 42);
    CHECK(n.args.size() == 1 && n.args[0] == 7);
    CHECK(g_ticks == 1);
    h.Key(0x73, true);
    CHECK(g_keys.size() == 1 && g_keys[0] == 0x73);

    // Unsupported surfaces say so instead of crashing.
    CHECK(g_api->world_get_all(SHVI_ENTITY_PED, nullptr, 0) == SHVI_E_UNSUPPORTED);
    CHECK(g_api->present_register(nullptr, nullptr) == SHVI_E_UNSUPPORTED);

    // module_unregister removes scripts and callbacks of that owner.
    g_api->module_unregister(&owner);
    h.Frame();
    CHECK(g_ticks == 1);
    CHECK(h.Scripts().Live() == 0);

    // The guard latch stops everything and refuses new registrations.
    CHECK(g_api->script_register(&owner, NativeScript, nullptr) == SHVI_OK);
    h.Block("test");
    CHECK(g_api->is_blocked() == 1);
    CHECK(g_api->script_register(&owner, NativeScript, nullptr) == SHVI_E_BLOCKED);
    h.Frame();
    CHECK(h.Scripts().Live() <= 1);  // reaped on the next scheduler tick
    CHECK(Logged("online guard latched"));
}

// ---- PS5 pad mapping
static std::vector<std::pair<std::uint32_t, int>> g_ev;
static void Emit(std::uint32_t vk, int down, void*) { g_ev.emplace_back(vk, down); }

static void TestPad() {
    g_ev.clear();
    CHECK(shvi_ps5_pad_edges(0, SHVI_PAD_CROSS, Emit, nullptr) == 1);
    CHECK(g_ev.back().first == 0x0D && g_ev.back().second == 1);
    CHECK(shvi_ps5_pad_edges(SHVI_PAD_CROSS, 0, Emit, nullptr) == 1);
    CHECK(g_ev.back().second == 0);
    g_ev.clear();
    // R3 alone does not open the menu; L1 + R3 does (press and release of F4).
    CHECK(shvi_ps5_pad_edges(0, SHVI_PAD_R3, Emit, nullptr) == 0);
    CHECK(shvi_ps5_pad_edges(SHVI_PAD_L1, SHVI_PAD_L1 | SHVI_PAD_R3, Emit, nullptr) == 2);
    CHECK(g_ev.size() == 2 && g_ev[0].first == 0x73 && g_ev[0].second == 1 && g_ev[1].second == 0);
    g_ev.clear();
    CHECK(shvi_ps5_pad_edges(0, SHVI_PAD_UP | SHVI_PAD_LEFT, Emit, nullptr) == 2);
    CHECK(shvi_ps5_pad_edges(5, 5, Emit, nullptr) == 0);
    CHECK(shvi_ps5_pad_edges(0, SHVI_PAD_CROSS, nullptr, nullptr) == 0);
}

int main() {
    TestScheduler();
    TestHost();
    TestPad();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("engine tests passed\n");
    return 0;
}
