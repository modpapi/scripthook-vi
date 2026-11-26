// Trainer logic against a fake host: which natives each feature calls, with which cells, and that
// nothing is called when the native table does not bind what a feature needs.
#include "../samples/trainer/trainer.hpp"

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

int g_fail = 0, g_checks = 0;
#define CHECK(x) do { ++g_checks; if (!(x)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x); } } while (0)

// ------------------------------------------------------------------ fake host
struct Call {
    std::uint64_t hash;
    std::vector<std::uint64_t> args;
};
std::vector<Call> g_calls;
std::uint64_t g_hash;
std::vector<std::uint64_t> g_args;
alignas(16) std::uint64_t g_ret[4];
std::set<std::uint64_t> g_bound;
std::map<std::uint64_t, std::vector<std::uint64_t>> g_results;
std::vector<std::string> g_log;
std::vector<shvi_entity_ref> g_vehicles, g_peds;

std::uint64_t H(const char* n) { return shvi::sdk::EngineHash(n); }
void Init(std::uint64_t h) { g_hash = h; g_args.clear(); }
void Push(std::uint64_t v) { g_args.push_back(v); }
std::uint64_t* CallFn() {
    g_calls.push_back({g_hash, g_args});
    std::memset(g_ret, 0, sizeof(g_ret));
    if (auto it = g_results.find(g_hash); it != g_results.end())
        for (std::size_t i = 0; i < it->second.size() && i < 4; ++i) g_ret[i] = it->second[i];
    return g_ret;
}
int Exists(std::uint64_t h) { return g_bound.count(h) ? 1 : 0; }
void LogFn(const char* m) { g_log.emplace_back(m); }
int WorldGetAll(std::uint32_t kind, shvi_entity_ref* out, int max) {
    const auto& v = kind == SHVI_ENTITY_VEHICLE ? g_vehicles : g_peds;
    if (!out) return static_cast<int>(v.size());
    for (int i = 0; i < max && i < static_cast<int>(v.size()); ++i) out[i] = v[static_cast<std::size_t>(i)];
    return static_cast<int>(v.size());
}

shvi_api MakeApi() {
    shvi_api a{};
    a.size = sizeof(a);
    a.abi_major = SHVI_ABI_MAJOR;
    a.abi_minor = SHVI_ABI_MINOR;
    a.log = LogFn;
    a.native_init = Init;
    a.native_push64 = Push;
    a.native_call = CallFn;
    a.world_get_all = WorldGetAll;
    a.native_exists = Exists;
    a.script_wait = [](std::uint32_t) {};
    return a;
}

const shvi_entity_ref kPed{0x1111, 0x2222}, kVeh{0x3333, 0x4444};
std::uint64_t F(float f) { return shvi_f2u(f); }
std::uint64_t PackXY(float x, float y) { return F(x) | (F(y) << 32); }

const Call* Last(const char* name) {
    for (auto it = g_calls.rbegin(); it != g_calls.rend(); ++it)
        if (it->hash == H(name)) return &*it;
    return nullptr;
}
int Count(const char* name) {
    int n = 0;
    for (auto& c : g_calls) n += c.hash == H(name) ? 1 : 0;
    return n;
}

void Reset(bool bindAll) {
    g_calls.clear();
    g_results.clear();
    g_bound.clear();
    g_log.clear();
    g_vehicles.clear();
    g_peds.clear();
    g_results[H("PLAYER_PED_ID")] = {kPed.lo, kPed.hi};
    if (bindAll)
        for (const auto& m : trainer::kNativeMeta) g_bound.insert(H(m.name));
}

struct Fixture {
    shvi_api api = MakeApi();
    trainer::Trainer t;
    std::uint32_t now = 1000;
    explicit Fixture(bool bindAll = true, bool experimental = true) {
        Reset(bindAll);
        shvi::api() = &api;
        t.SetClock([this] { return now; });
        t.settings.allowExperimental = experimental;
        t.Refresh();
        t.Tick();
    }
    void Frame(std::uint32_t ms = 16) { now += ms; t.Tick(); }
};

