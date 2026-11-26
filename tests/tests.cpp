// Unit tests for the generic core. No game needed: fake memory images, fake
// native handlers, a fake clock and a fake shvi_api table.
#include "core/callbacks.h"
#include "core/crossmap.h"
#include "core/denylist.h"
#include "core/engine_call.h"
#include "core/fiber_scheduler.h"
#include "core/game_profile.h"
#include "core/probes.h"
#include "games/gta6/gta6_mode.h"
#include "core/invoker.h"
#include "core/joaat.h"
#include "core/online_guard.h"
#include "core/pattern.h"
#include "core/pe.h"
#include "shvi/shvi.hpp"
#include "games/gta6/vi_script.h"
#include "core/game_ready.h"
#include "games/gta6/vi_ecs.h"
#include "games/gta6/vi_thread_context.h"
#include "games/gta6/vi_ui_adapters.h"
#include "core/json.h"
#include "core/native_table.h"
#include "core/trampoline.h"
#include "core/overlay_drawlist.h"
#include "core/host_info.h"
#include "shvi/natives_vi.hpp"
#include "shvi/vi_ui.hpp"
#include "shvi/ui.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

static int g_failed = 0, g_checks = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(c)) { ++g_failed; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
    } while (0)

struct TestCase { const char* name; void (*fn)(); };
static std::vector<TestCase>& Tests() { static std::vector<TestCase> t; return t; }
#define TEST(n) static void n(); static const bool n##_reg = (Tests().push_back({#n, n}), true); static void n()

using namespace shvi;

// ------------------------------------------------------------------ joaat
TEST(joaat_matches_known_hashes) {
    CHECK(Joaat("adder") == 0xB779A091u);
    CHECK(Joaat("ADDER") == Joaat("adder"));
    CHECK(Joaat("player_story000_00") == 0xE472B687u);  // GTA VI, Leonida/docs/modding.md
    CHECK(Joaat("cs_mandy") == 0xD7C0CDFFu);
}

// Values computed by the game's own atStringHash (0x144FC21D0, Xbox ts
// 1794606036) through `shvi_scan --hash`: '\' hashes as '/', and a leading
// quote makes the hash stop at the closing quote.
TEST(joaat_matches_game_string_hash_on_paths_and_quotes) {
    CHECK(Joaat("a\\b") == 0x88D8A887u);
    CHECK(Joaat("a/b") == 0x88D8A887u);
    CHECK(Joaat("\"quoted\"x") == 0x29951E84u);
    CHECK(Joaat("\"quoted\"x") == Joaat("quoted"));
}

// ------------------------------------------------------------------ pattern
TEST(pattern_parse) {
    auto p = scan::Pattern::Parse("48 8D 0D ? ?? FF");
    CHECK(p && p->size() == 6);
    CHECK(p->mask[3] == 0 && p->mask[4] == 0 && p->mask[5] == 1 && p->bytes[5] == 0xFF);
    CHECK(!scan::Pattern::Parse("48 8G"));
    CHECK(!scan::Pattern::Parse(""));
}

TEST(pattern_find_and_rip) {
    // fake image: code at [0,64), data at [64,128)
    alignas(16) std::uint8_t img[128] = {};
    auto base = reinterpret_cast<std::uintptr_t>(img);
    // at 10: lea rcx, [rip + X] -> target = 10 + 7 + disp = 100
    std::uint8_t lea[] = {0x48, 0x8D, 0x0D, 0, 0, 0, 0, 0x48, 0x8B, 0x14, 0xFA};
    std::int32_t disp = 100 - (10 + 7);
    std::memcpy(lea + 3, &disp, 4);
    std::memcpy(img + 10, lea, sizeof(lea));

    scan::Scanner s({base, sizeof(img)}, {{base, 64}});
    scan::Target t{"table", {{"48 8D 0D ? ? ? ? 48 8B 14 FA", {scan::Add(3), scan::Rip()}}}};
    auto hit = s.Resolve(t);
    CHECK(hit && hit->value == base + 100 && hit->candidate == 0);
}

TEST(pattern_falls_through_ambiguous_and_validates) {
    alignas(16) std::uint8_t img[256] = {};
    auto base = reinterpret_cast<std::uintptr_t>(img);
    std::uint8_t sig[] = {0xAA, 0xBB, 0xCC, 0x01};
    std::memcpy(img + 8, sig, 4);
    sig[3] = 0x02;
    std::memcpy(img + 40, sig, 4);  // second, different match of "AA BB CC ?"
    std::uint8_t other[] = {0x11, 0x22, 0x33, 0x44};
    std::memcpy(img + 80, other, 4);

    scan::Scanner s({base, sizeof(img)}, {{base, sizeof(img)}});
    scan::Target ambiguous{"x", {{"AA BB CC ?", {}}, {"11 22 33 44", {}}}};
    auto hit = s.Resolve(ambiguous);
    CHECK(hit && hit->candidate == 1 && hit->value == base + 80);

    // validator picks the right one of the two raw matches
    scan::Target validated{"y", {{"AA BB CC ?", {}}}, true,
                           [&](std::uintptr_t v) { return *reinterpret_cast<std::uint8_t*>(v + 3) == 0x02; }};
    hit = s.Resolve(validated);
    CHECK(hit && hit->value == base + 40);

    scan::Target missing{"z", {{"DE AD BE EF", {}}}};
    auto rep = scan::ResolveAll(s, std::vector<scan::Target>{missing});
    CHECK(!rep.Ok() && rep.missingRequired.size() == 1);
}

TEST(pattern_read_i32_value_target) {
    alignas(16) std::uint8_t img[64] = {};
    auto base = reinterpret_cast<std::uintptr_t>(img);
    std::int32_t off = 0x830;
    std::memcpy(img + 4, &off, 4);
    std::uint8_t sig[] = {0x48, 0x8B, 0x04, 0xD0};
    std::memcpy(img + 8, sig, 4);
    scan::Scanner s({base, sizeof(img)}, {{base, sizeof(img)}});
    scan::Target t{"tls", {{"48 8B 04 D0", {scan::Add(-4), scan::ReadI32()}}}};
    auto hit = s.Resolve(t);
    CHECK(hit && hit->value == 0x830);
}

// ------------------------------------------------------------------ invoker
namespace fake {
void Add(rage::NativeCallContext* c) {
    auto a = static_cast<std::int64_t*>(c->args);
    static_cast<std::int64_t*>(c->ret)[0] = a[0] + a[1];
}
void Half(rage::NativeCallContext* c) {
    float f;
    std::memcpy(&f, c->args, 4);
    f *= 0.5f;
    std::memcpy(c->ret, &f, 4);
}
// GET_ENTITY_COORDS-like: returns a Vector3 in 3 slots
void Coords(rage::NativeCallContext* c) {
    rage::scrVector v{1.5f, 0, 2.5f, 0, 3.5f, 0};
    std::memcpy(c->ret, &v, sizeof(v));
}
// GET_MODEL_DIMENSIONS-like: two Vector3* out params via the fix-up buffers
void Dims(rage::NativeCallContext* c) {
    auto args = static_cast<std::uint64_t*>(c->args);
    for (int i = 0; i < 2; ++i) {
        c->outVectors[c->dataCount] = reinterpret_cast<rage::scrVector*>(args[1 + i]);
        c->inVectors[c->dataCount] = {10.f * (i + 1), 20.f * (i + 1), 30.f * (i + 1), 0};
        ++c->dataCount;
    }
}
void Crash(rage::NativeCallContext*) { *static_cast<volatile int*>(nullptr) = 1; }
}  // namespace fake

static Invoker MakeInvoker() {
    return Invoker([](std::uint64_t h) -> rage::NativeHandler {
        switch (h) {
        case 1: return fake::Add;
        case 2: return fake::Half;
        case 3: return fake::Coords;
        case 4: return fake::Dims;
        case 5: return fake::Crash;
        default: return nullptr;
        }
    });
}

TEST(invoker_args_and_returns) {
    Invoker inv = MakeInvoker();
    CHECK(inv.Invoke<int>(1, 40, 2) == 42);
    CHECK(inv.Invoke<int>(1, -5, 2) == -3);  // sign extension
    CHECK(inv.Invoke<float>(2, 3.0f) == 1.5f);
    auto v = inv.Invoke<rage::scrVector>(3);
    CHECK(v.x == 1.5f && v.y == 2.5f && v.z == 3.5f);
}

TEST(invoker_vector_fixup) {
    Invoker inv = MakeInvoker();
    rage::scrVector mn{}, mx{};
    inv.Invoke<void>(4, 0x1234u, &mn, &mx);
    CHECK(mn.x == 10.f && mn.y == 20.f && mn.z == 30.f);
    CHECK(mx.x == 20.f && mx.y == 40.f && mx.z == 60.f);
}

TEST(invoker_unresolved_fault_and_gate) {
    Invoker inv = MakeInvoker();
    CHECK(inv.Invoke<int>(999) == 0 && inv.LastCallFailed() && inv.UnresolvedCount() == 1);
    CHECK(inv.Invoke<int>(5) == 0 && inv.LastCallFailed() && inv.FaultCount() == 1);  // SEH caught
    bool open = false;
    inv.SetGate([&] { return open; });
    CHECK(inv.Invoke<int>(1, 1, 1) == 0 && inv.LastCallFailed());
    CHECK(inv.InvokeUnchecked<int>(1, 1, 1) == 2);  // guard probes bypass the gate
    open = true;
    CHECK(inv.Invoke<int>(1, 1, 1) == 2);
}

TEST(invoker_crossmap) {
    Crossmap cm;
    std::size_t bad = 0;
    CHECK(cm.Parse("; old -> new\n0xAAAA 0x1\n garbage\n0xBBBB\t0x2 # c\n", &bad) == 2 && bad == 1);
    Invoker inv = MakeInvoker();
    inv.SetCrossmap(&cm);
    CHECK(inv.Invoke<int>(0xAAAA, 2, 3) == 5);  // old hash routed to handler 1
}

// ------------------------------------------------------------------ engine calls (VI outcome c)
namespace eng {
int Add3(int a, int b, int c) { return a + b + c; }
float Scale(float v, float k) { return v * k; }
bool IsNeg(std::int64_t v) { return v < 0; }
void Store(std::uint32_t* out, std::uint32_t v) { *out = v; }
rage::scrVec3N Pos(float x) { return {x, x * 2, x * 3, 0}; }
int Boom(int) { return *static_cast<volatile int*>(nullptr); }
}  // namespace eng

static EngineTable MakeEngineTable() {
    EngineTable t;
    t.Bind<int(int, int, int)>(EngineHash("ADD3"), &eng::Add3);
    t.Bind<float(float, float)>(EngineHash("SCALE"), &eng::Scale);
    t.Bind<bool(std::int64_t)>(EngineHash("IS_NEG"), &eng::IsNeg);
    t.Bind<void(std::uint32_t*, std::uint32_t)>(EngineHash("STORE"), &eng::Store);
    t.Bind<rage::scrVec3N(float)>(EngineHash("POS"), &eng::Pos);
    t.Bind<int(int)>(EngineHash("BOOM"), &eng::Boom);
    return t;
}

TEST(engine_hash_is_tagged_case_folded_joaat) {
    CHECK(EngineHash("adder") == (kEngineHashTag | 0xB779A091ull));
    CHECK(EngineHash("ADDER") == EngineHash("adder"));
    CHECK((EngineHash("x") >> 48) == (kEngineHashTag >> 48));
}

TEST(engine_call_marshals_ints_floats_bools_pointers) {
    EngineTable t = MakeEngineTable();
    Invoker inv([&](std::uint64_t h) { return t.Find(h); });
    CHECK(inv.Invoke<int>(EngineHash("ADD3"), 40, 1, 1) == 42);
    CHECK(inv.Invoke<int>(EngineHash("ADD3"), -5, 2, 0) == -3);
    CHECK(inv.Invoke<float>(EngineHash("SCALE"), 3.0f, 0.5f) == 1.5f);
    CHECK(inv.Invoke<bool>(EngineHash("IS_NEG"), std::int64_t{-7}));
    CHECK(!inv.Invoke<bool>(EngineHash("IS_NEG"), std::int64_t{7}));
    std::uint32_t cell = 0;
    inv.Invoke<void>(EngineHash("STORE"), &cell, 0xC0FFEEu);
    CHECK(cell == 0xC0FFEEu && !inv.LastCallFailed());
}

TEST(engine_call_widens_vector_return_to_script_layout) {
    EngineTable t = MakeEngineTable();
    Invoker inv([&](std::uint64_t h) { return t.Find(h); });
    auto v = inv.Invoke<rage::scrVector>(EngineHash("POS"), 1.5f);
    CHECK(v.x == 1.5f && v.y == 3.0f && v.z == 4.5f);
}

TEST(engine_call_fault_gate_and_unknown) {
    EngineTable t = MakeEngineTable();
    Invoker inv([&](std::uint64_t h) { return t.Find(h); });
    CHECK(inv.Invoke<int>(EngineHash("BOOM"), 1) == 0 && inv.LastCallFailed() && inv.FaultCount() == 1);
    CHECK(inv.Invoke<int>(EngineHash("NOPE")) == 0 && inv.UnresolvedCount() == 1);
    inv.SetGate([] { return false; });
    CHECK(inv.Invoke<int>(EngineHash("ADD3"), 1, 1, 1) == 0 && inv.LastCallFailed());
    CHECK(t.Size() == 6 && t.Find(EngineHash("ADD3")).Valid());
}

// GTA VI native handlers take a 24-byte context (Xbox build, VM opcode 139 at
// 0x143DE7411): {args*, ret*, argCount, retCount}.
namespace vi_fake {
void Sum(rage::ViNativeContext* c) {
    std::uint64_t s = 0;
    for (std::uint32_t i = 0; i < c->argCount; ++i) s += c->args[i];
    if (c->ret && c->retCount >= 1) c->ret[0] = s;
}
void Vec(rage::ViNativeContext* c) {  // returns three floats, one per slot (script layout)
    if (!c->ret || c->retCount < 3) return;
    float xyz[3] = {1.f, 2.f, 3.f};
    for (int i = 0; i < 3; ++i) { c->ret[i] = 0; std::memcpy(&c->ret[i], &xyz[i], 4); }
}
}  // namespace vi_fake