// ------------------------------------------------------------------ tests
void TestMetaTable() {
    // sorted by name, so the binary search is valid
    for (std::size_t i = 1; i < sizeof(trainer::kNativeMeta) / sizeof(trainer::kNativeMeta[0]); ++i)
        CHECK(std::strcmp(trainer::kNativeMeta[i - 1].name, trainer::kNativeMeta[i].name) < 0);
    CHECK(trainer::ConfidenceOf("SET_ENTITY_HEALTH") == trainer::Conf::Verified);
    CHECK(trainer::ConfidenceOf("SET_VEHICLE_FIXED") == trainer::Conf::Corroborated);
    CHECK(trainer::ConfidenceOf("_REFILL_AMMO_IN_CURRENT_PED_WEAPON") == trainer::Conf::Guide);
    CHECK(trainer::ConfidenceOf("NO_SUCH_NATIVE") == trainer::Conf::Unknown);
}

void TestEveryNeedIsANamedNative() {
    Fixture fx;
    std::set<std::string> ids;
    for (auto& f : fx.t.Features()) {
        CHECK(ids.insert(f.id).second);  // ids are unique
        for (const char* n : f.needs) {
            bool known = trainer::ConfidenceOf(n) != trainer::Conf::Unknown;
            if (!known) std::printf("  feature %s needs unknown native %s\n", f.id.c_str(), n);
            CHECK(known);
        }
        CHECK(f.available || f.blocked);
    }
    CHECK(fx.t.Features().size() >= 90);
}

void TestNothingBoundMeansNothingCalled() {
    Fixture fx(false);
    for (auto& f : fx.t.Features()) CHECK(!f.available);
    std::size_t before = g_calls.size();
    for (auto& f : fx.t.Features()) {
        if (f.kind == trainer::Kind::Toggle) f.on = true;
        fx.t.Activate(f);
        CHECK(f.kind != trainer::Kind::Toggle || !f.on);  // a refused toggle falls back to off
    }
    fx.Frame();
    CHECK(g_calls.size() == before);  // not even PLAYER_PED_ID
    CHECK(fx.t.Label(*fx.t.Find("player.god")).rfind("[n/a] ", 0) == 0);
}

void TestMissingNativeDisablesOnlyItsFeatures() {
    Fixture fx;
    g_bound.erase(H("SET_VEHICLE_FIXED"));
    fx.t.Refresh();
    CHECK(!fx.t.Find("veh.repair")->available);
    CHECK(fx.t.Find("veh.repair")->missing == "SET_VEHICLE_FIXED");
    CHECK(!fx.t.Find("veh.autorepair")->available);
    CHECK(fx.t.Find("veh.clean")->available && fx.t.Find("player.god")->available);
}

void TestExperimentalGate() {
    Fixture fx(true, false);
    trainer::Feature* refill = fx.t.Find("weapon.refill");  // a guide-only name
    CHECK(refill->available && refill->experimental && !fx.t.Usable(*refill));
    CHECK(fx.t.Label(*refill).rfind("[locked] ", 0) == 0);
    CHECK(!fx.t.Activate(*refill) && Count("_REFILL_AMMO_IN_CURRENT_PED_WEAPON") == 0);
    trainer::Feature* god = fx.t.Find("player.god");  // behaviour-checked
    CHECK(!god->experimental && fx.t.Usable(*god));
    trainer::Feature* proofs = fx.t.Find("player.proofs");  // corroborated name, guessed argument
    CHECK(proofs->experimental);
    fx.t.settings.allowExperimental = true;
    CHECK(fx.t.Activate(*refill) && Count("_REFILL_AMMO_IN_CURRENT_PED_WEAPON") == 1);
    CHECK(fx.t.Label(*refill).rfind("[exp] ", 0) == 0);
}