TEST(vi_native_handler_gets_24_byte_context) {
    EngineTable t;
    t.BindViNative(EngineHash("VI_SUM"), &vi_fake::Sum);
    t.BindViNative(EngineHash("VI_VEC"), &vi_fake::Vec);
    Invoker inv([&](std::uint64_t h) { return t.Find(h); });
    CHECK(inv.Invoke<std::uint64_t>(EngineHash("VI_SUM"), 40ull, 1ull, 1ull) == 42);
    auto v = inv.Invoke<rage::scrVector>(EngineHash("VI_VEC"));
    CHECK(v.x == 1.f && v.y == 2.f && v.z == 3.f);
}

// Live run 2026-11-25: PLAYER_PED_ID's `js` lands 71 bytes into the handler, so a 64-byte read window
// finds no id. The live reader must be given at least a page.
TEST(handler_id_needs_more_than_64_bytes) {
    namespace vs = shvi::gta6::script;
    std::vector<std::uint8_t> f = {0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x83, 0x79, 0x10, 0x00,
                                   0x48, 0x89, 0xCE, 0x78, 0x37};  // push*3; sub rsp; cmp [rcx+10h],0; mov rsi,rcx; js +37h
    f.resize(71, 0x90);
    const std::uint64_t id = 0x6471F11FF69D8A77ull;
    f.insert(f.end(), {0x48, 0x8B, 0x01, 0x48, 0xB9});  // mov rax,[rcx]; mov rcx, imm64
    for (int i = 0; i < 8; ++i) f.push_back(static_cast<std::uint8_t>(id >> (8 * i)));
    f.resize(4096, 0x90);
    CHECK(!vs::HandlerId(f.data(), 64));
    CHECK(vs::HandlerId(f.data(), 4096) == id);
}

// ABI 1.5 native_exists: the same resolution a call would use (crossmap first), no call made.
TEST(invoker_has_mapped_and_abi_1_5) {
    Invoker inv = MakeInvoker();
    CHECK(inv.HasMapped(1) && inv.HasMapped(2));
    CHECK(!inv.HasMapped(0xDEAD));
    CHECK(inv.UnresolvedCount() == 0);  // asking is not a failed call
    CHECK(inv.HasMapped(1));            // and does not poison the cache
    CHECK(inv.Invoke<int>(1, 40, 2) == 42);
    CHECK(SHVI_ABI_MINOR >= 5);
    shvi_api a{};
    a.size = sizeof(shvi_api);
    CHECK(SHVI_API_HAS(&a, native_exists));
    CHECK(offsetof(shvi_api, script_function_call) == offsetof(shvi_api, native_exists) + sizeof(void*));  // 1.6 follows
    CHECK(offsetof(shvi_api, script_object_pin) == offsetof(shvi_api, script_function_call) + sizeof(void*));  // 1.7 follows
    CHECK(SHVI_ABI_MINOR >= 7 && sizeof(shvi_api) == offsetof(shvi_api, script_object_unpin) + sizeof(void*));
    {
        shvi_api old{};
        old.size = offsetof(shvi_api, script_function_call);  // a 1.5 host
        shvi::api() = &old;
        CHECK(shvi::script_function_call(1, 2, nullptr, 0, nullptr, 0) == SHVI_E_UNSUPPORTED);
    }
    a.size = offsetof(shvi_api, native_exists);  // a 1.4 host
    CHECK(!SHVI_API_HAS(&a, native_exists) && SHVI_API_HAS(&a, present_unregister));
    shvi::api() = &a;
    CHECK(!shvi::native_exists("SET_ENTITY_HEALTH"));  // the wrapper says no on an older host
    a.size = sizeof(shvi_api);
    a.native_exists = [](std::uint64_t h) { return h == shvi::sdk::EngineHash("SET_ENTITY_HEALTH") ? 1 : 0; };
    CHECK(shvi::native_exists("SET_ENTITY_HEALTH") && !shvi::native_exists("NOPE"));
}

TEST(invoker_target_still_accepts_plain_handlers) {
    NativeTarget nt = fake::Add;  // implicit from a RAGE handler
    CHECK(nt.Valid() && nt.handler == fake::Add && !nt.thunk);
    CHECK(!NativeTarget{}.Valid());
}

// ------------------------------------------------------------------ probes + GTA VI profile
namespace probe_fake {
std::uint32_t GoodHash(const char* s, std::uint32_t seed) {  // RAGE atStringHash rules
    std::uint32_t h = seed;
    bool quoted = *s == '"';
    if (quoted) ++s;
    for (; *s && !(quoted && *s == '"'); ++s) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        else if (c == '\\') c = '/';
        h += static_cast<std::uint8_t>(c);
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return h;
}
std::uint32_t CaseSensitiveHash(const char* s, std::uint32_t seed) {
    std::uint32_t h = seed;
    for (; *s; ++s) { h += static_cast<std::uint8_t>(*s); h += h << 10; h ^= h >> 6; }
    h += h << 3; h ^= h >> 11; h += h << 15;
    return h;
}
std::uint32_t Crashes(const char*, std::uint32_t) { return *static_cast<volatile std::uint32_t*>(nullptr); }
}  // namespace probe_fake

TEST(probe_string_hash_accepts_joaat_rejects_others) {
    std::string why;
    CHECK(probes::StringHashMatchesJoaat(reinterpret_cast<std::uintptr_t>(&probe_fake::GoodHash), &why));
    CHECK(!probes::StringHashMatchesJoaat(reinterpret_cast<std::uintptr_t>(&probe_fake::CaseSensitiveHash), &why) &&
          !why.empty());
    CHECK(!probes::StringHashMatchesJoaat(reinterpret_cast<std::uintptr_t>(&probe_fake::Crashes), &why));
    CHECK(!probes::StringHashMatchesJoaat(0, &why));
}

static GameProfile* FindGta6Xbox(std::vector<std::unique_ptr<GameProfile>>& v) {
    for (auto& p : v)
        if (std::string_view(p->Name()).find("Xbox") != std::string_view::npos) return p.get();
    return nullptr;
}

TEST(gta6_profiles_scan_targets_all_parse) {
    auto profiles = CreateGta6Profiles();
    CHECK(FindGta6Xbox(profiles) != nullptr);
    std::size_t n = 0;
    for (auto& p : profiles)
        for (const auto& t : p->ScanTargets())
            for (const auto& c : t.candidates) {
                ++n;
                bool ok = scan::Pattern::Parse(c.pattern).has_value();
                CHECK(ok);
                if (!ok) printf("  bad pattern in %.*s\n", int(t.name.size()), t.name.data());
            }
    CHECK(n >= 6);
}

TEST(gta6_xbox_profile_matches_exe_and_fails_closed) {
    auto profiles = CreateGta6Profiles();
    GameProfile* p = FindGta6Xbox(profiles);
    if (!p) { CHECK(false); return; }
    HostInfo host;
    host.exeName = L"GAME_SCARLETT_MASTER.EXE";
    CHECK(p->Matches(host));
    host.exeName = L"GTA6.exe";
    CHECK(!p->Matches(host));
    CHECK(!p->InstallTick([] {}));  // no tick source known: plugins never start
    Invoker inv([](std::uint64_t) { return NativeTarget{}; });
    CHECK(p->QueryMode(inv) == GameMode::Unknown);  // fail closed
}

// ------------------------------------------------------------------ callbacks (ABI 1.1 ticks, keyboard)
namespace cb {
std::vector<int> g_order;
void A(void* u) { g_order.push_back(*static_cast<int*>(u)); }
void Faulty(void*) { *static_cast<volatile int*>(nullptr) = 1; }
void Key(std::uint32_t vk, int down, void* u) {
    auto* log = static_cast<std::vector<std::uint32_t>*>(u);
    log->push_back(vk * 2 + static_cast<std::uint32_t>(down));
}
}  // namespace cb

TEST(callback_list_runs_in_order_isolates_faults_and_removes_by_owner) {
    CallbackList<> ticks;
    int one = 1, two = 2, three = 3;
    int ownerA = 0, ownerB = 0;
    ticks.Add(&ownerA, cb::A, &one);
    ticks.Add(&ownerB, cb::Faulty, nullptr);
    ticks.Add(&ownerB, cb::A, &two);
    ticks.Add(&ownerA, cb::A, &three);
    cb::g_order.clear();
    ticks.Run();
    CHECK((cb::g_order == std::vector<int>{1, 2, 3}));
    CHECK(ticks.FaultCount() == 1 && ticks.Size() == 3);  // the faulting one is dropped
    ticks.RemoveOwner(&ownerA);
    cb::g_order.clear();
    ticks.Run();
    CHECK((cb::g_order == std::vector<int>{2}) && ticks.Size() == 1);
    CHECK(!ticks.Add(&ownerA, nullptr, nullptr));
}

TEST(key_message_translation) {
    auto d = TranslateKeyMessage(WM_KEYDOWN, 'A', 0);
    CHECK(d && d->vk == 'A' && d->down);
    auto u = TranslateKeyMessage(WM_KEYUP, VK_F5, 0xC0000001);
    CHECK(u && u->vk == VK_F5 && !u->down);
    auto s = TranslateKeyMessage(WM_SYSKEYDOWN, VK_MENU, 0);
    CHECK(s && s->vk == VK_MENU && s->down);
    CHECK(!TranslateKeyMessage(WM_MOUSEMOVE, 0, 0));
    CHECK(!TranslateKeyMessage(WM_KEYDOWN, 0, 0));  // vk 0 is not a key
}

TEST(keyboard_dispatch_passes_vk_and_state) {
    CallbackList<std::uint32_t, int> keys;
    std::vector<std::uint32_t> log;
    int owner = 0;
    keys.Add(&owner, cb::Key, &log);
    keys.Run(std::uint32_t{'K'}, 1);
    keys.Run(std::uint32_t{'K'}, 0);
    CHECK((log == std::vector<std::uint32_t>{'K' * 2 + 1, 'K' * 2}));
}

// ------------------------------------------------------------------ denylist
TEST(denylist_parse_and_invoker_enforcement) {
    Denylist dl;
    std::size_t bad = 0;
    CHECK(dl.Parse("; network money natives\n0x1\n  0x0000000000000002 # trailing\nnonsense\n", &bad) == 2 &&
          bad == 1);
    CHECK(dl.Contains(1) && dl.Contains(2) && !dl.Contains(3));
    Invoker inv = MakeInvoker();
    inv.SetDenylist(&dl);
    CHECK(inv.Invoke<int>(1, 40, 2) == 0 && inv.LastCallFailed() && inv.DeniedCount() == 1);
    CHECK(inv.InvokeUnchecked<int>(1, 40, 2) == 42);  // guard probes bypass it
    Crossmap cm;
    cm.Parse("0xAAAA 0x1\n");
    inv.SetCrossmap(&cm);
    CHECK(inv.Invoke<int>(0xAAAA, 1, 1) == 0 && inv.DeniedCount() == 2);  // old hash of a denied native
    CHECK(inv.Invoke<float>(2, 3.0f) == 0.0f && inv.DeniedCount() == 3);
}

// ------------------------------------------------------------------ GTA VI mode classifier + tick gate
TEST(gta6_mode_classifier_is_fail_closed) {
    using namespace gta6;
    CHECK(ClassifyMode(false, 0, 0) == GameMode::Unknown);                                // no object yet
    CHECK(ClassifyMode(true, 0, kStoryMode) == GameMode::Unknown);                        // no mode yet
    CHECK(ClassifyMode(true, kDefaultGameMode, kStoryMode) == GameMode::Unknown);         // loading
    CHECK(ClassifyMode(true, kStoryMode, kStoryMode) == GameMode::Offline);               // story boot
    CHECK(kStoryMode == 0x5B5EEFFEu && kStoryMode == Joaat("Story000"));                  // the value seen in the world
    CHECK(ClassifyMode(true, kStoryModeDefault, kStoryModeDefault) == GameMode::Offline); // the derived default
    CHECK(ClassifyMode(true, kStoryMode, kStoryModeDefault) == GameMode::Unknown);        // mixed: not settled
    CHECK(ClassifyMode(true, kStoryMode, kOtherContentModes[0]) == GameMode::Transition); // switching content
    for (auto m : kOtherContentModes) CHECK(ClassifyMode(true, m, m) == GameMode::Transition);
    CHECK(ClassifyMode(true, 0x12345678u, 0x12345678u) == GameMode::Unknown);             // unseen value
    CHECK(ClassifyMode(true, kStoryMode, 0x12345678u) == GameMode::Unknown);
}

TEST(gta6_tick_gate_once_per_frame_on_one_thread) {
    gta6::TickGate g(4000);  // 4 ms minimum spacing, in microseconds
    CHECK(g.Allow(7, 0));          // first call pins thread 7
    CHECK(!g.Allow(7, 1000));      // second update group in the same frame
    CHECK(g.Allow(7, 16000));      // next frame
    CHECK(!g.Allow(9, 40000));     // another thread, while 7 is still active
    CHECK(g.Allow(7, 40000));
    CHECK(!g.Allow(9, 200000));    // 7 quiet for 160 ms: not long enough
    CHECK(g.Allow(9, 300000));     // quiet for 260 ms: the pin moves to 9
    CHECK(g.Thread() == 9 && g.Repins() == 1);
    CHECK(!g.Allow(7, 310000));    // and 7 is now the other thread
    CHECK(g.Allow(9, 320000));
}

// ------------------------------------------------------------------ SDK UI: menu model, layout, input, GTA V backend
namespace uit {
bool g_flag = false;
int g_presses = 0;
}  // namespace uit