void TestGodModeCellsAndReapply() {
    Fixture fx;
    CHECK(fx.t.Set("player.god", true));
    const Call* c = Last("SET_ENTITY_INVINCIBLE");
    CHECK(c && c->args == (std::vector<std::uint64_t>{kPed.lo, kPed.hi, 1}));
    int n = Count("SET_ENTITY_INVINCIBLE");
    fx.Frame(100);
    CHECK(Count("SET_ENTITY_INVINCIBLE") == n);  // inside the re-apply interval
    fx.Frame(800);
    CHECK(Count("SET_ENTITY_INVINCIBLE") == n + 1);
    CHECK(fx.t.Set("player.god", false));
    CHECK(Last("SET_ENTITY_INVINCIBLE")->args[2] == 0);
    n = Count("SET_ENTITY_INVINCIBLE");
    fx.Frame(2000);
    CHECK(Count("SET_ENTITY_INVINCIBLE") == n);  // off: not held
}

void TestHeal() {
    Fixture fx;
    g_results[H("GET_ENTITY_MAX_HEALTH")] = {350};
    CHECK(fx.t.Activate("player.heal"));
    const Call* c = Last("SET_ENTITY_HEALTH");
    // entity (2 cells), health, instigator (2 cells), two flags
    CHECK(c && c->args == (std::vector<std::uint64_t>{kPed.lo, kPed.hi, 350, 0, 0, 0, 0}));
}

void TestNeverWanted() {
    Fixture fx;
    g_results[H("GET_PLAYER_WANTED_LEVEL")] = {3};
    fx.t.Set("player.neverwanted", true);
    CHECK(Last("SET_MAX_WANTED_LEVEL")->args[0] == 0);
    CHECK(Count("CLEAR_PLAYER_WANTED_LEVEL") == 1);
    g_results[H("GET_PLAYER_WANTED_LEVEL")] = {0};
    fx.Frame(1000);
    CHECK(Count("CLEAR_PLAYER_WANTED_LEVEL") == 1);  // nothing to clear
    fx.t.Set("player.neverwanted", false);
    CHECK(Last("SET_MAX_WANTED_LEVEL")->args[0] == 5);
}

void TestVehicleFeaturesNeedAVehicle() {
    Fixture fx;
    fx.t.Activate("veh.repair");
    CHECK(Count("SET_VEHICLE_FIXED") == 0);  // on foot
    g_results[H("GET_VEHICLE_PED_IS_USING")] = {kVeh.lo, kVeh.hi};
    fx.Frame();
    CHECK(fx.t.Context().inVehicle);
    fx.t.Activate("veh.repair");
    CHECK(Last("SET_VEHICLE_FIXED")->args == (std::vector<std::uint64_t>{kVeh.lo, kVeh.hi}));
    CHECK(Last("SET_VEHICLE_ENGINE_HEALTH")->args == (std::vector<std::uint64_t>{kVeh.lo, kVeh.hi, F(1.0f)}));
    fx.t.Find("veh.boostspeed")->f = 60.0f;
    fx.t.Activate("veh.boost");
    CHECK(Last("SET_VEHICLE_FORWARD_SPEED")->args == (std::vector<std::uint64_t>{kVeh.lo, kVeh.hi, F(60.0f), 0}));
    fx.t.Activate("veh.opendoors");
    CHECK(Count("SET_VEHICLE_DOOR_OPEN") == 6);
}