static shvi::ui::Menu MakeDemoMenu(bool* toggle, int* count, float* scale, int* choice) {
    using namespace shvi::ui;
    Menu m("TRAINER");
    Page& root = m.Root();
    Page& sub = m.AddPage("SETTINGS");
    root.Button("Press me", [] { ++uit::g_presses; });
    root.Toggle("God mode", toggle);
    root.Int("Count", count, 0, 10, 2);
    root.Float("Scale", scale, 0.2f, 1.0f, 0.1f);
    root.Choice("Colour", choice, {"Red", "Green", "Blue"});
    root.Submenu("Settings", sub);
    for (int i = 0; i < 12; ++i) sub.Button("Item " + std::to_string(i), nullptr);
    return m;
}

TEST(ui_menu_opens_navigates_and_wraps) {
    using shvi::ui::Action;
    bool t = false; int c = 0; float s = 0.5f; int ch = 0;
    auto m = MakeDemoMenu(&t, &c, &s, &ch);
    CHECK(!m.IsOpen());
    m.Handle(Action::Down);                       // ignored while closed
    CHECK(m.Cursor() == 0);
    m.Handle(Action::Open);
    CHECK(m.IsOpen() && m.Cursor() == 0);
    m.Handle(Action::Up);                         // wraps to the last item
    CHECK(m.Cursor() == 5);
    m.Handle(Action::Down);                       // wraps back to the first
    CHECK(m.Cursor() == 0);
    m.Handle(Action::Select);
    CHECK(uit::g_presses == 1);
    m.Handle(Action::Open);                       // Open toggles: closes
    CHECK(!m.IsOpen());
}

TEST(ui_menu_values_toggle_clamp_and_cycle) {
    using shvi::ui::Action;
    bool t = false; int c = 9; float s = 0.95f; int ch = 2;
    auto m = MakeDemoMenu(&t, &c, &s, &ch);
    m.Handle(Action::Open);
    m.Handle(Action::Down);  // Toggle
    m.Handle(Action::Select);
    CHECK(t);
    m.Handle(Action::Right);  // Left/Right also flip a toggle
    CHECK(!t);
    m.Handle(Action::Down);  // Int 0..10 step 2
    m.Handle(Action::Right);
    CHECK(c == 10);           // clamped
    m.Handle(Action::Left);
    CHECK(c == 8);
    m.Handle(Action::Down);  // Float 0.2..1.0 step 0.1
    m.Handle(Action::Right);
    CHECK(s == 1.0f);
    m.Handle(Action::Down);  // Choice wraps
    m.Handle(Action::Right);
    CHECK(ch == 0);
    m.Handle(Action::Left);
    CHECK(ch == 2);
}

TEST(ui_menu_submenu_back_and_scroll_window) {
    using shvi::ui::Action;
    bool t = false; int c = 0; float s = 0.5f; int ch = 0;
    auto m = MakeDemoMenu(&t, &c, &s, &ch);
    m.Handle(Action::Open);
    for (int i = 0; i < 5; ++i) m.Handle(Action::Down);
    m.Handle(Action::Select);                     // enter SETTINGS
    CHECK(m.Current().Title() == "SETTINGS" && m.Cursor() == 0);
    shvi::ui::Style st;
    st.maxVisible = 5;
    for (int i = 0; i < 7; ++i) m.Handle(Action::Down);  // cursor 7
    CHECK(m.Cursor() == 7 && m.ScrollTop(st) == 3);       // window [3, 8)
    m.Handle(Action::Back);                       // back to root, cursor restored on the Submenu item
    CHECK(m.Current().Title() == "TRAINER" && m.Cursor() == 5 && m.IsOpen());
    m.Handle(Action::Back);                       // Back at root closes
    CHECK(!m.IsOpen());
}

TEST(ui_menu_layout_header_items_highlight_values_counter) {
    using namespace shvi::ui;
    bool t = true; int c = 4; float s = 0.5f; int ch = 1;
    auto m = MakeDemoMenu(&t, &c, &s, &ch);
    Style st;
    CHECK(m.Layout(st).empty());                  // nothing while closed
    m.Handle(Action::Open);
    m.Handle(Action::Down);                       // highlight the toggle
    DrawList dl = m.Layout(st);
    auto texts = [&](std::string_view s2) {
        int n = 0;
        for (auto& d : dl) n += d.kind == DrawCmd::Text && d.text == s2;
        return n;
    };
    CHECK(texts("TRAINER") == 1 && texts("God mode") == 1 && texts("ON") == 1);
    CHECK(texts("< 4 >") == 1 && texts("< 0.50 >") == 1 && texts("< Green >") == 1 && texts(">>") == 1);
    CHECK(texts("2 / 6") == 1);
    // header rect first, at the style origin
    CHECK(dl[0].kind == DrawCmd::Rect && dl[0].x == st.x && dl[0].y == st.y && dl[0].w == st.width);
    // exactly one item row uses the highlight colour, and it's row 1
    int hi = 0; float hiY = 0;
    for (auto& d : dl)
        if (d.kind == DrawCmd::Rect && d.color == st.highlight) { ++hi; hiY = d.y; }
    CHECK(hi == 1 && hiY == st.y + st.headerHeight + st.itemHeight);
    // the value text is right-aligned to the panel's inner edge
    for (auto& d : dl)
        if (d.kind == DrawCmd::Text && d.text == "ON") CHECK(d.align == Align::Right && d.x == st.x + st.width - st.padX);
}

TEST(ui_input_queue_maps_keys_across_threads) {
    using namespace shvi::ui;
    InputQueue q;  // default keys: F4 open, numpad / arrows, Enter, Backspace
    std::thread th([&] {
        InputQueue::OnKey(VK_F4, 1, &q);
        InputQueue::OnKey(VK_F4, 0, &q);          // key-up is ignored
        InputQueue::OnKey(VK_NUMPAD2, 1, &q);
        InputQueue::OnKey(VK_UP, 1, &q);
        InputQueue::OnKey('Q', 1, &q);            // unmapped
        InputQueue::OnKey(VK_RETURN, 1, &q);
        InputQueue::OnKey(VK_BACK, 1, &q);
    });
    th.join();
    std::vector<Action> got;
    for (Action a; (a = q.Pop()) != Action::None;) got.push_back(a);
    CHECK((got == std::vector<Action>{Action::Open, Action::Down, Action::Up, Action::Select, Action::Back}));
}

namespace viui_fake {
struct Call { std::uint64_t hash; std::vector<std::uint64_t> args; };
std::vector<Call> calls;
alignas(16) std::uint64_t ret[4];
void Init(std::uint64_t h) { calls.push_back({h, {}}); }
void Push(std::uint64_t v) { calls.back().args.push_back(v); }
std::uint64_t* CallFn() { return ret; }
float F(std::uint64_t u) { return shvi_u2f(u); }
}  // namespace viui_fake

TEST(ui_vi_backend_calls_engine_contract_names) {
    using namespace shvi::ui;
    namespace e = shvi::vi::engine;
    shvi_api a{};
    a.size = sizeof(a);
    a.abi_major = SHVI_ABI_MAJOR;
    a.native_init = viui_fake::Init;
    a.native_push64 = viui_fake::Push;
    a.native_call = viui_fake::CallFn;
    shvi::api() = &a;
    viui_fake::calls.clear();
    shvi::vi::EngineBackend be;
    DrawCmd r{DrawCmd::Rect};
    r.x = 0.1f; r.y = 0.2f; r.w = 0.3f; r.h = 0.1f; r.color = {1, 2, 3, 4};
    be.Draw(r);
    auto& c0 = viui_fake::calls.at(0);
    // top-left rect, colour packed 0xRRGGBBAA
    CHECK(c0.hash == e::UI_DRAW_RECT && c0.args.size() == 5);
    CHECK(viui_fake::F(c0.args[0]) == 0.1f && viui_fake::F(c0.args[1]) == 0.2f && viui_fake::F(c0.args[2]) == 0.3f);
    CHECK(c0.args[4] == 0x01020304u);

    viui_fake::calls.clear();
    DrawCmd t{DrawCmd::Text};
    t.text = "ON"; t.x = 0.5f; t.y = 0.3f; t.scale = 0.35f; t.font = 4; t.color = {255, 255, 255, 255};
    t.align = Align::Right;
    be.Draw(t);
    auto& c1 = viui_fake::calls.at(0);
    CHECK(viui_fake::calls.size() == 1 && c1.hash == e::UI_DRAW_TEXT && c1.args.size() == 7);
    CHECK(std::strcmp(reinterpret_cast<const char*>(c1.args.at(0)), "ON") == 0);
    CHECK(viui_fake::F(c1.args[3]) == 0.35f && c1.args[4] == 4 && c1.args[5] == 0xFFFFFFFFu);
    CHECK(c1.args[6] == 2);  // 0 left, 1 centre, 2 right

    viui_fake::calls.clear();
    shvi::vi::DisableMenuControls();
    CHECK(viui_fake::calls.size() == 1 && viui_fake::calls[0].hash == e::UI_DISABLE_MENU_CONTROLS);
    shvi::api() = nullptr;
}

TEST(sdk_engine_hash_matches_core) {
    // Plugins name engine functions with the SDK copy; profiles bind with the core one.
    for (const char* n : {"STRING_HASH", "UI_DRAW_RECT", "ui_draw_text", "a\\b", "\"Quoted\" tail"})
        CHECK(shvi::sdk::EngineHash(n) == shvi::EngineHash(std::string_view(n)) &&
              shvi_engine_hash(n) == shvi::EngineHash(std::string_view(n)));
    CHECK(shvi_engine_hash("STRING_HASH") == shvi::EngineHash(std::string_view("STRING_HASH")));
    static_assert(shvi::sdk::EngineHash("UI_DRAW_RECT") == shvi::EngineHash(std::string_view("UI_DRAW_RECT")));
}

// ------------------------------------------------------------------ GTA VI script runtime (Xbox build)
namespace vs = shvi::gta6::script;

TEST(vi_opcode_permutation_inverts_and_matches_engine_patcher) {
    // The engine's own script patcher (0x143E09AB0) writes raw opcode 0xDC55 for a tail jump.
    CHECK(vs::OpcodeCase(0xDC55, vs::kXboxOpcodeMul) == vs::kCaseTailJump);
    CHECK(vs::RawOpcode(vs::kCaseTailJump, vs::kXboxOpcodeMul) == 0xDC55);
    bool all = true;
    for (std::uint32_t c = 0; c < vs::kCaseCount; ++c)
        all = all && vs::OpcodeCase(vs::RawOpcode(static_cast<std::uint16_t>(c), vs::kXboxOpcodeMul),
                                    vs::kXboxOpcodeMul) == c;
    CHECK(all);
}

TEST(vi_operand_codec_matches_vm_and_engine_encoder) {
    const auto& k = vs::kXboxKeys;
    CHECK(vs::InverseMod256(0x6D) == 0x65);  // the VM multiplies by 0x6D, the patcher by 0x65 (101)
    const std::uint64_t d = 0x0123456789ABCDEFull;
    std::uint64_t e = vs::EncodeBlock(d, k.a, k.mul);
    // byte 0 by the engine encoder's formula: (101 * d) ^ 0x85
    CHECK((e & 0xFF) == ((0x65u * 0xEFu) & 0xFFu ^ 0x85u));
    CHECK(((e >> 56) & 0xFF) == ((0x65u * 0x01u) & 0xFFu ^ 0xF2u));
    CHECK(vs::DecodeBlock(e, k.a, k.mul) == d);
    // the VM's decode, byte 3: ((b ^ 0xB6) * 0x6D) & 0xFF
    std::uint64_t raw = 0x00000000AB000000ull;
    CHECK(((vs::DecodeBlock(raw, k.a, k.mul) >> 24) & 0xFF) == (((0xABu ^ 0xB6u) * 0x6Du) & 0xFFu));
    CHECK(vs::DecodeBlock(vs::EncodeBlock(d, k.b, k.mul), k.b, k.mul) == d);
}

TEST(vi_tail_jump_encodes_like_engine_patcher) {
    const std::uint64_t prog = 0x000002A1'2345'6780ull;
    const std::uint32_t off = 0x1234;
    auto j = vs::EncodeTailJump(prog, off, true, vs::kXboxKeys, vs::kXboxOpcodeMul);
    CHECK(j[0] == 0x55 && j[1] == 0xDC);
    const std::uint8_t ka[8] = {0x85, 0x94, 0xA7, 0xB6, 0xC1, 0xD0, 0xE3, 0xF2};
    const std::uint8_t kb[4] = {0x0D, 0x1C, 0x2F, 0x3E};
    bool same = true;
    for (int i = 0; i < 8; ++i)
        same = same && j[2 + i] == static_cast<std::uint8_t>((101 * static_cast<std::uint8_t>(prog >> (8 * i))) ^ ka[i]);
    for (int i = 0; i < 4; ++i)
        same = same && j[10 + i] == static_cast<std::uint8_t>((101 * static_cast<std::uint8_t>(off >> (8 * i))) ^ kb[i]);
    CHECK(same);
    auto back = vs::DecodeTailJump(j.data(), true, vs::kXboxKeys, vs::kXboxOpcodeMul);
    CHECK(back && back->program == prog && back->codeOffset == off);
    // unobfuscated programs get raw 505 and plain operands
    auto p = vs::EncodeTailJump(prog, off, false, vs::kXboxKeys, vs::kXboxOpcodeMul);
    CHECK(p[0] == 0xF9 && p[1] == 0x01 && p[2] == 0x80 && p[10] == 0x34 && p[11] == 0x12);
    CHECK(!vs::DecodeTailJump(p.data(), true, vs::kXboxKeys, vs::kXboxOpcodeMul));  // wrong mode: not a jump
}