void TestTeleportMovesTheVehicleWhenDriving() {
    Fixture fx;
    g_results[H("GET_ENTITY_COORDS")] = {PackXY(10.0f, 20.0f), F(30.0f)};
    g_results[H("GET_ENTITY_FORWARD_VECTOR")] = {PackXY(0.0f, 1.0f), F(0.0f)};
    fx.t.Find("tp.step")->f = 5.0f;
    fx.t.Activate("tp.forward");
    const Call* c = Last("SET_ENTITY_COORDS");
    // entity, Vector3 (x,y packed + z), four flags, float
    CHECK(c && c->args.size() == 9);
    CHECK(c->args[0] == kPed.lo && c->args[1] == kPed.hi);
    CHECK(c->args[2] == PackXY(10.0f, 25.0f) && c->args[3] == F(30.0f));
    CHECK(c->args[8] == F(-1.0f));
    g_results[H("GET_VEHICLE_PED_IS_USING")] = {kVeh.lo, kVeh.hi};
    fx.Frame();
    fx.t.Activate("tp.up");
    c = Last("SET_ENTITY_COORDS");
    CHECK(c->args[0] == kVeh.lo && c->args[3] == F(35.0f));
    // slots
    fx.t.Find("tp.slot")->i = 3;
    fx.t.Activate("tp.load");
    CHECK(!g_log.empty() && g_log.back() == "trainer: slot is empty");
    fx.t.Activate("tp.save");
    g_results[H("GET_ENTITY_COORDS")] = {PackXY(0.0f, 0.0f), F(0.0f)};
    fx.t.Activate("tp.load");
    c = Last("SET_ENTITY_COORDS");
    CHECK(c->args[2] == PackXY(10.0f, 20.0f) && c->args[3] == F(30.0f));
}

void TestWaypoint() {
    Fixture fx;
    g_results[H("IS_WAYPOINT_ACTIVE")] = {0};
    fx.t.Activate("tp.waypoint");
    CHECK(Count("SET_ENTITY_COORDS") == 0);
    g_results[H("IS_WAYPOINT_ACTIVE")] = {1};
    g_results[H("_GET_WAYPOINT_COORDS")] = {PackXY(100.0f, -200.0f), F(0.0f)};
    g_results[H("GET_ENTITY_COORDS")] = {PackXY(1.0f, 2.0f), F(44.0f)};
    fx.t.Activate("tp.waypoint");
    const Call* c = Last("SET_ENTITY_COORDS");
    CHECK(c && c->args[2] == PackXY(100.0f, -200.0f) && c->args[3] == F(44.0f));  // keeps the height when the blip has none
}

void TestTimeAndWeather() {
    Fixture fx;
    g_results[H("GET_CLOCK_MINUTES")] = {42};
    fx.t.Find("time.hour")->i = 7;
    fx.t.Activate("time.hour");
    CHECK(Last("SET_CLOCK_TIME")->args == (std::vector<std::uint64_t>{7, 42, 0, 0}));
    fx.t.Find("time.preset")->i = 4;
    fx.t.Activate("time.preset");
    CHECK(Last("SET_CLOCK_TIME")->args == (std::vector<std::uint64_t>{19, 30, 0, 0}));
    fx.t.Find("weather.type")->i = 0;
    fx.t.Activate("weather.type");
    const Call* c = Last("SET_WEATHER_TYPE");
    CHECK(c && c->args.size() == 7 && c->args[0] == trainer::kWeatherTypes[0].hash);
    // per-frame hold
    fx.t.Find("time.scale")->f = 0.25f;
    fx.t.Set("time.holdscale", true);
    int n = Count("SET_TIME_SCALE");
    fx.Frame(); fx.Frame();
    CHECK(Count("SET_TIME_SCALE") == n + 2);
    CHECK(Last("SET_TIME_SCALE")->args[0] == F(0.25f));
}

void TestWorldUsesNearbyEntitiesOnly() {
    Fixture fx;
    const shvi_entity_ref near1{1, 1}, far1{2, 2};
    g_vehicles = {near1, far1, kVeh};
    g_results[H("GET_VEHICLE_PED_IS_USING")] = {kVeh.lo, kVeh.hi};
    fx.Frame();
    // GET_ENTITY_COORDS is the same for everyone in the fake, so distance is 0: the radius keeps all,
    // and the player's own vehicle must still be skipped.
    fx.t.Activate("world.explodevehicles");
    CHECK(Count("EXPLODE_VEHICLE") == 2);
    for (auto& c : g_calls)
        if (c.hash == H("EXPLODE_VEHICLE")) CHECK(!(c.args[0] == kVeh.lo && c.args[1] == kVeh.hi));
    fx.t.Activate("world.repairvehicles");  // repairing includes the player's own
    CHECK(Count("SET_VEHICLE_FIXED") == 3);
}