TEST(vi_case_lengths_and_kinds_match_the_vm) {
    bool inRange = true;
    for (std::uint32_t c = 0; c < vs::kCaseCount; ++c)
        inRange = inRange && vs::kCaseLength[c] >= 2 && vs::kCaseLength[c] <= vs::kMaxInstructionLength;
    CHECK(inRange);
    // Anchors read straight from the VM cases.
    CHECK(vs::kCaseLength[vs::kCaseNativeCall] == 8 && vs::kCaseKind[vs::kCaseNativeCall] == vs::CaseKind::Native);
    CHECK(vs::kCaseLength[vs::kCaseTailJump] == vs::kTailJumpSize);  // what the engine patcher writes
    CHECK(vs::kCaseKind[vs::kCaseTailJump] == vs::CaseKind::TailJump);
    CHECK(vs::kCaseLength[48] == 10);                                  // push u64 immediate
    CHECK(vs::kCaseLength[489] == 14 && vs::kCaseLength[50] == 18);   // push 3 floats, push 4 x i32
    CHECK(vs::kCaseLength[vs::kCaseCallFar] == 7 && vs::kCaseLength[vs::kCaseCallFarShort] == 5);
    CHECK(vs::kCaseKind[vs::kCaseReturn] == vs::CaseKind::Return && vs::kCaseLength[146] == 3);
    CHECK(vs::kCaseKind[vs::kCaseSwitch] == vs::CaseKind::Switch && vs::kCaseLength[vs::kCaseSwitch] == 10);
    CHECK(vs::kCaseKind[vs::kCaseCastProgramType] == vs::CaseKind::Seq);  // not a call
    // Every branch is opcode + its relative offset, nothing else.
    int branches = 0, shortOnes = 0;
    bool shape = true;
    for (std::uint16_t c = 0; c < vs::kCaseCount; ++c) {
        auto w = vs::BranchImmBytes(c);
        if (!w) continue;
        ++branches;
        shortOnes += (w == 1);
        shape = shape && vs::kCaseLength[c] == 2 + w;
    }
    CHECK(shape && branches == 40 && shortOnes == 3);
}

TEST(vi_decoder_walks_engine_encoded_code_and_patched_entry) {
    const auto& k = vs::kXboxKeys;
    const auto mul = vs::kXboxOpcodeMul;
    std::vector<std::uint8_t> code;
    auto emit = [&](std::uint16_t c, std::uint64_t a, std::uint64_t b = 0) {
        std::uint8_t buf[18] = {};
        std::uint16_t raw = vs::RawOpcode(c, mul);
        std::uint64_t ea = vs::EncodeBlock(a, k.a, k.mul), eb = vs::EncodeBlock(b, k.b, k.mul);
        std::memcpy(buf, &raw, 2);
        std::memcpy(buf + 2, &ea, 8);
        std::memcpy(buf + 10, &eb, 8);
        code.insert(code.end(), buf, buf + vs::kCaseLength[c]);  // keys run past the instruction: truncate
    };
    // A small function: load local, branch-if-false (i8) over a push, native call, return.
    emit(1, 0x02);                               // 0:  len 3
    emit(160, static_cast<std::uint8_t>(11));    // 3:  len 3, target = 3 + 2 + 11 = 16
    emit(48, 0x1122334455667788ull);             // 6:  len 10
    emit(vs::kCaseNativeCall, 0x0102'00ABCDEFull);  // 16: handler offset 0xABCDEF, argc 2, retc 1
    emit(vs::kCaseJump, static_cast<std::uint32_t>(-23));  // 24: len 6, target = 24 + 2 - 23 = 3
    emit(vs::kCaseReturn, 0);                    // 30
    std::vector<std::uint32_t> offs;
    std::vector<vs::Instruction> ins;
    for (std::uint32_t o = 0; o < code.size();) {
        auto i = vs::DecodeAt(code.data(), code.size(), o, true, k, mul);
        if (!i) break;
        ins.push_back(*i);
        offs.push_back(o);
        o += i->length;
    }
    CHECK((offs == std::vector<std::uint32_t>{0, 3, 6, 16, 24, 30}) && code.size() == 32);
    CHECK(ins[0].caseId == 1 && ins[0].a == 0x02);
    CHECK(ins[1].kind == vs::CaseKind::Branch && ins[1].Target() == 16);
    CHECK(ins[2].a == 0x1122334455667788ull);
    CHECK(ins[3].kind == vs::CaseKind::Native && (ins[3].a & 0xFFFFFFFF) == 0xABCDEF && ((ins[3].a >> 32) & 0xFF) == 2 &&
          ((ins[3].a >> 40) & 0xFF) == 1);
    CHECK(ins[4].Target() == 3 && ins[5].kind == vs::CaseKind::Return && !ins[5].Target());
    CHECK(!vs::DecodeAt(code.data(), code.size() - 1, 30, true, k, mul));  // truncated instruction
    // The engine patcher overwrites the entry with a tail jump: the walk sees one 14-byte TailJump.
    auto tj = vs::EncodeTailJump(0x2A0'0000'1000ull, 0x40, true, k, mul);
    std::vector<std::uint8_t> patched = code;
    std::memcpy(patched.data(), tj.data(), tj.size());
    auto t = vs::DecodeAt(patched.data(), patched.size(), 0, true, k, mul);
    CHECK(t && t->kind == vs::CaseKind::TailJump && t->length == 14 && t->a == 0x2A0'0000'1000ull && t->b == 0x40);
    // Unobfuscated program: raw case numbers, plain operands.
    std::uint8_t plain[3] = {160, 0, 0xFE};
    auto u = vs::DecodeAt(plain, 3, 0, false, k, mul);
    CHECK(u && u->kind == vs::CaseKind::Branch && u->Target() == 0);
}

// Engine-encoded instructions, for the walker tests (same encoding as above).
static std::vector<std::uint8_t> EncodeProgram(const std::vector<std::pair<int, std::uint64_t>>& ins) {
    const auto& k = vs::kXboxKeys;
    std::vector<std::uint8_t> code;
    for (auto [c, a] : ins) {
        std::uint8_t buf[18] = {};
        std::uint16_t raw = vs::RawOpcode(static_cast<std::uint16_t>(c), vs::kXboxOpcodeMul);
        std::uint64_t ea = vs::EncodeBlock(a, k.a, k.mul), eb = vs::EncodeBlock(0, k.b, k.mul);
        std::memcpy(buf, &raw, 2);
        std::memcpy(buf + 2, &ea, 8);
        std::memcpy(buf + 10, &eb, 8);
        code.insert(code.end(), buf, buf + vs::kCaseLength[c]);
    }
    return code;
}

TEST(vi_walker_decodes_linearly_and_resyncs) {
    auto code = EncodeProgram({{1, 2}, {48, 0x1122334455667788ull}, {vs::kCaseNativeCall, 0x0302'0000'1234ull},
                               {vs::kCaseReturn, 0}});
    std::vector<std::uint32_t> offs;
    auto st = vs::Walk(code.data(), code.size(), 0, true, vs::kXboxKeys, vs::kXboxOpcodeMul,
                       [&](const vs::Instruction& i) { offs.push_back(i.offset); return true; });
    CHECK((offs == std::vector<std::uint32_t>{0, 3, 13, 21}) && st.instructions == 4 && st.resyncs == 0);
    CHECK(st.end == code.size());
    // Junk in front: an opcode word that decodes to a case >= 530 is skipped 2 bytes at a time.
    std::uint16_t bad = vs::RawOpcode(600, vs::kXboxOpcodeMul);
    std::vector<std::uint8_t> junk(4);
    std::memcpy(junk.data(), &bad, 2);
    std::memcpy(junk.data() + 2, &bad, 2);
    junk.insert(junk.end(), code.begin(), code.end());
    offs.clear();
    st = vs::Walk(junk.data(), junk.size(), 0, true, vs::kXboxKeys, vs::kXboxOpcodeMul,
                  [&](const vs::Instruction& i) { offs.push_back(i.offset); return true; });
    CHECK(st.resyncs == 2 && offs.size() == 4 && offs[0] == 4);
    // The callback can stop the walk.
    st = vs::Walk(code.data(), code.size(), 0, true, vs::kXboxKeys, vs::kXboxOpcodeMul,
                  [&](const vs::Instruction& i) { return i.caseId != 48; });
    CHECK(st.instructions == 2 && st.end == 3);
}

TEST(vi_native_census_collects_case_139_operands) {
    auto code = EncodeProgram({{vs::kCaseNativeCall, 0x0102'0045'6780ull}, {1, 0},
                               {vs::kCaseNativeCall, 0x0003'03BC'94F0ull}, {vs::kCaseReturn, 0}});
    std::vector<vs::NativeCall> calls;
    auto st = vs::ForEachNativeCall(code.data(), code.size(), true, vs::kXboxKeys, vs::kXboxOpcodeMul,
                                    [&](const vs::NativeCall& c) { calls.push_back(c); });
    CHECK(st.resyncs == 0 && calls.size() == 2);
    CHECK(calls[0].site == 0 && calls[0].handlerOffset == 0x00456780 && calls[0].argc == 2 && calls[0].retc == 1);
    CHECK(calls[1].site == 11 && calls[1].handlerOffset == 0x03BC94F0 && calls[1].argc == 3 && calls[1].retc == 0);
}

TEST(vi_statics_follow_the_vm_import_lookup) {
    // Root program R imports A (hash 0x2000) and B (0x9000), sorted by hash; entry 0 is R.
    alignas(16) std::uint8_t root[0x200] = {}, modA[0x200] = {};
    alignas(16) std::uint8_t imports[3 * vs::import::kStride] = {};
    alignas(16) std::uint8_t statics[0x400] = {};
    alignas(16) std::uint8_t thread[vs::thread::kBaseSize] = {};
    auto p32 = [](std::uint8_t* b, std::size_t o, std::uint32_t v) { std::memcpy(b + o, &v, 4); };
    auto p64 = [](std::uint8_t* b, std::size_t o, std::uint64_t v) { std::memcpy(b + o, &v, 8); };
    p32(root, vs::program::kNameHash, 0x5000);
    p32(root, vs::program::kImportCount, 3);
    p64(root, vs::program::kImports, reinterpret_cast<std::uintptr_t>(imports));
    p32(modA, vs::program::kNameHash, 0x2000);
    auto entry = [&](int i, std::uintptr_t prog, std::uint32_t hash, std::uint32_t off) {
        p64(imports, i * vs::import::kStride + vs::import::kProgram, prog);
        p32(imports, i * vs::import::kStride + vs::import::kNameHash, hash);
        p32(imports, i * vs::import::kStride + vs::import::kStaticOffset, off);
    };
    entry(0, reinterpret_cast<std::uintptr_t>(root), 0, 0);
    entry(1, reinterpret_cast<std::uintptr_t>(modA), 0x2000, 0x100);
    entry(2, 0, 0x9000, 0x300);
    p64(thread, vs::thread::kStatics, reinterpret_cast<std::uintptr_t>(statics));
    p64(thread, vs::thread::kRootProgram, reinterpret_cast<std::uintptr_t>(root));
    auto t = reinterpret_cast<std::uintptr_t>(thread), s = reinterpret_cast<std::uintptr_t>(statics);
    vs::ProgramView rv(reinterpret_cast<std::uintptr_t>(root));
    CHECK(rv.FindImport(0x5000) == reinterpret_cast<std::uintptr_t>(imports));  // itself: entry 0
    CHECK(rv.FindImport(0x9000) == reinterpret_cast<std::uintptr_t>(imports) + 2 * vs::import::kStride);
    CHECK(rv.FindImport(0x4000) == 0);
    CHECK(vs::StaticAddress(t, 0x5000, 8) == s + 8);
    CHECK(vs::StaticAddress(t, 0x2000, 0x10) == s + 0x110);
    CHECK(vs::StaticAddress(t, 0x9000, 0) == s + 0x300);
    CHECK(vs::StaticAddress(t, 0x1234, 0) == 0);  // not imported by the root
    p64(thread, vs::thread::kStatics, 0);
    CHECK(vs::StaticAddress(t, 0x5000, 0) == 0);  // no static block
}

TEST(vi_handler_id_reads_the_self_identification_prologue) {
    // Synthetic handler with the engine's shape (own bytes, made-up id):
    //  0 push rsi | 1 sub rsp,20h | 5 cmp dword [rcx+10h],0 | 9 js +N | 11 ... body ... | cold: mov rax,[rcx];
    //  mov rcx, imm64 ; mov [rax], rcx ; jmp back
    std::vector<std::uint8_t> f = {0x56, 0x48, 0x83, 0xEC, 0x20, 0x83, 0x79, 0x10, 0x00, 0x78, 0x00};
    std::size_t js = 9;
    for (int i = 0; i < 20; ++i) f.push_back(0x90);  // body
    f.push_back(0xC3);
    std::size_t cold = f.size();
    f[js + 1] = static_cast<std::uint8_t>(cold - (js + 2));
    const std::uint64_t id = 0x1B2FD25990E7ED74ull ^ 0x5555555555555555ull;
    f.insert(f.end(), {0x48, 0x8B, 0x01, 0x48, 0xB9});
    for (int i = 0; i < 8; ++i) f.push_back(static_cast<std::uint8_t>(id >> (8 * i)));
    f.insert(f.end(), {0x48, 0x89, 0x08, 0xEB, 0x00});
    CHECK(vs::HandlerId(f.data(), f.size()) == id);
    // the register move between cmp and js that some handlers have (mov rsi, rcx)
    std::vector<std::uint8_t> g = f;
    g.insert(g.begin() + 9, {0x48, 0x89, 0xCE});
    g[12 + 1] = static_cast<std::uint8_t>(cold + 3 - (12 + 2));
    CHECK(vs::HandlerId(g.data(), g.size()) == id);
    // not a handler: no argc test; truncated cold block
    std::uint8_t plain[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x01, 0xC3};
    CHECK(!vs::HandlerId(plain, sizeof plain));
    CHECK(!vs::HandlerId(f.data(), f.size() - 10));
}

TEST(vi_type_handles_and_builtin_names) {
    CHECK(vs::Classify(0x80000000u) == vs::TypeKind::Builtin);
    CHECK(vs::Classify(0xC0000007u) == vs::TypeKind::Dynamic);
    CHECK(vs::Classify(12u) == vs::TypeKind::ProgramLocal);
    CHECK(std::strcmp(vs::BuiltinTypeName(0x80000000u), "System.Object") == 0);
    CHECK(std::strcmp(vs::BuiltinTypeName(0x80000006u), "System.String") == 0);
    CHECK(vs::BuiltinTypeName(0x80000066u) == nullptr && vs::BuiltinTypeName(5u) == nullptr);
    // every name in the table hashes (atStringHash) to the full-name hash the engine registered
    std::size_t named = 0;
    bool ok = true;
    for (const auto& t : vs::kBuiltinTypes)
        if (t.name) { ++named; ok = ok && Joaat(t.name) == t.fullNameHash; }
    CHECK(ok && named == 101 && vs::kBuiltinTypes.size() == 102);
    CHECK(vs::kBuiltinTypes[73].name == nullptr);  // the one still unknown (T1.12)
    CHECK(std::strcmp(vs::BuiltinTypeName(0x80000000u | 71), "System.Rage") == 0);
}

TEST(vi_program_view_reads_xbox_layout) {
    alignas(16) std::uint8_t prog[0x200] = {};
    alignas(16) std::uint8_t code[32] = {};
    auto put32 = [&](std::size_t o, std::uint32_t v) { std::memcpy(prog + o, &v, 4); };
    auto put64 = [&](std::size_t o, std::uint64_t v) { std::memcpy(prog + o, &v, 8); };
    put32(vs::program::kNameHash, 0xB407AF85u);
    std::uint16_t ver = vs::program::kResourceVersion;
    std::memcpy(prog + vs::program::kResourceVersionField, &ver, 2);
    put32(vs::program::kTypeCount, 3);
    put64(vs::program::kCodeBase, reinterpret_cast<std::uintptr_t>(code));
    put32(vs::program::kImportCount, 2);
    prog[vs::program::kFlags + 1] = 0x01;  // operands obfuscated
    vs::ProgramView v(reinterpret_cast<std::uintptr_t>(prog));
    CHECK(v.LooksValid() && v.NameHash() == 0xB407AF85u && v.TypeCount() == 3 && v.ImportCount() == 2);
    CHECK(v.Code() == reinterpret_cast<std::uintptr_t>(code) && v.Obfuscated());
    prog[vs::program::kResourceVersionField] = 0x70;
    CHECK(!v.LooksValid());
}

TEST(vi_thread_view_follows_the_engine_constructor_and_frame_push) {
    namespace th = vs::thread;
    namespace fr = vs::frame;
    // A story thread as 0x141C17F00 + 0x141C180C0 build it: three inline areas.
    std::vector<std::uint8_t> mem(th::kStorySize + 16);
    std::uint8_t* t = reinterpret_cast<std::uint8_t*>((reinterpret_cast<std::uintptr_t>(mem.data()) + 15) & ~std::uintptr_t{15});
    const auto T = reinterpret_cast<std::uintptr_t>(t);
    auto put = [&](std::uintptr_t a, std::uint64_t v) { std::memcpy(reinterpret_cast<void*>(a), &v, 8); };
    auto get = [&](std::uintptr_t a) { std::uint64_t v; std::memcpy(&v, reinterpret_cast<void*>(a), 8); return v; };
    const std::uintptr_t stack = T + th::kStoryStack, locals = T + th::kStoryLocals, frames = T + th::kStoryFramesArea;
    put(T + th::kStackBegin, stack);
    put(T + th::kLocalsBegin, locals);
    put(T + th::kFramesBegin, frames);
    put(T + th::kFramesBase, frames);
    put(T + th::kFrame, frames - th::kFrameSize);
    put(T + th::kFrameBottom, frames);
    put(T + th::kFramesEnd, frames + th::kFrameSize * th::kStoryFrames);
    put(T + th::kLocalsBase, locals);
    put(T + th::kLocalsTop, locals);
    put(T + th::kLocalsEnd, locals + 8 * th::kStoryLocalSlots);
    put(T + th::kStackBase, stack);
    put(T + th::kStackEnd, stack + 8 * th::kStoryStackSlots);
    put(T + th::kStackTop, stack);
    vs::ThreadView v(T);
    CHECK(v.LooksValid() && v.FrameCount() == 0 && !v.CurrentPcOffset() && v.CurrentProgram() == 0);

    alignas(16) std::uint8_t progA[0x200] = {}, progB[0x200] = {}, codeA[64] = {}, codeB[64] = {};
    put(reinterpret_cast<std::uintptr_t>(progA) + vs::program::kCodeBase, reinterpret_cast<std::uintptr_t>(codeA));
    put(reinterpret_cast<std::uintptr_t>(progB) + vs::program::kCodeBase, reinterpret_cast<std::uintptr_t>(codeB));
    // the frame push: frame += 56; {pc = code + off, program, args = stack top, locals = locals top, saved top}
    auto push = [&](std::uint8_t* prog, std::uint8_t* code, std::uint32_t off, bool engine) {
        std::uintptr_t f = get(T + th::kFrame) + th::kFrameSize;
        put(T + th::kFrame, f);
        put(f + fr::kPc, reinterpret_cast<std::uintptr_t>(code) + off);
        put(f + fr::kProgram, reinterpret_cast<std::uintptr_t>(prog));
        put(f + fr::kArgs, get(T + th::kStackTop));
        put(f + fr::kLocals, engine ? 0 : get(T + th::kLocalsTop));
        put(f + fr::kSavedLocalsTop, get(T + th::kLocalsTop));
        put(T + th::kLocalsTop, get(T + th::kLocalsTop) + 32);
    };
    put(T + th::kStackTop, stack + 16);  // two arguments
    push(progA, codeA, 0x10, false);
    push(progB, codeB, 0x24, true);
    CHECK(v.LooksValid() && v.FrameCount() == 2);
    CHECK(v.CurrentProgram() == reinterpret_cast<std::uintptr_t>(progA));  // skips the engine frame
    CHECK(v.CurrentPcOffset() == 0x24u);
    // layout validator catches a broken thread
    put(T + th::kStackTop, stack + 8 * th::kStoryStackSlots + 8);
    CHECK(!v.LooksValid());
    put(T + th::kStackTop, stack);
    put(T + th::kFrame, frames + 3);  // not on a frame boundary
    CHECK(!v.LooksValid());
}

TEST(vi_instance_list_walks_tls_slots_to_threads) {
    // Two OS threads' TLS slots hold instances; a third slot is empty (instance gone).
    alignas(16) std::uint8_t instA[vs::instance::kSize] = {}, instB[vs::instance::kSize] = {};
    alignas(16) std::uint8_t thrA[64] = {}, thrB[64] = {};
    auto put = [](void* base, std::size_t off, std::uint64_t v) { std::memcpy(static_cast<std::uint8_t*>(base) + off, &v, 8); };
    put(instA, vs::instance::kThread, reinterpret_cast<std::uintptr_t>(thrA));
    put(instB, vs::instance::kThread, reinterpret_cast<std::uintptr_t>(thrB));
    std::uint64_t tlsSlotA = reinterpret_cast<std::uintptr_t>(instA), tlsSlotB = reinterpret_cast<std::uintptr_t>(instB),
                  tlsSlotC = 0;
    std::uint64_t items[3] = {reinterpret_cast<std::uintptr_t>(&tlsSlotA), reinterpret_cast<std::uintptr_t>(&tlsSlotC),
                              reinterpret_cast<std::uintptr_t>(&tlsSlotB)};
    alignas(16) std::uint8_t list[16] = {};
    std::uint32_t cap = 4, count = 3;
    std::memcpy(list + vs::instance_list::kCapacity, &cap, 4);
    std::memcpy(list + vs::instance_list::kCount, &count, 4);
    put(list, vs::instance_list::kItems, reinterpret_cast<std::uintptr_t>(items));
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> seen;
    auto n = vs::ForEachInstance(reinterpret_cast<std::uintptr_t>(list),
                                 [&](std::uintptr_t i, std::uintptr_t t) { seen.emplace_back(i, t); });
    CHECK(n == 2 && seen.size() == 2);
    CHECK(seen[0].first == reinterpret_cast<std::uintptr_t>(instA) && seen[0].second == reinterpret_cast<std::uintptr_t>(thrA));
    CHECK(seen[1].first == reinterpret_cast<std::uintptr_t>(instB) && seen[1].second == reinterpret_cast<std::uintptr_t>(thrB));
    count = 9;  // count > capacity: a broken list is not walked
    std::memcpy(list + vs::instance_list::kCount, &count, 4);
    CHECK(vs::ForEachInstance(reinterpret_cast<std::uintptr_t>(list), [&](std::uintptr_t, std::uintptr_t) {}) == 0);
    static_assert(vs::instance::kSize == 912 && vs::instance::kThread + 8 <= vs::instance::kSize);
}

TEST(vi_ecs_component_lookup_matches_engine_formula) {
    namespace ecs = shvi::gta6::ecs;
    // World record with 72 slots per entity (the fixed lookups go up to slot 71).
    constexpr std::uint32_t kWidth = 72, kEntities = 4;
    std::vector<std::uint16_t> index(kWidth * kEntities, ecs::world::kNone);
    alignas(16) std::uint8_t compA[3 * 0x40] = {}, compB[2 * 0x100] = {};
    struct Storage { std::uint64_t base, stride; };
    std::vector<Storage> storage(kWidth, Storage{0, 0});
    storage[44] = {reinterpret_cast<std::uintptr_t>(compA), 0x40};
    storage[70] = {reinterpret_cast<std::uintptr_t>(compB), 0x100};
    index[2 * kWidth + 44] = 2;  // entity index 2 has slot 44 -> element 2
    index[2 * kWidth + 70] = 1;  // and slot 70 -> element 1
    index[1 * kWidth + 44] = 0;
    alignas(16) std::uint8_t rec[ecs::world::kRecordSize] = {};
    std::memcpy(rec + ecs::world::kSlotsPerEntity, &kWidth, 4);
    std::uint64_t p = reinterpret_cast<std::uintptr_t>(index.data());
    std::memcpy(rec + ecs::world::kIndexTable, &p, 8);
    p = reinterpret_cast<std::uintptr_t>(storage.data());
    std::memcpy(rec + ecs::world::kStorage, &p, 8);
    const auto R = reinterpret_cast<std::uintptr_t>(rec);
    const std::uint32_t id2 = (ecs::kMainWorld << ecs::kWorldShift) | 2, id1 = (ecs::kMainWorld << ecs::kWorldShift) | 1;
    CHECK(ecs::World(id2) == 3 && ecs::Index(id2) == 2 && (id2 & 0x3E0000) == 0x60000);  // what the engine tests
    CHECK(ecs::Component(R, id2, 44) == reinterpret_cast<std::uintptr_t>(compA) + 2 * 0x40);
    CHECK(ecs::Component(R, id2, 70) == reinterpret_cast<std::uintptr_t>(compB) + 0x100);
    CHECK(ecs::Component(R, id1, 44) == reinterpret_cast<std::uintptr_t>(compA));
    CHECK(ecs::Component(R, id1, 70) == 0);   // 0xFFFF: no component
    CHECK(ecs::Component(R, id2, 99) == 0);   // past the row width
    std::uint32_t zero = 0;
    std::memcpy(rec + ecs::world::kSlotsPerEntity, &zero, 4);
    CHECK(ecs::Component(R, id2, 44) == 0);   // unused world
}

namespace {
// A fake pool with the engine's layout: +0 base, +8 u32 stride, +0x28 u32* info, +0x30 u32 required flags.
struct FakePool {
    alignas(16) std::uint8_t raw[shvi::gta6::ecs::pool::kSize] = {};
    void Set(std::uintptr_t base, std::uint32_t stride, const std::uint32_t* info, std::uint32_t mask) {
        namespace pl = shvi::gta6::ecs::pool;
        std::uint64_t b = base, i = reinterpret_cast<std::uintptr_t>(info);
        std::memcpy(raw + pl::kBase, &b, 8);
        std::memcpy(raw + pl::kStride, &stride, 4);
        std::memcpy(raw + pl::kInfo, &i, 8);
        std::memcpy(raw + pl::kRequired, &mask, 4);
    }
    std::uintptr_t addr() const { return reinterpret_cast<std::uintptr_t>(raw); }
};
}  // namespace

TEST(vi_pool_handle_round_trips_like_0x140D36890) {
    namespace ecs = shvi::gta6::ecs;
    alignas(16) std::uint8_t items[4 * 0x200] = {};
    // info: bits 17-26 generation, low bits flags; the engine ORs (info & 0xFFFE0000) into the handle.
    std::uint32_t info[4] = {0x1u, (5u << 17) | 0x3u, (0x3FFu << 17) | 0x1u, (7u << 17)};
    FakePool p;
    p.Set(reinterpret_cast<std::uintptr_t>(items), 0x200, info, 0x1);
    auto obj = [&](int i) { return reinterpret_cast<std::uintptr_t>(items) + 0x200u * i; };
    std::uint32_t h1 = ecs::PoolHandleOf(p.addr(), obj(1));
    CHECK(h1 == (1u | (5u << 17)));
    CHECK(ecs::PoolAt(p.addr(), h1) == obj(1));
    CHECK(ecs::PoolAt(p.addr(), ecs::PoolHandleOf(p.addr(), obj(2))) == obj(2));
    CHECK(ecs::PoolAt(p.addr(), 1u | (6u << 17)) == 0);  // stale generation
    CHECK(ecs::PoolAt(p.addr(), 3u | (7u << 17)) == 0);  // slot lacks the required flag
    CHECK(ecs::PoolAt(p.addr(), 0x1FFFF) == 0);          // the engine's "none"
    p.Set(0, 0x200, info, 0x1);
    CHECK(ecs::PoolAt(p.addr(), h1) == 0);  // no storage
}

TEST(vi_pool_walk_lists_live_items_like_0x140518D30) {
    namespace ecs = shvi::gta6::ecs;
    alignas(16) std::uint8_t items[5 * 0x40] = {};
    // capacity 5; items 1 and 3 have the required flag, item 4 has another flag only
    std::uint32_t info[5] = {0, (2u << 17) | 0x1u, 0, (8u << 17) | 0x3u, 0x2u};
    FakePool p;
    p.Set(reinterpret_cast<std::uintptr_t>(items), 0x40, info, 0x1);
    std::uint32_t cap = 5, count = 2;
    std::memcpy(p.raw + ecs::pool::kCapacity, &cap, 4);
    std::memcpy(p.raw + ecs::pool::kCount, &count, 4);
    std::uintptr_t out[4] = {};
    CHECK(ecs::PoolCapacity(p.addr()) == 5 && ecs::PoolCount(p.addr()) == 2);
    CHECK(ecs::LiveItems(p.addr(), out, 4) == 2);
    CHECK(out[0] == reinterpret_cast<std::uintptr_t>(items) + 0x40 && out[1] == reinterpret_cast<std::uintptr_t>(items) + 0xC0);
    CHECK(ecs::LiveItems(p.addr(), out, 1) == 2);  // the total, even when out is short
    CHECK(ecs::LiveItems(p.addr(), nullptr, 0) == 2);
    p.Set(0, 0x40, info, 0x1);
    CHECK(ecs::LiveItems(p.addr(), out, 4) == 0);  // no storage
}

TEST(vi_entity_pool_lookup_by_world_like_0x143C30F40) {
    namespace ecs = shvi::gta6::ecs;
    alignas(16) std::uint8_t items[3 * 0x80] = {};
    std::uint32_t info[3] = {0, (9u << 17) | 0x4u, (9u << 17)};
    FakePool p;
    p.Set(reinterpret_cast<std::uintptr_t>(items), 0x80, info, 0x4);
    std::uint64_t table[32] = {};
    table[ecs::kMainWorld] = p.addr();
    const auto T = reinterpret_cast<std::uintptr_t>(table);
    // ECS id: bits 0-16 index, 17-21 world, 22-31 generation (compared with info bits 17-26).
    std::uint32_t id = 1u | (ecs::kMainWorld << ecs::kWorldShift) | (9u << 22);
    CHECK(ecs::Generation(id) == 9);
    CHECK(ecs::WorldPoolItem(T, id) == reinterpret_cast<std::uintptr_t>(items) + 0x80);
    CHECK(ecs::WorldPoolItem(T, id ^ (1u << 22)) == 0);   // wrong generation
    CHECK(ecs::WorldPoolItem(T, (id & ~0x1FFFFu) | 2) == 0);  // required flag missing
    CHECK(ecs::WorldPoolItem(T, (id & ~(0x1Fu << 17)) | (4u << 17)) == 0);  // world without a pool
    CHECK(ecs::WorldPoolItem(T, id | 0x1FFFF) == 0);
}

TEST(vi_entity_matrix_offsets_like_0x141B4F130_and_0x145B8F760) {
    namespace ecs = shvi::gta6::ecs;
    // Heading 90 degrees: right = +y, forward = -x, up = +z; entity at world (100, 50, 7).
    ecs::Matrix m{{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}};
    ecs::Vec3 pos{100, 50, 7};
    auto w = ecs::LocalToWorld(m, pos, {2, 3, 1});  // 2 m right, 3 m ahead, 1 m up
    CHECK(w.x == 97 && w.y == 52 && w.z == 8);
    auto l = ecs::WorldToLocal(m, pos, w);
    CHECK(l.x == 2 && l.y == 3 && l.z == 1);
    CHECK(ecs::UprightValue(m) == 1.0f);
    alignas(16) std::uint8_t obj[0xA0] = {};
    float rows[12] = {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0};
    std::memcpy(obj + ecs::object::kMatrix, rows, sizeof rows);
    ecs::Matrix r = ecs::ReadMatrix(reinterpret_cast<std::uintptr_t>(obj));
    CHECK(r.right.y == 1 && r.forward.x == -1 && r.up.z == 1);
}

TEST(vi_packed_position_decodes_like_0x144DC3790) {
    namespace ecs = shvi::gta6::ecs;
    // The SIMD decode: lanes x,y get + 32 * int16 at bytes 12/14; z is the float at bytes 8..11.
    ecs::PackedPosition p{3.5f, -10.25f, 41.0f, 120, -37};
    std::uint8_t raw[16];
    std::memcpy(raw, &p, 16);
    std::int16_t sx, sy;
    float fx, fy, fz;
    std::memcpy(&fx, raw + 0, 4);
    std::memcpy(&fy, raw + 4, 4);
    std::memcpy(&fz, raw + 8, 4);
    std::memcpy(&sx, raw + 12, 2);
    std::memcpy(&sy, raw + 14, 2);
    auto w = ecs::Decode(p);
    CHECK(w.x == fx + 32.0f * sx && w.y == fy + 32.0f * sy && w.z == fz);
    CHECK(w.x == 3843.5f && w.y == -1194.25f && w.z == 41.0f);
    auto q = ecs::Encode({-2501.75f, 6000.0f, -3.0f});
    auto back = ecs::Decode(q);
    CHECK(back.x == -2501.75f && back.y == 6000.0f && back.z == -3.0f);
    CHECK(q.x >= -16.0f && q.x <= 16.0f && q.y >= -16.0f && q.y <= 16.0f);
    static_assert(ecs::object::kEntityId == 156 && ecs::object::kPosition == 112);
}

// ------------------------------------------------------------------ fibers
TEST(fiber_wait_and_order) {
    std::uint64_t now = 0;
    FiberScheduler s([&] { return now; });
    struct Ctx { std::vector<std::string>* log; const char* name; std::uint32_t ms; int runs; };
    std::vector<std::string> log;
    Ctx a{&log, "a", 0, 0}, b{&log, "b", 100, 0};
    auto fn = [](void* u) {
        auto* c = static_cast<Ctx*>(u);
        for (;;) {
            c->log->push_back(c->name);
            ++c->runs;
            FiberScheduler::Wait(c->ms);
        }
    };
    s.Add(&s, fn, &a);
    s.Add(&s, fn, &b);
    s.Tick();          // a, b
    now = 50; s.Tick();   // a
    now = 100; s.Tick();  // a, b
    CHECK(a.runs == 3 && b.runs == 2);
    CHECK((log == std::vector<std::string>{"a", "b", "a", "a", "b"}));
    CHECK(!FiberScheduler::InScript());
    CHECK(s.Live() == 2);
}

TEST(fiber_crash_isolation_and_removal) {
    FiberScheduler s([] { return std::uint64_t{0}; });
    int good = 0;
    s.Add(nullptr, [](void*) { throw std::runtime_error("boom"); }, nullptr);
    s.Add(nullptr, [](void*) { *static_cast<volatile int*>(nullptr) = 1; }, nullptr);
    int owner = 0;
    s.Add(&owner, [](void* u) { for (;;) { ++*static_cast<int*>(u); FiberScheduler::Wait(0); } }, &good);
    s.Add(nullptr, [](void*) {}, nullptr);  // returns: Done, reaped
    s.Tick();
    s.Tick();
    CHECK(good == 2);
    CHECK(s.Crashed() == 2);
    CHECK(s.Live() == 1);
    s.RemoveOwner(&owner);
    s.Tick();
    CHECK(good == 2 && s.Live() == 0);
}

TEST(fiber_kill_all) {
    FiberScheduler s([] { return std::uint64_t{0}; });
    int n = 0;
    s.Add(nullptr, [](void* u) { for (;;) { ++*static_cast<int*>(u); FiberScheduler::Wait(0); } }, &n);
    s.Tick();
    s.KillAll();
    s.Tick();
    CHECK(n == 1 && s.Live() == 0);
}

// ------------------------------------------------------------------ guard
TEST(guard_arms_after_offline_streak_and_latches) {
    OnlineGuard g;
    CHECK(!g.Allowed());
    for (int i = 0; i < OnlineGuard::kArmTicks - 1; ++i) g.Report(GameMode::Offline);
    CHECK(!g.Allowed());
    g.Report(GameMode::Offline);
    CHECK(g.Allowed());
    g.Report(GameMode::Unknown);  // loading screen: pause, re-arm needed
    CHECK(!g.Allowed() && !g.Latched());
    for (int i = 0; i < OnlineGuard::kArmTicks; ++i) g.Report(GameMode::Offline);
    CHECK(g.Allowed());
    g.Report(GameMode::Transition);
    CHECK(g.Latched() && !g.Allowed());
    for (int i = 0; i < 100; ++i) g.Report(GameMode::Offline);  // one-way
    CHECK(g.Latched() && !g.Allowed());
}

TEST(guard_preflight) {
    std::vector<std::wstring_view> ac = {L"BEClient_x64.dll"};
    std::vector<std::wstring_view> flags = {L"-StraightIntoFreemode"};
    auto none = [](std::wstring_view) { return false; };
    auto be = [](std::wstring_view m) { return m == L"BEClient_x64.dll"; };
    CHECK(OnlineGuard::Check(ac, flags, L"game.exe -windowed", none).ok);
    CHECK(!OnlineGuard::Check(ac, flags, L"game.exe", be).ok);
    CHECK(!OnlineGuard::Check(ac, flags, L"game.exe -straightintofreemode", none).ok);
}

// ------------------------------------------------------------------ pe
TEST(pe_loaded_module_and_version) {
    auto m = pe::Module::FromLoaded(GetModuleHandleW(nullptr));
    CHECK(m && !m->ExecutableRegions().empty());
    auto v = pe::GetModuleVersion(GetModuleHandleW(L"kernel32.dll"));
    CHECK(v && v->major >= 6);
}

TEST(pe_map_image_file_matches_loaded_layout) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::string err;
    auto mapped = pe::MapImageFile(path, &err);
    CHECK(mapped.has_value());
    if (!mapped) { printf("  map error: %s\n", err.c_str()); return; }
    auto loaded = pe::Module::FromLoaded(GetModuleHandleW(nullptr));
    CHECK(loaded && mapped->module.image.size == loaded->image.size);
    CHECK(mapped->module.sections.size() == loaded->sections.size());
    CHECK(mapped->preferredBase != 0 && mapped->timeDateStamp != 0);
    // Code bytes sit at the same RVA in both (x64 .text carries no base relocations here).
    auto fn = reinterpret_cast<std::uintptr_t>(&eng::Add3);
    std::uintptr_t rva = fn - loaded->image.begin;
    CHECK(std::memcmp(reinterpret_cast<const void*>(mapped->module.image.begin + rva),
                      reinterpret_cast<const void*>(fn), 16) == 0);
    CHECK(!pe::MapImageFile(L"Z:/definitely/missing.exe", &err) && !err.empty());
}