void TestMenuBuilds() {
    Fixture fx;
    shvi::ui::Menu menu("VI TRAINER");
    fx.t.BuildMenu(menu);
    menu.SetOpen(true);
    shvi::ui::Style st;
    auto list = menu.Layout(st);
    CHECK(!list.empty());
    bool sawPlayer = false;
    for (auto& d : list) sawPlayer = sawPlayer || d.text.rfind("PLAYER  (", 0) == 0;
    CHECK(sawPlayer);
    // open the first page and toggle its first item through the menu
    menu.Handle(shvi::ui::Action::Select);
    menu.Handle(shvi::ui::Action::Select);
    CHECK(fx.t.Find("player.god")->on && Count("SET_ENTITY_INVINCIBLE") == 1);
}

void TestSetWantedAndVitals() {
    Fixture fx;
    fx.t.Find("player.setwanted")->i = 3;
    fx.t.Activate("player.setwanted");
    CHECK(Count("CLEAR_PLAYER_WANTED_LEVEL") == 1);  // the raise native never lowers
    CHECK(Last("VI_RAISE_PLAYER_WANTED_LEVEL")->args == (std::vector<std::uint64_t>{kPed.lo, kPed.hi, 3}));
    fx.t.Find("player.setwanted")->i = 0;
    fx.t.Activate("player.setwanted");
    CHECK(Count("VI_RAISE_PLAYER_WANTED_LEVEL") == 1 && Count("CLEAR_PLAYER_WANTED_LEVEL") == 2);
    CHECK(fx.t.Find("player.setwanted")->experimental);  // the raise native had no effect in the 2026-11-25 runs
    CHECK(!fx.t.Find("player.vitals")->experimental);
    g_results[H("VI_GET_PED_VITAL_MAX")] = {F(200.0f)};
    fx.t.Activate("player.vitals");
    CHECK(Count("VI_SET_PED_VITAL") == 6);  // indices 0..5, as the game's own restore script does
    CHECK(Last("VI_SET_PED_VITAL")->args == (std::vector<std::uint64_t>{kPed.lo, kPed.hi, F(200.0f), 5}));
    g_results[H("VI_GET_PED_VITAL_MAX")] = {F(-1.0f)};  // a ped without that pool
    fx.t.Activate("player.vitals");
    CHECK(Count("VI_SET_PED_VITAL") == 6);
}

void TestAsyncFeaturesAreBlocked() {
    // Runs 8 and 9 (2026-11-25): the async natives need a script program's Task<T>; both built-in tokens
    // took the game down. The features stay registered, are never run, and say why.
    Fixture fx;
    for (const char* id : {"weapon.give", "weapon.giveall", "spawn.vehicle", "spawn.vehiclecopy"}) {
        trainer::Feature* f = fx.t.Find(id);
        CHECK(f && f->blocked && !f->available && !fx.t.Usable(*f));
        CHECK(fx.t.Label(*f).rfind("[blocked] ", 0) == 0);
        CHECK(!fx.t.Activate(*f));
    }
    CHECK(Count("VI_GIVE_ITEM_TO_PED_ASYNC") == 0 && Count("VI_CREATE_VEHICLE_ASYNC") == 0);
    CHECK(!g_log.empty() && g_log.back().rfind("blocked: ", 0) == 0);
    // the model list itself still works (the player-model feature uses it)
    fx.t.SetModelList({"# comment", "0x1234ABCD", "adder"});
    CHECK(fx.t.Find("spawn.model")->options.size() == 2);
    CHECK(trainer::ParseHashEntry("adder") == 0xB779A091u);  // joaat("adder"), the public GTA V value
}