TEST(pe_image_file_imports_exports) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    pe::ImageFile exe;
    CHECK(exe.Load(self, nullptr));
    auto imps = exe.Imports();
    CHECK(std::any_of(imps.begin(), imps.end(), [](const std::string& s) { return _stricmp(s.c_str(), "KERNEL32.dll") == 0; }));

    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    pe::ImageFile ver;
    CHECK(ver.Load(std::wstring(sys) + L"\\version.dll", nullptr));
    auto ex = ver.ExportNames();
    CHECK(std::find(ex.begin(), ex.end(), "VerQueryValueW") != ex.end());
    pe::ImageFile di;
    CHECK(di.Load(std::wstring(sys) + L"\\dinput8.dll", nullptr));
    auto dx = di.ExportNames();
    CHECK(std::find(dx.begin(), dx.end(), "DirectInput8Create") != dx.end());
}

// ------------------------------------------------------------------ SDK (C++ wrapper over the C ABI)
namespace fakeapi {
std::uint64_t hash;
std::vector<std::uint64_t> pushed;
alignas(16) std::uint64_t ret[4];
void Init(std::uint64_t h) { hash = h; pushed.clear(); }
void Push(std::uint64_t v) { pushed.push_back(v); }
std::uint64_t* Call() { ret[0] = hash == 7 ? pushed[0] * 2 : 0; return ret; }
}  // namespace fakeapi

TEST(sdk_wrapper_marshals_through_c_abi) {
    shvi_api a{};
    a.size = sizeof(a);
    a.abi_major = SHVI_ABI_MAJOR;
    a.native_init = fakeapi::Init;
    a.native_push64 = fakeapi::Push;
    a.native_call = fakeapi::Call;
    CHECK(shvi_api_compatible(&a));
    shvi::api() = &a;
    CHECK(shvi::invoke<int>(7, 21) == 42);
    shvi::invoke<void>(8, 1.0f, true, -1);
    CHECK(fakeapi::pushed.size() == 3 && fakeapi::pushed[0] == 0x3F800000u && fakeapi::pushed[1] == 1 &&
          fakeapi::pushed[2] == ~0ull);
    shvi_api old = a;
    old.size = 8;
    CHECK(!shvi_api_compatible(&old));
}

TEST(sdk_abi_1_1_appends_tick_register) {
    CHECK(SHVI_ABI_MAJOR == 1 && SHVI_ABI_MINOR >= 1);
    // append-only: the new field sits right after the last 1.0 field
    CHECK(offsetof(shvi_api, tick_register) == offsetof(shvi_api, keyboard_register) + sizeof(void*));
    shvi_api host10{};
    host10.size = static_cast<std::uint32_t>(offsetof(shvi_api, tick_register));  // a 1.0 host
    host10.abi_major = 1;
    CHECK(SHVI_API_HAS(&host10, keyboard_register) && !SHVI_API_HAS(&host10, tick_register));
    host10.size = sizeof(shvi_api);
    CHECK(SHVI_API_HAS(&host10, tick_register));
}

TEST(sdk_abi_1_2_appends_entity_api) {
    CHECK(SHVI_ABI_MAJOR == 1 && SHVI_ABI_MINOR >= 2);
    CHECK(offsetof(shvi_api, world_get_all) == offsetof(shvi_api, tick_register) + sizeof(void*));
    CHECK(offsetof(shvi_api, entity_ptr) == offsetof(shvi_api, world_get_all) + sizeof(void*));
    CHECK(sizeof(shvi_entity_ref) == 16);
    shvi_api host11{};
    host11.size = static_cast<std::uint32_t>(offsetof(shvi_api, world_get_all));  // a 1.1 host
    host11.abi_major = 1;
    CHECK(SHVI_API_HAS(&host11, tick_register) && !SHVI_API_HAS(&host11, world_get_all));
    CHECK(!SHVI_API_HAS(&host11, entity_ptr));
    CHECK(SHVI_ENTITY_PED == 0 && SHVI_ENTITY_VEHICLE == 1 && SHVI_ENTITY_OBJECT == 2 && SHVI_ENTITY_PICKUP == 3);
}

TEST(ready_gate_needs_game_ready_and_guard) {
    using shvi::Readiness;
    shvi::ReadyGate gate;
    CHECK(!gate.Update(Readiness::Ready, false));     // guard not Offline yet
    CHECK(!gate.Update(Readiness::NotReady, true));   // loading: no player ped
    CHECK(!gate.Update(Readiness::Unknown, true));    // no signal: fail closed
    CHECK(gate.Update(Readiness::Ready, true));
    CHECK(gate.Changed() && gate.Open());
    CHECK(gate.Update(Readiness::Ready, true) && !gate.Changed());
    CHECK(!gate.Update(Readiness::NotReady, true));   // closes again between loads
    CHECK(gate.Changed() && !gate.Open());
    CHECK(!gate.Update(Readiness::Ready, false));     // guard wins
}

TEST(sdk_abi_1_3_appends_static_ptr) {
    CHECK(SHVI_ABI_MINOR >= 3);
    CHECK(offsetof(shvi_api, static_ptr) == offsetof(shvi_api, entity_ptr) + sizeof(void*));
    CHECK(offsetof(shvi_api, present_register) == offsetof(shvi_api, static_ptr) + sizeof(void*));  // 1.4 follows
    shvi_api host12{};
    host12.size = static_cast<std::uint32_t>(offsetof(shvi_api, static_ptr));  // a 1.2 host
    host12.abi_major = 1;
    CHECK(SHVI_API_HAS(&host12, entity_ptr) && !SHVI_API_HAS(&host12, static_ptr));
    shvi::api() = &host12;
    CHECK(shvi::static_ptr<int>(1, 0, 0) == nullptr);  // the wrapper refuses on an older host
}

// ------------------------------------------------------------------ json + native table (T2.3)
TEST(json_parses_objects_arrays_escapes_and_rejects_junk) {
    std::string err;
    auto v = json::Parse(R"({"a": [1, 2.5, "x\"yA"], "b": {"c": true, "d": null}, "e": "0x1F"})", &err);
    CHECK(v && v->IsObject() && err.empty());
    const json::Value* a = v->Get("a");
    CHECK(a && a->items.size() == 3 && a->items[1].num == 2.5 && a->items[2].str == "x\"yA");
    CHECK(v->Get("b")->Get("c")->b && v->Get("b")->Get("d")->kind == json::Value::Kind::Null);
    CHECK(json::ToU64(v->Get("e")) == 0x1Fu && json::ToU64(&a->items[0]) == 1u && !json::ToU64(&a->items[1]));
    CHECK(!json::Parse("{\"a\": }", &err) && !err.empty());
    CHECK(!json::Parse("[1, 2", &err));
    CHECK(!json::Parse("{} x", &err));
}

namespace nativetable_fake {
// A fake image with two handler-shaped functions (vi_handler_id test shape).
std::vector<std::uint8_t> Handler(std::uint64_t id) {
    std::vector<std::uint8_t> f = {0x56, 0x48, 0x83, 0xEC, 0x20, 0x83, 0x79, 0x10, 0x00, 0x78, 0x00, 0xC3};
    std::size_t cold = f.size();
    f[10] = static_cast<std::uint8_t>(cold - 11);
    f.insert(f.end(), {0x48, 0x8B, 0x01, 0x48, 0xB9});
    for (int i = 0; i < 8; ++i) f.push_back(static_cast<std::uint8_t>(id >> (8 * i)));
    f.insert(f.end(), {0x48, 0x89, 0x08, 0xEB, 0x00});
    while (f.size() < 64) f.push_back(0xCC);
    return f;
}
}  // namespace nativetable_fake

TEST(native_table_binds_by_build_and_handler_id) {
    using namespace nativetable_fake;
    std::vector<std::uint8_t> image(0x400, 0xCC);
    auto h1 = Handler(0x1111222233334444ull), h2 = Handler(0x5555666677778888ull);
    std::memcpy(image.data() + 0x100, h1.data(), h1.size());
    std::memcpy(image.data() + 0x200, h2.data(), h2.size());
    const char* text = R"({"build": {"exe": "x.exe", "pe_timestamp": 1794606036},
      "natives": {
        "GOOD_ONE": {"rva": "0x100", "id": "0x1111222233334444", "args": 1, "params": ["int"], "ret": "int"},
        "MOVED":    {"rva": "0x200", "id": "0x1111222233334444"},
        "DENIED":   {"rva": "0x200", "id": "0x5555666677778888", "deny": true},
        "OUTSIDE":  {"rva": "0x7FF0", "id": "0x1"},
        "NO_ID":    {"rva": "0x100"}
      }})";
    NativeTable t;
    std::string err;
    std::size_t skipped = 0;
    CHECK(ParseNativeTable(text, &t, &err, &skipped) && t.peTimestamp == 1794606036u && skipped == 1);
    CHECK(t.natives.size() == 4 && t.natives[0].params.size() == 1 && t.natives[2].deny);
    auto base = reinterpret_cast<std::uintptr_t>(image.data());
    auto readId = [&](std::uintptr_t fn) -> std::optional<std::uint64_t> {
        return vs::HandlerId(reinterpret_cast<const std::uint8_t*>(fn), base + image.size() - fn);
    };
    EngineTable e;
    auto rep = BindNativeTable(t, 1794606036u, base, image.size(), readId, e);
    CHECK(!rep.refused && rep.bound == 1 && rep.denied == 1 && rep.idMismatch == 1 && rep.outOfImage == 1);
    NativeTarget good = e.Find(EngineHash("GOOD_ONE"));
    CHECK(good.Valid() && good.fn == image.data() + 0x100 && good.thunk == &detail::ViNativeThunk);
    CHECK(!e.Find(EngineHash("MOVED")).Valid() && !e.Find(EngineHash("DENIED")).Valid());
    EngineTable other;
    auto wrong = BindNativeTable(t, 1794606037u, base, image.size(), readId, other);  // another build
    CHECK(wrong.refused && wrong.bound == 0 && other.Size() == 0);
    CHECK(!ParseNativeTable(R"({"natives": {}})", &t, &err));  // no build: refused
}

TEST(native_table_parses_the_shipped_crossmap) {
    NativeTable t;
    std::string err;
    std::size_t skipped = 0;
    bool ok = LoadNativeTableFile(L"" SHVI_SOURCE_DIR_W L"/crossmap/gta6_xbox_1.0.2.1.json", &t, &err);
    CHECK(ok && t.peTimestamp == 1794606036u && skipped == 0);
    std::size_t denied = 0, withParams = 0;
    for (const auto& n : t.natives) {
        denied += n.deny;
        withParams += !n.params.empty() || n.args == 0;
    }
    CHECK(t.natives.size() >= 40 && denied == 2 && withParams == t.natives.size());
}

// ------------------------------------------------------------------ trampolines (T2.9)
TEST(trampolines_stay_within_4gb_above_the_base) {
    // A base in our own image: the window above it has free space.
    auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    TrampolineAllocator a(base);
    void* p = a.Allocate(100);
    void* q = a.Allocate(16);
    CHECK(p && q && a.InRange(p, 100) && a.InRange(q, 16) && a.BlockCount() == 1);
    CHECK(reinterpret_cast<std::uintptr_t>(q) == reinterpret_cast<std::uintptr_t>(p) + 112);  // 16-aligned bump
    auto off = a.OffsetOf(p);
    CHECK(off && base + *off == reinterpret_cast<std::uintptr_t>(p));
    std::memset(p, 0xC3, 100);  // committed and writable
    CHECK(!a.OffsetOf(reinterpret_cast<void*>(base - 1)));
    CHECK(!a.InRange(reinterpret_cast<void*>(base + TrampolineAllocator::kWindow)));
    CHECK(a.InRange(reinterpret_cast<void*>(base + TrampolineAllocator::kWindow - 1)));
    // A base just under the top of user space has no room: refused, nothing out of range.
    TrampolineAllocator top(0x7FFF'FFFF'0000ull);
    CHECK(top.Allocate(64) == nullptr && top.BlockCount() == 0);
    CHECK(a.Allocate(0) == nullptr);
}

// ------------------------------------------------------------------ thread context (T2.7)
namespace threadctx_fake {
alignas(16) std::uint8_t tls[0x1500] = {};
alignas(16) std::uint8_t instance[vs::instance::kSize] = {};
alignas(16) std::uint8_t thread[0x100] = {};
std::uintptr_t Tls() { return reinterpret_cast<std::uintptr_t>(tls); }
int enters = 0, leaves = 0;
void* lastScope = nullptr;
std::uint32_t lastId = 0;
void* lastThread = nullptr;
void Enter(void* inst, void* thr, void* scope, bool activate, std::uint32_t id) {
    ++enters; lastScope = scope; lastId = id; lastThread = thr;
    CHECK(inst == instance && activate);
}
void Leave(void* scope) { ++leaves; CHECK(scope == lastScope); }
}  // namespace threadctx_fake

TEST(thread_context_switch_scope_tls_and_none) {
    using namespace threadctx_fake;
    using shvi::gta6::ThreadContextMode;
    CHECK(shvi::gta6::ParseThreadContextMode("scope\r\n") == ThreadContextMode::EngineScope);
    CHECK(shvi::gta6::ParseThreadContextMode(" tls") == ThreadContextMode::TlsSwap);
    CHECK(shvi::gta6::ParseThreadContextMode("") == ThreadContextMode::None);
    CHECK(shvi::gta6::ParseThreadContextMode("yes") == ThreadContextMode::None);
    auto inst = reinterpret_cast<std::uintptr_t>(instance), thr = reinterpret_cast<std::uintptr_t>(thread);
    std::memcpy(tls + vs::instance::kTlsSlots + 8 * 2, &inst, 8);  // slot 2 holds the tick thread's instance
    std::memcpy(instance + vs::instance::kThread, &thr, 8);
    std::uint32_t id = 0x77;
    std::memcpy(instance + vs::instance::kId, &id, 4);
    std::uintptr_t engineThread = 0xABCD;
    std::memcpy(tls + 0x1478, &engineThread, 8);
    shvi::gta6::ThreadContextEngine e{&Enter, &Leave, &Tls, 0x1478};
    {
        shvi::gta6::ScopedScriptThread s(ThreadContextMode::EngineScope, e);
        CHECK(s.Active() && enters == 1 && leaves == 0 && lastId == 0x77 && lastThread == thread);
    }
    CHECK(leaves == 1);
    {
        shvi::gta6::ScopedScriptThread s(ThreadContextMode::TlsSwap, e);
        std::uintptr_t cur;
        std::memcpy(&cur, tls + 0x1478, 8);
        CHECK(s.Active() && cur == thr);
    }
    std::uintptr_t after;
    std::memcpy(&after, tls + 0x1478, 8);
    CHECK(after == engineThread && enters == 1);  // restored; no scope calls in TLS mode
    {
        shvi::gta6::ScopedScriptThread s(ThreadContextMode::None, e);
        CHECK(!s.Active());
    }
    std::memset(tls + vs::instance::kTlsSlots, 0, 8 * vs::instance::kSlotKinds);  // no instance on this thread
    {
        shvi::gta6::ScopedScriptThread s(ThreadContextMode::EngineScope, e);
        CHECK(!s.Active() && enters == 1);
    }
}

// ------------------------------------------------------------------ UI adapter (T2.8)
namespace uiadapter_fake {
std::vector<std::array<std::uint64_t, 3>> calls;
void DisableControlAction(rage::ViNativeContext* c) {
    CHECK(c->argCount == 3 && c->retCount == 0);
    calls.push_back({c->args[0], c->args[1], c->args[2]});
}
}  // namespace uiadapter_fake

TEST(ui_disable_menu_controls_calls_disable_control_action_per_control) {
    using namespace uiadapter_fake;
    shvi::gta6::DisableControls(&DisableControlAction);
    CHECK(calls.size() == shvi::gta6::kMenuControls.size());
    bool shape = true;
    for (std::size_t i = 0; i < calls.size(); ++i)
        shape = shape && calls[i][0] == 0 && calls[i][1] == shvi::gta6::kMenuControls[i] && calls[i][2] == 0;
    CHECK(shape && calls[0][1] == Joaat("INPUT_FRONTEND_UP"));
    shvi::gta6::DisableControls(nullptr);  // not bound: nothing happens
    CHECK(calls.size() == shvi::gta6::kMenuControls.size());
}

// ------------------------------------------------------------------ generated natives (T2.10)
namespace natives_fake {
std::uint64_t hash = 0;
std::vector<std::uint64_t> pushed;
alignas(16) std::uint64_t ret[4] = {};
void Init(std::uint64_t h) { hash = h; pushed.clear(); }
void Push(std::uint64_t v) { pushed.push_back(v); }
std::uint64_t* Call() { return ret; }
}  // namespace natives_fake