void TestSpawnThroughScriptWrappers() {
    Fixture fx;
    struct Rec { std::uint32_t prog, off; std::vector<std::uint64_t> args; std::uint32_t retc; };
    static std::vector<Rec> calls;
    calls.clear();
    static bool can = false;
    fx.t.SetScriptCall([] { return can; }, [](std::uint32_t p, std::uint32_t o, const std::uint64_t* a, std::uint32_t n, std::uint64_t* r, std::uint32_t rc) {
        calls.push_back({p, o, std::vector<std::uint64_t>(a, a + n), rc});
        if (r && rc) r[0] = 0x1234;
        return SHVI_OK;
    });
    can = false;
    fx.t.Refresh();
    CHECK(!fx.t.Find("spawn.clone")->available);  // a host without script_function_call
    CHECK(fx.t.Find("spawn.clone")->missing.find("script_function_call") != std::string::npos);
    can = true;
    fx.t.Refresh();
    CHECK(fx.t.Find("spawn.clone")->blocked && !fx.t.Find("spawn.clone")->available);  // blocked after runs 10-15
    CHECK(!fx.t.Activate("spawn.clone") && calls.empty());
    fx.t.settings.runBlocked = true;  // the retry switch: the call the trainer would make
    fx.t.Refresh();
    CHECK(fx.t.Find("spawn.clone")->available && fx.t.Find("spawn.clone")->experimental);
    g_results[H("GET_ENTITY_MODEL")] = {0xCAFEu};
    g_results[H("GET_ENTITY_COORDS")] = {PackXY(10.0f, 20.0f), F(30.0f)};
    g_results[H("GET_ENTITY_FORWARD_VECTOR")] = {PackXY(1.0f, 0.0f), F(0.0f)};
    g_results[H("GET_ENTITY_HEADING")] = {F(1.5f)};
    fx.t.Find("spawn.distance")->f = 6.0f;
    CHECK(fx.t.Activate("spawn.clone"));
    CHECK(calls.size() == 1 && calls[0].prog == 0x48391B87u && calls[0].off == 0x18247u && calls[0].retc == 1);
    CHECK(calls[0].args.size() == 13 && calls[0].args[0] == 0xCAFEu);
    CHECK(calls[0].args[1] == PackXY(16.0f, 20.0f) && calls[0].args[2] == F(30.0f) && calls[0].args[3] == F(1.5f));
    for (std::size_t i = 4; i < 13; ++i) CHECK(calls[0].args[i] == ((i == 6 || i == 10) ? 1u : 0u));  // the flags a shipped script sets
    fx.t.SetModelList({"0x1234ABCD"});
    CHECK(fx.t.Activate("spawn.object"));
    CHECK(calls.size() == 2 && calls[1].off == 0x1BE38u && calls[1].args.size() == 10 && calls[1].args[0] == 0x1234ABCDu);
    CHECK(Count("VI_CREATE_PED_ASYNC") == 0 && Count("VI_CREATE_OBJECT_ASYNC") == 0);  // never the natives directly
}

void TestStatusLine() {
    Fixture fx;
    g_results[H("GET_ENTITY_COORDS")] = {PackXY(1.5f, 2.5f), F(3.5f)};
    g_results[H("GET_ENTITY_SPEED")] = {F(10.0f)};
    fx.Frame();
    CHECK(fx.t.Status().find("X 1.5") != std::string::npos && fx.t.Status().find("36 km/h") != std::string::npos);
}

}  // namespace

int main() {
    TestMetaTable();
    TestEveryNeedIsANamedNative();
    TestNothingBoundMeansNothingCalled();
    TestMissingNativeDisablesOnlyItsFeatures();
    TestExperimentalGate();
    TestGodModeCellsAndReapply();
    TestHeal();
    TestNeverWanted();
    TestVehicleFeaturesNeedAVehicle();
    TestTeleportMovesTheVehicleWhenDriving();
    TestWaypoint();
    TestTimeAndWeather();
    TestWorldUsesNearbyEntitiesOnly();
    TestMenuBuilds();
    TestStatusLine();
    TestSetWantedAndVitals();
    TestAsyncFeaturesAreBlocked();
    TestSpawnThroughScriptWrappers();
    std::printf("trainer tests: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