TEST(generated_natives_pack_entities_and_vectors_into_cells) {
    using namespace natives_fake;
    shvi_api a{};
    a.size = sizeof(a);
    a.abi_major = SHVI_ABI_MAJOR;
    a.native_init = Init;
    a.native_push64 = Push;
    a.native_call = Call;
    shvi::api() = &a;
    namespace n = shvi::natives;
    shvi_entity_ref ped{0x1111, 0x2222};
    ret[0] = 175;
    CHECK(n::ENTITY::GET_ENTITY_HEALTH(ped) == 175 && hash == shvi::sdk::EngineHash("GET_ENTITY_HEALTH"));
    CHECK((pushed == std::vector<std::uint64_t>{0x1111, 0x2222}));
    n::ENTITY::SET_ENTITY_COORDS(ped, n::Vec3{1.0f, 2.0f, 3.0f, 0}, true, false, false, true, 0.5f);
    CHECK(pushed.size() == 9 && pushed[2] == (0x3F800000ull | (0x40000000ull << 32)) && pushed[3] == 0x40400000ull);
    CHECK(pushed[4] == 1 && pushed[7] == 1 && pushed[8] == 0x3F000000ull);
    float xyz[4] = {10.f, 20.f, 30.f, 0.f};
    std::memcpy(ret, xyz, 16);
    n::Vec3 c = n::ENTITY::GET_ENTITY_COORDS(ped, true);
    CHECK(c.x == 10.f && c.y == 20.f && c.z == 30.f && pushed.size() == 3);
    ret[0] = 1;
    CHECK(n::PAD::IS_CONTROL_JUST_PRESSED(0, Joaat("INPUT_FRONTEND_ACCEPT")) && pushed[1] == Joaat("INPUT_FRONTEND_ACCEPT"));
}

// ------------------------------------------------------------------ overlay draw list (T7.1)
TEST(drawlist_publish_moves_back_to_front) {
    overlay::DrawList d;
    d.AddRect(0.1f, 0.2f, 0.3f, 0.4f, 0xFF0000FFu);
    d.AddText("hi", 0.5f, 0.5f, 0.35f, 0xFFFFFFFFu, 1);
    CHECK(d.Front() && d.Front()->empty());
    d.Publish();
    auto f = d.Front();
    CHECK(f->size() == 2);
    if (f->size() == 2) {
        CHECK((*f)[0].kind == overlay::DrawItem::Kind::Rect && (*f)[0].x == 0.1f && (*f)[0].rgba == 0xFF0000FFu);
        CHECK((*f)[1].kind == overlay::DrawItem::Kind::Text && (*f)[1].text == "hi" && (*f)[1].align == 1);
    }
    d.Publish();
    CHECK(d.Front()->empty());
}

TEST(drawlist_front_survives_publish) {
    overlay::DrawList d;
    d.AddRect(0.5f, 0, 0, 0, 1);
    d.Publish();
    auto f = d.Front();
    std::uint64_t gen = d.Generation();
    d.AddRect(0.7f, 0, 0, 0, 2);
    d.AddRect(0.8f, 0, 0, 0, 3);
    d.Publish();
    CHECK(d.Generation() == gen + 1);
    CHECK(f->size() == 1 && (*f)[0].x == 0.5f);
    CHECK(d.Front()->size() == 2);
}

TEST(drawlist_clear_drops_pending) {
    overlay::DrawList d;
    d.AddRect(0.1f, 0.1f, 0.1f, 0.1f, 1);
    d.Publish();
    d.AddRect(0.2f, 0.2f, 0.2f, 0.2f, 1);
    std::uint64_t gen = d.Generation();
    d.Clear();
    CHECK(d.Front()->empty() && d.Generation() == gen + 1);
    d.Publish();
    CHECK(d.Front()->empty());
}

TEST(drawlist_concurrent_no_torn_frames) {
    overlay::DrawList d;
    std::atomic<bool> done{false};
    std::atomic<int> mismatches{0}, reads{0};
    std::thread consumer([&] {
        while (!done.load()) {
            auto f = d.Front();
            for (const auto& it : *f)
                if (it.x != f->front().x) { ++mismatches; break; }
            ++reads;
        }
    });
    for (int n = 0; n < 10000; ++n) {
        for (int i = 0; i < n % 7 + 1; ++i) d.AddRect(float(n), 0, 0, 0, 0);
        d.Publish();
    }
    done = true;
    consumer.join();
    CHECK(mismatches.load() == 0);
    CHECK(reads.load() > 0);
    CHECK(d.Front()->size() == 9999 % 7 + 1);
}

// ------------------------------------------------------------------ UI_DRAW_* -> overlay draw list (T7.1)
static void CallEngine(const EngineTable& t, const char* name, std::vector<std::uint64_t> args) {
    NativeTarget nt = t.Find(EngineHash(name));
    std::uint64_t ret[4] = {};
    rage::NativeCallContext c{};
    c.ret = ret;
    c.args = args.data();
    c.argCount = static_cast<std::uint32_t>(args.size());
    if (nt.thunk) nt.thunk(nt.fn, &c);
}
static EngineTable MakeUiDrawTable() {
    EngineTable t;
    t.Bind<void(float, float, float, float, std::uint32_t)>(EngineHash("UI_DRAW_RECT"), &gta6::UiDrawRect);
    t.Bind<void(const char*, float, float, float, std::int32_t, std::uint32_t, std::int32_t)>(
        EngineHash("UI_DRAW_TEXT"), &gta6::UiDrawText);
    return t;
}

TEST(ui_draw_adapters_fill_drawlist) {
    auto& d = overlay::GlobalDrawList();
    d.Clear();
    EngineTable t = MakeUiDrawTable();
    CallEngine(t, "UI_DRAW_RECT", {shvi_f2u(0.25f), shvi_f2u(0.5f), shvi_f2u(0.1f), shvi_f2u(0.2f), 0x11223344u});
    const char* menu = "MENU";
    CallEngine(t, "UI_DRAW_TEXT", {reinterpret_cast<std::uintptr_t>(menu), shvi_f2u(0.9f), shvi_f2u(0.1f),
                                   shvi_f2u(0.4f), 4, 0xFFFFFFFFu, 2});
    d.Publish();
    auto f = d.Front();
    CHECK(f->size() == 2);
    if (f->size() == 2) {
        CHECK((*f)[0].kind == overlay::DrawItem::Kind::Rect && (*f)[0].x == 0.25f && (*f)[0].h == 0.2f &&
              (*f)[0].rgba == 0x11223344u);
        CHECK((*f)[1].kind == overlay::DrawItem::Kind::Text && (*f)[1].text == "MENU" && (*f)[1].scale == 0.4f &&
              (*f)[1].align == 2 && (*f)[1].x == 0.9f);
    }
    // align is clamped to 0..2
    CallEngine(t, "UI_DRAW_TEXT", {reinterpret_cast<std::uintptr_t>(menu), 0, 0, shvi_f2u(0.4f), 0, 1,
                                   static_cast<std::uint64_t>(-3)});
    CallEngine(t, "UI_DRAW_TEXT", {reinterpret_cast<std::uintptr_t>(menu), 0, 0, shvi_f2u(0.4f), 0, 1, 9});
    d.Publish();
    f = d.Front();
    CHECK(f->size() == 2 && (*f)[0].align == 0 && (*f)[1].align == 2);
    d.Clear();
}

TEST(ui_draw_text_null_and_long) {
    auto& d = overlay::GlobalDrawList();
    d.Clear();
    gta6::UiDrawText(nullptr, 0.1f, 0.1f, 0.35f, 0, 0xFFFFFFFFu, 0);
    d.Publish();
    CHECK(d.Front()->empty());
    std::string longText(5000, 'x');
    gta6::UiDrawText(longText.c_str(), 0.1f, 0.1f, 0.35f, 0, 0xFFFFFFFFu, 0);
    d.Publish();
    CHECK(d.Front()->size() == 1 && d.Front()->at(0).text.size() == 5000);
    d.Clear();
}

TEST(ui_draw_cleared_when_gate_closes) {
    auto& d = overlay::GlobalDrawList();
    d.AddRect(0.1f, 0.1f, 0.1f, 0.1f, 1);
    overlay::FinishTickDrawList(true);
    CHECK(d.Front()->size() == 1);
    d.AddRect(0.2f, 0.2f, 0.2f, 0.2f, 1);
    overlay::FinishTickDrawList(false);
    CHECK(d.Front()->empty());
    overlay::FinishTickDrawList(true);
    CHECK(d.Front()->empty());
}

// ------------------------------------------------------------------ ABI 1.4 present_register (T7.1)
TEST(abi_1_4_present_fields) {
    CHECK(SHVI_ABI_MINOR >= 4);
    shvi_api a{};
    a.size = sizeof(shvi_api);
    CHECK(SHVI_API_HAS(&a, present_register));
    CHECK(SHVI_API_HAS(&a, present_unregister));
    a.size = offsetof(shvi_api, present_register);
    CHECK(!SHVI_API_HAS(&a, present_register));
    CHECK(SHVI_API_HAS(&a, static_ptr));
    CHECK(offsetof(shvi_api, native_exists) == offsetof(shvi_api, present_unregister) + sizeof(void*));  // 1.5 follows
    shvi_present_info info{};
    info.struct_size = sizeof(info);
    CHECK(offsetof(shvi_present_info, swap_chain) == 16 && offsetof(shvi_present_info, back_buffer_index) == 48);
    CHECK(sizeof(shvi_present_info) == 56);
}

// ------------------------------------------------------------------ script_call on VIctory's live image
// Bytes read from the live title under victory-gta6 (2026-11-24): VIctory rewrote the 9-byte
// `mov rcx, gs:[58h]` into `call rel32; nop dword [rax+0]`, so the file-exact signature had 0 hits.
TEST(script_call_resolves_on_victory_rewritten_image) {
    alignas(16) std::uint8_t img[128] = {};
    const std::uint8_t live[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xEC,
                                 0x30, 0x48, 0x8B, 0x41, 0x08, 0x48, 0x89, 0xCE, 0xE8, 0xF6, 0xE0, 0x20, 0xFC,
                                 0x0F, 0x1F, 0x40, 0x00, 0x4C, 0x89, 0xCB, 0x44, 0x89, 0xC5, 0x49, 0x89, 0xD6};
    std::memcpy(img + 32, live, sizeof(live));
    auto base = reinterpret_cast<std::uintptr_t>(img);
    scan::Scanner s({base, sizeof(img)}, {{base, sizeof(img)}});
    auto profiles = CreateGta6Profiles();
    bool found = false;
    for (const auto& t : profiles.at(0)->ScanTargets()) {
        if (t.name != "script_call") continue;
        found = true;
        auto hit = s.Resolve(t);
        CHECK(hit && hit->value == base + 32);
    }
    CHECK(found);
}

// ------------------------------------------------------------------ hosted start under VIctory (T7.1)
TEST(host_info_from_title_splits_path) {
    HMODULE self = GetModuleHandleW(nullptr);
    HostInfo h = HostInfoFromTitle(self, L"D:\\g\\GTA6\\game_scarlett_master.exe");
    CHECK(h.exe == self);
    CHECK(h.exePath == L"D:\\g\\GTA6\\game_scarlett_master.exe");
    CHECK(h.exeName == L"game_scarlett_master.exe");
    CHECK(h.exeDir == L"D:\\g\\GTA6");
    HostInfo fwd = HostInfoFromTitle(self, L"D:/g/x.exe");
    CHECK(fwd.exeName == L"x.exe" && fwd.exeDir == L"D:/g");
}

TEST(host_info_matches_xbox_profile) {
    HostInfo h = HostInfoFromTitle(GetModuleHandleW(nullptr), L"D:\\g\\GTA6\\game_scarlett_master.exe");
    auto profiles = CreateGta6Profiles();
    bool matched = false;
    for (auto& p : profiles)
        if (p->Matches(h)) matched = true;
    CHECK(matched);
}

TEST(hosted_from_loaded_accepts_dll_image) {
    // VIctory maps the title with IMAGE_FILE_DLL set; FromLoaded must not care.
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const std::uint8_t*>(k32) +
                                                           reinterpret_cast<const IMAGE_DOS_HEADER*>(k32)->e_lfanew);
    CHECK((nt->FileHeader.Characteristics & IMAGE_FILE_DLL) != 0);
    auto m = pe::Module::FromLoaded(k32);
    CHECK(m && !m->ExecutableRegions().empty());
}

TEST(victory_title_info_rejects_bad) {
    static_assert(sizeof(VictoryTitleInfo) == 32);
    const wchar_t* path = L"D:\\g\\GTA6\\game_scarlett_master.exe";
    VictoryTitleInfo ok{sizeof(VictoryTitleInfo), 1, GetModuleHandleW(nullptr), 0x1000, path};
    CHECK(ValidateTitleInfo(&ok) == SHVI_OK);
    CHECK(ValidateTitleInfo(nullptr) != SHVI_OK);
    VictoryTitleInfo small = ok;
    small.struct_size = 24;
    CHECK(ValidateTitleInfo(&small) != SHVI_OK);
    VictoryTitleInfo v2 = ok;
    v2.version = 2;
    CHECK(ValidateTitleInfo(&v2) != SHVI_OK);
    VictoryTitleInfo nobase = ok;
    nobase.image_base = nullptr;
    CHECK(ValidateTitleInfo(&nobase) != SHVI_OK);
    VictoryTitleInfo nopath = ok;
    nopath.exe_path = nullptr;
    CHECK(ValidateTitleInfo(&nopath) != SHVI_OK);
    VictoryTitleInfo bigger = ok;
    bigger.struct_size = 48;  // a newer VIctory with more fields is fine
    CHECK(ValidateTitleInfo(&bigger) == SHVI_OK);
}

int main() {
    for (auto& t : Tests()) {
        int before = g_failed;
        t.fn();
        printf("%s %s\n", g_failed == before ? "PASS" : "FAIL", t.name);
    }
    printf("\n%zu tests, %d checks, %d failed\n", Tests().size(), g_checks, g_failed);
    return g_failed ? 1 : 0;
}
