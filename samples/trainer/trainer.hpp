// ScriptHook VI trainer: the mod-menu logic (no rendering, no D3D hook).
//
//   Trainer        a registry of features in categories (player, weapons, vehicle, teleport, time,
//                  weather, world, misc). Each feature names the natives it needs.
//   Refresh()      asks the host which of those natives are bound in the running build
//                  (ABI 1.5 native_exists) and enables only the features it can serve.
//   Tick()         once per frame from a script fiber: refreshes the player/vehicle context and
//                  re-applies the toggles that have to be held (every frame or every interval).
//   BuildMenu()    fills a shvi::ui::Menu; drawing is whatever ui::Backend the plugin picks.
//
// Natives are called through the generated NativeDB wrappers (shvi/natives_full.hpp). Every name has
// a confidence (shvi/natives_meta.inc). A feature is "experimental" when its weakest native is not at
// least corroborated by the code layout, or when the meaning of an argument is a guess; experimental
// features stay off until the user allows them (Settings).
//
// Story mode / offline only: the host's online guard still applies to every call made here.
#pragma once
#include <shvi/natives_full.hpp>
#include <shvi/shvi.hpp>
#include <shvi/ui.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace trainer {

namespace A = shvi::nativedb::all;
using shvi::nativedb::Vec3;
using Ref = shvi_entity_ref;

// ------------------------------------------------------------------ data
struct HashName {
    std::uint32_t hash;
    const char* name;
    int uses;
};
#include "trainer_data.inc"

// Reads and writes of script objects the trainer does not own (the experimental task features). On MSVC
// they run under SEH; elsewhere there is no portable fault guard yet, so they are refused (return false)
// and those features report "unreadable" instead of risking a crash.
namespace guarded {
#if defined(_MSC_VER)
inline bool Copy(void* dst, std::uintptr_t src, std::size_t n) {
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (1) {
        return false;
    }
}
inline bool Bump16(std::uintptr_t at) {
    __try {
        ++*reinterpret_cast<std::uint16_t*>(at);
        return true;
    } __except (1) {
        return false;
    }
}
#else
inline bool Copy(void*, std::uintptr_t, std::size_t) { return false; }
inline bool Bump16(std::uintptr_t) { return false; }
#endif
}  // namespace guarded

enum class Conf : std::uint8_t { Verified = 0, Corroborated = 1, Inferred = 2, Guide = 3, Conflict = 4, Denied = 5, Unknown = 6 };

struct NativeMeta {
    const char* name;
    int conf;
};
inline constexpr NativeMeta kNativeMeta[] = {
#include <shvi/natives_meta.inc>
};

inline Conf ConfidenceOf(const char* name) {
    std::size_t lo = 0, hi = sizeof(kNativeMeta) / sizeof(kNativeMeta[0]);
    while (lo < hi) {
        std::size_t mid = (lo + hi) / 2;
        int c = std::strcmp(kNativeMeta[mid].name, name);
        if (c == 0) return static_cast<Conf>(kNativeMeta[mid].conf);
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return Conf::Unknown;
}
inline const char* ConfName(Conf c) {
    switch (c) {
        case Conf::Verified: return "verified";
        case Conf::Corroborated: return "corroborated";
        case Conf::Inferred: return "inferred";
        case Conf::Guide: return "guide";
        case Conf::Conflict: return "conflict";
        case Conf::Denied: return "denied";
        default: return "unknown";
    }
}

// ------------------------------------------------------------------ helpers
inline bool Valid(const Ref& r) { return r.lo != 0 || r.hi != 0; }
inline bool Same(const Ref& a, const Ref& b) { return a.lo == b.lo && a.hi == b.hi; }
// Headings are radians in GTA VI: seen in game (3.8 for a forward vector at 219 degrees) and in the code of both natives.
inline constexpr float kRadToDeg = 57.29578f, kDegToRad = 0.017453292f;
inline Vec3 V(float x, float y, float z) { return Vec3{x, y, z, 0.0f}; }
inline Vec3 Add(const Vec3& a, const Vec3& b) { return V(a.x + b.x, a.y + b.y, a.z + b.z); }
inline Vec3 Mul(const Vec3& a, float s) { return V(a.x * s, a.y * s, a.z * s); }
inline float Dist2(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

// joaat of a lower-cased name: how the game hashes model, weapon and weather names.
inline std::uint32_t HashName32(std::string_view s) {
    std::uint32_t h = 0;
    for (char ch : s) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        h += static_cast<std::uint8_t>(ch); h += h << 10; h ^= h >> 6;
    }
    h += h << 3; h ^= h >> 11; h += h << 15;
    return h;
}
// "name" or "0xHEX" -> hash (a user list entry)
inline std::uint64_t ParseHashEntry(const std::string& e) {
    if (e.size() > 2 && e[0] == '0' && (e[1] == 'x' || e[1] == 'X')) return std::strtoull(e.c_str() + 2, nullptr, 16);
    // "name  0xKEY": a display name followed by the 64-bit key (names alone do not resolve in GTA VI)
    std::size_t k = e.rfind(" 0x");
    if (k != std::string::npos) return std::strtoull(e.c_str() + k + 3, nullptr, 16);
    return HashName32(e);
}
inline std::uint64_t PackXY(const Vec3& v) { return shvi_f2u(v.x) | (shvi_f2u(v.y) << 32); }

// GTA VI model keys are 64 bits and neither half is the name's joaat (model info +0x68 holds that) (seen in game
// 2026-11-26: the player's model is 0x6B13B6B43F959856; IS_MODEL_A_PED is 1 for it and 0 for 0x3F959856). The
// generated wrappers type models as a 32-bit Hash and drop the high half, so model natives go through these.
using ModelKey = std::uint64_t;
inline ModelKey ModelOf(const Ref& r) {
    return shvi::invoke<std::uint64_t>(shvi::sdk::EngineHash("GET_ENTITY_MODEL"), r.lo, r.hi);
}
inline bool ModelQuery(std::string_view native, ModelKey m) {
    return (shvi::invoke<std::uint64_t>(shvi::sdk::EngineHash(native), m) & 0xFF) != 0;
}

// Async natives build a script Task before they do anything and take its type as their last argument:
// a token for Task<T> in the calling script program's type space. A plugin is not a script program.
// Both built-in types were tried in game on 2026-11-25 and neither works:
//   0x8000002F  System.Threading.Tasks.Task`1 (run 8): the native returned a task, and seconds later the
//               garbage collector crashed on it. An object of the open generic type has no runtime type.
//   0x8000002E  System.Threading.Tasks.Task (run 9): the native faulted inside task setup; the game exited.
// So these calls need a real instantiated Task<T> from a loaded program. The way to get one is to call the
// game's own script wrappers (module "native": func_018247 creates a ped, func_01BE38 an object) through
// the VM instead of the native. Until the hook can call script functions, the features are blocked.
inline constexpr std::int32_t kTaskTypeToken = static_cast<std::int32_t>(0x8000002Eu);
// Argument forms: where a native takes flags whose meaning is not known, the trainer passes the form shipped
// scripts use most (tally of literal call sites, 2026-11-25). SET_WEATHER_TYPE only worked in game in that form.
inline constexpr const char* kAsyncBlocked = "needs a script-side Task<T>: crashed the game in runs 8 and 9";
// Creation through the script wrappers (runs 10-15): with every flag zero the wrapper returns a task and
// nothing is created; with the flags a shipped script sets, the engine allocates a pooled object and
// faults in the allocator (0x1406728F0), because the per-thread table it registers objects in (script
// heap +0x17D8) is not valid on the worker thread at tick time. Entering the engine's script scope first
// is the likely fix; that path crashed on its first try and is now only fail-safe, not working.
inline constexpr const char* kSpawnBlocked = "the ped is not created (flags zero) or the engine faults (script flags): runs 10-15";
// What unblocked it (2026-11-26): the native registers its task under a weak GC handle, so the task was reclaimed
// and the request died with it; ABI 1.7 script_object_pin holds a strong one. And the model must be the full
// 64-bit key: with the 32-bit hash the creation fails its model check and completes at once with no entity.
inline constexpr const char* kSpawnChecked = "2026-11-26: ped and object creation through the script wrappers completes with the new entity once the task is pinned (ABI 1.7) and the model is the full 64-bit key";

enum class Category { Player, Weapons, Vehicle, Spawn, Teleport, Time, Weather, World, Misc, Count };
inline const char* CategoryName(Category c) {
    static const char* n[] = {"PLAYER", "WEAPONS", "VEHICLE", "SPAWN", "TELEPORT", "TIME", "WEATHER", "WORLD", "MISC"};
    return n[static_cast<int>(c)];
}

// What a feature sees each frame.
struct Ctx {
    Ref ped{};             // player ped
    Ref veh{};             // vehicle the player uses, or zero
    bool inVehicle = false;
    std::uint32_t nowMs = 0;
    float dt = 0.0f;
    Ref Target() const { return inVehicle ? veh : ped; }  // what "teleport me" moves
};

enum class Kind { Action, Toggle, Int, Float, Choice };
enum class Hold { None, Interval, Frame };  // how a switched-on toggle is kept applied

struct Feature {
    std::string id, label;
    Category cat = Category::Misc;
    Kind kind = Kind::Action;
    Hold hold = Hold::None;
    std::vector<const char*> needs;  // every native the feature calls
    bool guess = false;              // an argument's meaning is assumed (GTA V analogy), not checked
    const char* blocked = nullptr;   // set when the feature is known not to work: never run, the label says why
    bool scriptCall = false;         // goes through a script function (ABI 1.6 script_function_call)
    const char* checked = nullptr;   // confirmed by read-back in the running game (which run, what it showed):
                                     // lifts "experimental" whatever the names' confidence
    // state
    bool on = false;
    int i = 0, imin = 0, imax = 0, istep = 1;
    float f = 0, fmin = 0, fmax = 0, fstep = 0.1f;
    std::vector<std::string> options;
    // behaviour
    std::function<void(Ctx&, Feature&)> run;   // Action: do it. Toggle: apply current `on`. Value: apply the value.
    std::function<void(Ctx&, Feature&)> tick;  // optional: every frame while on (Hold::Frame)
    // computed by Refresh()
    bool available = false, experimental = false;
    Conf weakest = Conf::Unknown;
    std::string missing;
    std::uint32_t lastApply = 0;
};

struct Settings {
    bool allowExperimental = false;  // run features whose natives are not corroborated
    int reapplyMs = 750;             // Hold::Interval period
    bool showStatus = true;
    bool pinTask = false;            // run-22 experiment, autorun step settings.pintask
    bool runBlocked = false;         // run features marked blocked anyway: for tests and for retrying them in a run, not in the menu
};

class Trainer {
public:
    using Exists = std::function<bool(const char*)>;
    using Clock = std::function<std::uint32_t()>;

    Trainer() {
        exists_ = [](const char* n) { return shvi::native_exists(std::string_view(n)); };
        canScriptCall_ = [] { return shvi::api() && SHVI_API_HAS(shvi::api(), script_function_call); };
        callScript_ = [](std::uint32_t prog, std::uint32_t off, const std::uint64_t* args, std::uint32_t argc, std::uint64_t* ret, std::uint32_t retc) {
            return shvi::script_function_call(prog, off, args, argc, ret, retc);
        };
        pinScript_ = [](std::uint64_t obj, std::uint64_t* pin) { return shvi::script_object_pin(obj, pin); };
        unpinScript_ = [](std::uint64_t pin) { return shvi::script_object_unpin(pin); };
        clock_ = [] {
            using namespace std::chrono;
            return static_cast<std::uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
        };
        Register();
    }
    // Test seams: replace how natives are probed and how time is read.
    void SetExists(Exists e) { exists_ = std::move(e); }
    void SetClock(Clock c) { clock_ = std::move(c); }
    using ScriptCall = std::function<int(std::uint32_t, std::uint32_t, const std::uint64_t*, std::uint32_t, std::uint64_t*, std::uint32_t)>;
    void SetScriptCall(std::function<bool()> can, ScriptCall call) { canScriptCall_ = std::move(can); callScript_ = std::move(call); }
    using PinFn = std::function<int(std::uint64_t, std::uint64_t*)>;
    using UnpinFn = std::function<int(std::uint64_t)>;
    void SetPin(PinFn pin, UnpinFn unpin) { pinScript_ = std::move(pin); unpinScript_ = std::move(unpin); }

    Settings settings;
    std::vector<Feature>& Features() { return features_; }
    Feature* Find(const std::string& id) {
        for (auto& f : features_)
            if (f.id == id) return &f;
        return nullptr;
    }
    const Ctx& Context() const { return ctx_; }
    const std::string& Status() const { return status_; }

    // Which features can run in this build. Call once the host has bound its native table, and again
    // whenever that may have changed (cheap: one lookup per needed native).
    void Refresh() {
        haveCore_ = exists_("PLAYER_PED_ID");
        for (auto& f : features_) {
            f.missing.clear();
            f.weakest = Conf::Verified;
            for (const char* n : f.needs) {
                if (!exists_(n)) {
                    if (!f.missing.empty()) f.missing += ", ";
                    f.missing += n;
                }
                Conf c = ConfidenceOf(n);
                if (static_cast<int>(c) > static_cast<int>(f.weakest)) f.weakest = c;
            }
            if (f.scriptCall && !canScriptCall_()) {
                if (!f.missing.empty()) f.missing += ", ";
                f.missing += "script_function_call (host ABI 1.6)";
            }
            f.available = f.missing.empty() && haveCore_ && f.weakest != Conf::Denied && (!f.blocked || settings.runBlocked);
            f.experimental = !f.checked && (f.guess || f.scriptCall || static_cast<int>(f.weakest) > static_cast<int>(Conf::Inferred));
        }
    }
    bool Usable(const Feature& f) const { return f.available && (!f.experimental || settings.allowExperimental); }

    // The label a menu shows: availability and confidence are visible, never silent.
    std::string Label(const Feature& f) const {
        if (f.blocked && !settings.runBlocked) return "[blocked] " + f.label;
        if (!f.available) return "[n/a] " + f.label;
        if (f.experimental) return (settings.allowExperimental ? "[exp] " : "[locked] ") + f.label;
        return f.label;
    }

    // Per frame, from a script fiber.
    void Tick() {
        std::uint32_t now = clock_();
        ctx_.dt = lastTick_ ? (now - lastTick_) / 1000.0f : 0.0f;
        ctx_.nowMs = now;
        lastTick_ = now;
        if (!haveCore_) return;
        ctx_.ped = A::PLAYER_PED_ID();
        ctx_.veh = Ref{};
        ctx_.inVehicle = false;
        if (Valid(ctx_.ped) && exists_("GET_VEHICLE_PED_IS_USING")) {
            ctx_.veh = A::GET_VEHICLE_PED_IS_USING(ctx_.ped, false);
            ctx_.inVehicle = Valid(ctx_.veh);
        }
        if (!Valid(ctx_.ped)) return;
        for (auto& f : features_) {
            if (f.kind != Kind::Toggle || !f.on || !Usable(f)) continue;
            if (f.hold == Hold::Frame) {
                if (f.tick) f.tick(ctx_, f);
                else if (f.run) f.run(ctx_, f);
            } else if (f.hold == Hold::Interval && now - f.lastApply >= static_cast<std::uint32_t>(settings.reapplyMs)) {
                f.lastApply = now;
                if (f.run) f.run(ctx_, f);
            }
        }
        if (settings.showStatus) UpdateStatus();
        CheckSpawnWatch();
        StepRetry();
        StepCollectionScan();
    }

    // Runs an action, or applies a toggle/value that the menu just changed.
    bool Activate(Feature& f) {
        if (!Usable(f)) {
            Log(f.blocked ? "blocked: " + f.label + " (" + f.blocked + ")"
                : f.available ? "locked (experimental natives are off): " + f.label
                              : "not available: " + f.label + " needs " + f.missing);
            if (f.kind == Kind::Toggle) f.on = false;
            return false;
        }
        if (!Valid(ctx_.ped)) return false;
        f.lastApply = ctx_.nowMs;
        if (f.run) f.run(ctx_, f);
        return true;
    }
    bool Activate(const std::string& id) {
        Feature* f = Find(id);
        return f && Activate(*f);
    }
    bool Set(const std::string& id, bool on) {
        Feature* f = Find(id);
        if (!f || f->kind != Kind::Toggle) return false;
        f->on = on;
        return Activate(*f);
    }

    // Menu order: each category's features under section headings (kSections), in that order; features the
    // table does not name follow at the end under "More". The pair's section is set on a group's first feature.
    std::vector<std::pair<const char*, Feature*>> Ordered(Category cat) {
        struct Group { Category cat; const char* title; std::vector<const char*> ids; };
        using C = Category;
        static const std::vector<Group> kSections = {
            {C::Player, "Health", {"player.god", "player.heal", "player.autoheal", "player.maxhealth", "player.regen", "player.vitals",
                                   "player.keepvitals", "player.vitalindex", "player.vitalvalue", "player.nodamage", "player.proofs",
                                   "player.onlyplayerdamage", "player.noragdoll", "player.suicide"}},
            {C::Player, "Wanted level", {"player.clearwanted", "player.neverwanted", "player.setwanted", "player.maxwanted",
                                         "player.wantedoff", "player.ignored", "player.policeignore"}},
            {C::Player, "Abilities", {"player.stamina", "player.breath", "player.ability", "player.abilitycharge", "player.abilityfull",
                                      "player.abilityreset", "player.abilityoff", "player.accuracy"}},
            {C::Player, "Movement", {"player.moverate", "player.fastrun", "player.autowalk", "player.jump", "player.launch", "player.freeze",
                                     "player.nogravity", "player.entitygravity", "player.nocollision"}},
            {C::Player, "Appearance", {"player.invisible", "player.alpha", "player.model", "player.sweat", "player.clean", "player.extinguish"}},
            {C::Player, "Story", {"player.bringsecond", "player.stoptasks"}},
            {C::Weapons, "Ammo", {"weapon.infclip", "weapon.refill", "weapon.autorefill", "weapon.fillclip", "weapon.ammotype",
                                  "weapon.setammo", "weapon.removeammo"}},
            {C::Weapons, "Weapons", {"weapon.collection", "weapon.bestcollection", "weapon.equip", "weapon.givecount", "weapon.give",
                                     "weapon.giveall", "weapon.drop", "weapon.hide", "weapon.keepdrop"}},
            {C::Vehicle, "Condition", {"veh.repair", "veh.autorepair", "veh.god", "veh.clean", "veh.fixtyres", "veh.bursttyres",
                                       "veh.extinguish", "veh.explode", "veh.delete"}},
            {C::Vehicle, "Driving", {"veh.engine", "veh.boostspeed", "veh.boost", "veh.stop", "veh.power", "veh.maxspeed", "veh.steerbias",
                                     "veh.handbrake", "veh.burnout", "veh.flip", "veh.launch", "veh.nogravity", "veh.seatbelt"}},
            {C::Vehicle, "Lights and doors", {"veh.lights", "veh.fullbeam", "veh.hazards", "veh.interiorlight", "veh.alarm", "veh.locks",
                                              "veh.opendoors", "veh.closedoors", "veh.breakdoors"}},
            {C::Spawn, "Story characters", {"spawn.character", "spawn.charspawn", "spawn.charlist", "spawn.charinfo", "spawn.charbring",
                                            "spawn.chardespawn"}},
            {C::Spawn, "Models", {"spawn.model", "spawn.distance", "spawn.vehicle", "spawn.vehiclecopy", "spawn.collectvehicles", "spawn.ped", "spawn.pedcopy",
                                  "spawn.clone", "spawn.object", "spawn.objectcopy"}},
            {C::Spawn, "Clean up", {"spawn.deletepeds", "spawn.deletevehicles"}},
            {C::Teleport, "Quick", {"teleport.tosecond", "tp.waypoint", "tp.clearwaypoint", "tp.step", "tp.forward", "tp.up", "tp.down",
                                    "tp.noclip", "tp.heading"}},
            {C::Teleport, "Saved positions", {"tp.slot", "tp.save", "tp.load"}},
            {C::Teleport, "Coordinates", {"tp.x", "tp.y", "tp.z", "tp.fill", "tp.go", "tp.streamed", "tp.streamedstop"}},
            {C::World, "Population", {"world.traffic", "world.holdtraffic", "world.peds", "world.holdpeds", "world.holdambient",
                                      "world.flee", "world.nocops", "world.notrains", "world.noboats"}},
            {C::World, "Nearby", {"world.radius", "world.cleararea", "world.killpeds", "world.freezepeds", "world.deletepeds",
                                  "world.deletevehicles", "world.explodevehicles", "world.repairvehicles", "world.launchvehicles",
                                  "world.stopfires"}},
            {C::World, "Physics", {"world.explosiontype", "world.explosion", "world.gravity", "world.pause"}},
            {C::Misc, "Screen and sound", {"misc.camshake", "misc.stopshake", "misc.clearfx", "misc.tcstrength", "misc.radionext"}},
            {C::Misc, "Logging", {"misc.logpos", "misc.logmodel", "misc.character", "misc.probe", "misc.counts"}},
        };
        std::vector<std::pair<const char*, Feature*>> out;
        std::vector<const Feature*> placed;
        bool anyGroup = false;
        for (const Group& g : kSections) {
            if (g.cat != cat) continue;
            const char* head = g.title;
            for (const char* id : g.ids) {
                Feature* f = Find(id);
                if (!f || f->cat != cat) continue;
                out.emplace_back(head, f);
                placed.push_back(f);
                head = nullptr;
                anyGroup = true;
            }
        }
        const char* head = anyGroup ? "More" : nullptr;
        for (auto& f : features_) {
            if (f.cat != cat || std::find(placed.begin(), placed.end(), &f) != placed.end()) continue;
            out.emplace_back(head, &f);
            head = nullptr;
        }
        return out;
    }

    // The badge a menu row shows for a feature that is not plain usable, and its colour.
    static constexpr shvi::ui::Color kTagExp{217, 119, 6, 235}, kTagLocked{120, 85, 20, 235}, kTagBlocked{200, 40, 50, 235},
        kTagNa{70, 70, 80, 235}, kTagCount{255, 255, 255, 34};
    std::string Tag(const Feature& f) const {
        if (f.blocked && !settings.runBlocked) return "BLOCKED";
        if (!f.available) return "N/A";
        if (f.experimental) return settings.allowExperimental ? "EXP" : "LOCKED";
        return {};
    }
    shvi::ui::Color TagColor(const Feature& f) const {
        if (f.blocked && !settings.runBlocked) return kTagBlocked;
        if (!f.available) return kTagNa;
        return settings.allowExperimental ? kTagExp : kTagLocked;
    }
    // The footer line for a feature: why it is unavailable, or what to expect.
    std::string Hint(const Feature& f) const {
        if (f.blocked && !settings.runBlocked) return std::string("Blocked: ") + f.blocked;
        if (!f.available) return f.missing.empty() ? "Not available in this build" : "Needs " + f.missing;
        if (f.experimental && !settings.allowExperimental) return "Experimental: allow it in Settings";
        if (f.experimental) return std::string("Experimental: ") + (f.guess ? "an argument is assumed" : "native names are unconfirmed");
        return {};
    }

    // One page per category plus settings. Call after Refresh(); call again to rebuild labels.
    void BuildMenu(shvi::ui::Menu& menu) {
        using shvi::ui::Page;
        static const char* const kTitles[] = {"Player", "Weapons", "Vehicle", "Spawn", "Teleport", "Time", "Weather", "World", "Misc"};
        static const char* const kHints[] = {"Health, wanted level, abilities, movement",
                                             "Ammo, weapon collections, equip",
                                             "The vehicle you are in: repair, handling, lights, doors",
                                             "Story characters, models, nearby entities",
                                             "Waypoint, slots, coordinates, no-clip",
                                             "Clock, presets, game speed",
                                             "Weather type, rain and wind",
                                             "Traffic, pedestrians, nearby actions, gravity",
                                             "Camera, screen effects, radio, logging"};
        Page* pages[static_cast<int>(Category::Count)] = {};
        for (int c = 0; c < static_cast<int>(Category::Count); ++c) pages[c] = &menu.AddPage(CategoryName(static_cast<Category>(c)));
        Page& set = menu.AddPage("SETTINGS");
        for (int c = 0; c < static_cast<int>(Category::Count); ++c) {
            int total = 0, usable = 0;
            for (auto& f : features_)
                if (static_cast<int>(f.cat) == c) { ++total; usable += Usable(f) ? 1 : 0; }
            char count[32];
            std::snprintf(count, sizeof(count), "%d/%d", usable, total);
            menu.Root().Submenu(kTitles[c], *pages[c]).Tag(count, kTagCount).Hint(kHints[c]);
        }
        menu.Root().Submenu("Settings", set).Hint("Experimental features, re-apply interval, status line");
        for (int c = 0; c < static_cast<int>(Category::Count); ++c)
        for (const auto& entry : Ordered(static_cast<Category>(c))) {
            Page& p = *pages[c];
            if (entry.first) p.Section(entry.first);
            Feature* fp = entry.second;
            Feature& f = *fp;
            switch (f.kind) {
                case Kind::Action: p.Button(f.label, [this, fp] { Activate(*fp); }); break;
                case Kind::Toggle: p.Toggle(f.label, &f.on, [this, fp] { Activate(*fp); }); break;
                case Kind::Int: p.Int(f.label, &f.i, f.imin, f.imax, f.istep, [this, fp] { Activate(*fp); }); break;
                case Kind::Float: p.Float(f.label, &f.f, f.fmin, f.fmax, f.fstep, 2, [this, fp] { Activate(*fp); }); break;
                case Kind::Choice: p.Choice(f.label, &f.i, f.options, [this, fp] { Activate(*fp); }); break;
            }
            std::string tag = Tag(f);
            if (!tag.empty()) p.Tag(tag, TagColor(f));
            p.Hint(Hint(f)).Dim(!Usable(f));
        }
        set.Toggle("Allow experimental features", &settings.allowExperimental)
            .Hint("Runs features whose natives or arguments are not confirmed yet")
            .Int("Re-apply interval (ms)", &settings.reapplyMs, 100, 5000, 50)
            .Hint("How often held toggles are applied again")
            .Toggle("Status line", &settings.showStatus)
            .Hint("Position, heading and speed at the top left")
            .Button("Log feature availability", [this] { LogAvailability(); })
            .Hint("Writes every feature's state to shvi.log");
    }

    void LogAvailability() {
        int n = 0, ok = 0, exp = 0;
        for (auto& f : features_) {
            ++n;
            ok += f.available ? 1 : 0;
            exp += (f.available && f.experimental) ? 1 : 0;
            char line[512];
            std::snprintf(line, sizeof(line), "trainer: %-28s %s%s%s%s", f.id.c_str(), f.available ? "ok" : "n/a",
                          f.experimental ? " experimental" : "", f.available ? " weakest=" : " missing=",
                          f.available ? ConfName(f.weakest) : f.missing.c_str());
            Log(line);
        }
        char sum[128];
        std::snprintf(sum, sizeof(sum), "trainer: %d features, %d available, %d of them experimental", n, ok, exp);
        Log(sum);
    }

    static void Log(const std::string& s) {
        if (shvi::api() && shvi::api()->log) shvi::api()->log(s.c_str());
    }

private:
    // ---------------------------------------------------------------- registration
    Feature& Make(Category cat, Kind kind, const char* id, const char* label, std::vector<const char*> needs) {
        Feature f;
        f.cat = cat;
        f.kind = kind;
        f.id = id;
        f.label = label;
        f.needs = std::move(needs);
        features_.push_back(std::move(f));
        return features_.back();
    }
    Feature& Action(Category c, const char* id, const char* label, std::vector<const char*> needs,
                    std::function<void(Ctx&, Feature&)> run) {
        Feature& f = Make(c, Kind::Action, id, label, std::move(needs));
        f.run = std::move(run);
        return f;
    }
    Feature& Toggle(Category c, const char* id, const char* label, std::vector<const char*> needs, Hold hold,
                    std::function<void(Ctx&, Feature&)> run) {
        Feature& f = Make(c, Kind::Toggle, id, label, std::move(needs));
        f.hold = hold;
        f.run = std::move(run);
        return f;
    }
    Feature& Int(Category c, const char* id, const char* label, std::vector<const char*> needs, int v, int mn, int mx,
                 int step, std::function<void(Ctx&, Feature&)> run) {
        Feature& f = Make(c, Kind::Int, id, label, std::move(needs));
        f.i = v; f.imin = mn; f.imax = mx; f.istep = step;
        f.run = std::move(run);
        return f;
    }
    Feature& Float(Category c, const char* id, const char* label, std::vector<const char*> needs, float v, float mn,
                   float mx, float step, std::function<void(Ctx&, Feature&)> run) {
        Feature& f = Make(c, Kind::Float, id, label, std::move(needs));
        f.f = v; f.fmin = mn; f.fmax = mx; f.fstep = step;
        f.run = std::move(run);
        return f;
    }
    template <std::size_t N>
    Feature& Choice(Category c, const char* id, const char* label, std::vector<const char*> needs, const HashName (&tbl)[N],
                    std::function<void(Ctx&, Feature&)> run) {
        Feature& f = Make(c, Kind::Choice, id, label, std::move(needs));
        for (const auto& h : tbl)
            if (h.hash) f.options.emplace_back(h.name);
        if (f.options.empty()) f.options.emplace_back("(none)");
        f.run = std::move(run);
        return f;
    }
    Feature& ChoiceList(Category c, const char* id, const char* label, std::vector<const char*> needs,
                        std::vector<std::string> options, std::function<void(Ctx&, Feature&)> run) {
        Feature& f = Make(c, Kind::Choice, id, label, std::move(needs));
        f.options = std::move(options);
        f.run = std::move(run);
        return f;
    }
    template <std::size_t N>
    static std::uint32_t HashAt(const HashName (&tbl)[N], int index) {
        int k = 0;
        for (const auto& h : tbl)
            if (h.hash && k++ == index) return h.hash;
        return 0;
    }

    // Entities of one kind around the player, nearest first.
    static std::vector<Ref> Nearby(const Ctx& c, std::uint32_t kind, float radius, bool skipOwn) {
        std::vector<Ref> buf(512);
        int n = shvi::world_get_all(kind, buf.data(), static_cast<int>(buf.size()));
        if (n < 0) n = 0;
        buf.resize(static_cast<std::size_t>(std::min<int>(n, static_cast<int>(buf.size()))));
        Vec3 me = A::GET_ENTITY_COORDS(c.ped, true);
        std::vector<std::pair<float, Ref>> out;
        for (const Ref& r : buf) {
            if (!Valid(r) || (skipOwn && (Same(r, c.ped) || Same(r, c.veh)))) continue;
            float d = Dist2(A::GET_ENTITY_COORDS(r, false), me);
            if (d <= radius * radius) out.emplace_back(d, r);
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<Ref> refs;
        for (auto& p : out) refs.push_back(p.second);
        return refs;
    }

    static void MoveTo(const Ctx& c, const Vec3& p) {
        // SET_ENTITY_COORDS(entity, pos, 4 flags, float): layout verified; the script-typical call passes
        // the flags false and -1.0f.
        A::SET_ENTITY_COORDS(c.Target(), p, false, false, false, false, -1.0f);
    }

    static constexpr int kVitals = 6;  // the engine accepts index 0..5
    static void RestoreVitals(const Ref& ped) {
        for (int i = 0; i < kVitals; ++i) {
            float mx = A::VI_GET_PED_VITAL_MAX(ped, i);
            if (mx > 0.0f) A::VI_SET_PED_VITAL(ped, mx, i);
        }
    }
    // The 48-byte item block as scripts fill it: item hash +0x10, count +0x18, -1 +0x1C,
    // reason 0xE4DF46D5 +0x24, flag +0x28. Then the cancellation token (none) and the Task type.
    static void GiveItem(const Ref& ped, std::uint32_t item, int count) {
        std::uint64_t b[6] = {};
        b[2] = item;
        b[3] = static_cast<std::uint32_t>(count) | (0xFFFFFFFFull << 32);
        b[4] = 0xE4DF46D5ull << 32;
        b[5] = 1;
        std::uint64_t task = A::VI_GIVE_ITEM_TO_PED_ASYNC(ped, b[0], b[1], b[2], b[3], b[4], b[5], 0, kTaskTypeToken);
        LogTask("item give", item, task);
    }
    // What an async native handed back: a task object, or 0 when the call did nothing (unbound, faulted, refused).
    static void LogTask(const char* what, std::uint32_t key, std::uint64_t task) {
        char m[128];
        std::snprintf(m, sizeof(m), "trainer: %s 0x%08X -> task 0x%llX", what, key, static_cast<unsigned long long>(task));
        Log(m);
    }
    Vec3 Ahead(const Ctx& c) {
        return Add(A::GET_ENTITY_COORDS(c.ped, true), Mul(A::GET_ENTITY_FORWARD_VECTOR(c.ped), Find("spawn.distance")->f));
    }
    ModelKey SelectedModel() {
        Feature* m = Find("spawn.model");
        if (models_.empty() || m->i < 0 || m->i >= static_cast<int>(models_.size())) return 0;
        return models_[static_cast<std::size_t>(m->i)];
    }
    // The 12-cell block exactly as the game's own initializer (native.func_00D151) and its caller leave it:
    // model +0x00, -1 +0x1C, position +0x20, heading +0x30, byte +0x3A = 1, byte +0x45 = 1, -1.0f +0x54.
    void SpawnVehicle(const Ctx& c, ModelKey model) {
        if (!model) { Log("trainer: no model selected"); return; }
        Vec3 p = Ahead(c);
        std::uint64_t b[12] = {};
        b[0] = model;
        b[3] = 0xFFFFFFFFull << 32;
        b[4] = PackXY(p);
        b[5] = shvi_f2u(p.z);
        b[6] = shvi_f2u(A::GET_ENTITY_HEADING(c.ped));
        b[7] = 1ull << 16;
        b[8] = 1ull << 40;
        b[10] = static_cast<std::uint64_t>(shvi_f2u(-1.0f)) << 32;
        const char* experiment = std::getenv("SHVI_EXPERIMENTAL_VEHICLE");
        if (!experiment || std::strcmp(experiment, "1") || !shvi::api() ||
            !SHVI_API_HAS(shvi::api(), script_object_pin) || !SHVI_API_HAS(shvi::api(), script_object_unpin) ||
            !shvi::api()->script_object_pin || !shvi::api()->script_object_unpin) {
            Log("trainer: vehicle experiment requires opt-in and task pin support");
            return;
        }
        std::uint64_t args[13] = {};
        std::copy(std::begin(b), std::end(b), args);
        retry_.active = false;
        StartSpawnWatch(SHVI_ENTITY_VEHICLE, model, p);
        std::uint64_t task = 0;
        const int rc = callScript_(0x48391B87u, 0xFFFFFF01u, args, 13, &task, 1);
        LogScriptCall("experimental vehicle creation", model, rc, task);
        watch_.task = rc == SHVI_OK ? task : 0;
        if (!watch_.task) { EndWatch(); return; }
        PinWatchedTask();
        if (!watch_.pin) {
            Log("trainer: vehicle task could not be pinned; no further attempt will be made");
            EndWatch();
        }
    }
    // Peds and objects are created through the game's own script wrappers in module "native"
    // (program 0x48391B87), which pass a valid Task<T> type themselves:
    //   func_018247(model, pos (2 cells), heading, 3 flags, 2 cells, int, flag, 2 cells) -> task   (ped)
    //   func_01BE38(model, pos (2 cells), 2 flags, 1 cell, 2 flags, int, cancellation token) -> task (object)
    // The offsets are for build 1794606036. The flags' meaning is not known; they are left false.
    static constexpr std::uint32_t kNativeModule = 0x48391B87u, kCreatePedWrapper = 0x18247u, kCreateObjectWrapper = 0x1BE38u;
    void LogScriptCall(const char* what, ModelKey model, int rc, std::uint64_t task) {
        char m[160];
        std::snprintf(m, sizeof(m), "trainer: %s 0x%llX -> status %d, task 0x%llX", what, static_cast<unsigned long long>(model), rc, static_cast<unsigned long long>(task));
        Log(m);
        if (rc == SHVI_OK && task) DumpTask(task, "at return");
    }
    static void BumpWord(std::uint64_t at) { guarded::Bump16(static_cast<std::uintptr_t>(at)); }
    static bool ReadWordsAt(std::uint64_t at, std::uint64_t* q, int n) {
        return guarded::Copy(q, static_cast<std::uintptr_t>(at), static_cast<std::size_t>(n) * 8);
    }
    void DumpTask(std::uint64_t task, const char* when) {  // the 24-byte object header sits before the reference
        std::uint64_t q[12] = {};
        char d[360];
        if (!ReadWordsAt(task - 0x20, q, 12)) { Log("trainer: task unreadable"); return; }
        std::snprintf(d, sizeof(d), "trainer: task %s -0x20 %llX %llX %llX %llX | +0x00 %llX %llX %llX %llX | +0x20 %llX %llX %llX %llX", when,
                      (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3],
                      (unsigned long long)q[4], (unsigned long long)q[5], (unsigned long long)q[6], (unsigned long long)q[7],
                      (unsigned long long)q[8], (unsigned long long)q[9], (unsigned long long)q[10], (unsigned long long)q[11]);
        Log(d);
    }
    // After a creation request: look for a new entity of that model near the requested point, for 90 s.
    struct CollectionScan {
        bool active = false;
        int index = -1, best = -1, bestCount = -1;
        std::uint32_t next = 0;
    };
    CollectionScan scan_;
    int CountListedWeapons(const Ref& ped) {
        int n = 0;
        for (const HashName& w : kWeapons) n += A::HAS_PED_GOT_WEAPON(ped, w.hash, 0, false) ? 1 : 0;
        return n;
    }
    void StepCollectionScan() {
        if (!scan_.active || ctx_.nowMs < scan_.next || !Valid(ctx_.ped)) return;
        const int total = static_cast<int>(std::size(kWeaponCollections));
        char b[160];
        if (scan_.index >= 0 && scan_.index < total) {  // result of the collection applied last time
            int n = CountListedWeapons(ctx_.ped);
            std::snprintf(b, sizeof(b), "trainer: collection %d %s (0x%08X): %d listed weapons", scan_.index, kWeaponCollections[scan_.index].name,
                          kWeaponCollections[scan_.index].hash, n);
            Log(b);
            if (n > scan_.bestCount) { scan_.bestCount = n; scan_.best = scan_.index; }
        }
        ++scan_.index;
        while (scan_.index < total && kWeaponCollections[scan_.index].hash == 0x066532DDu) ++scan_.index;  // the type constant, not a collection
        if (scan_.index < total) {
            A::_GIVE_WEAPON_COLLECTION_TO_PED(ctx_.ped, kWeaponCollections[scan_.index].hash, 0x066532DDu);
            scan_.next = ctx_.nowMs + 5000;  // the weapons arrive some seconds after the call (0.5 s read nothing, run 48)
            return;
        }
        scan_.active = false;
        if (scan_.best >= 0) {
            A::_GIVE_WEAPON_COLLECTION_TO_PED(ctx_.ped, kWeaponCollections[scan_.best].hash, 0x066532DDu);
            std::snprintf(b, sizeof(b), "trainer: largest collection is %d %s with %d listed weapons; applied", scan_.best,
                          kWeaponCollections[scan_.best].name, scan_.bestCount);
            Log(b);
        }
    }
    struct SpawnWatch {
        bool active = false;
        std::uint32_t kind = 0;
        ModelKey model = 0;
        std::uint32_t until = 0, next = 0;
        Vec3 pos{};
        int before = 0;
        std::uint64_t task = 0;  // the Task the wrapper returned; scripts read its result at +0x28 (native.func_00F428)
        std::uint64_t pin = 0;   // ABI 1.7 pin that keeps the task alive while it is watched
        std::vector<Ref> known;  // every entity of that kind that existed before the request
        int newLogged = 0;
        int dumps = 0, changes = 0;
        std::uint64_t last[12] = {};
    };
    static bool ReadTaskWords(std::uint64_t task, std::uint64_t* q) {
        return guarded::Copy(q, static_cast<std::uintptr_t>(task), 64);
    }
    // The task is a script object we do not own; it may be gone. Read it under SEH.
    static bool ReadTaskResult(std::uint64_t task, Ref* out) {
        return guarded::Copy(out, static_cast<std::uintptr_t>(task) + 0x28, sizeof(Ref));
    }
    SpawnWatch watch_;
    Ref lastSpawned_{};
    Ref lastCharacter_{};
    int CountModelNear(std::uint32_t kind, ModelKey model, const Vec3& pos, float radius) {
        if (!exists_("GET_ENTITY_MODEL") || !exists_("GET_ENTITY_COORDS")) return -1;
        std::vector<Ref> buf(512);
        int n = shvi::world_get_all(kind, buf.data(), static_cast<int>(buf.size()));
        int hits = 0;
        for (int i = 0; i < n && i < static_cast<int>(buf.size()); ++i) {
            const Ref& r = buf[static_cast<std::size_t>(i)];
            if (!Valid(r) || ModelOf(r) != model) continue;
            if (Dist2(A::GET_ENTITY_COORDS(r, false), pos) <= radius * radius) ++hits;
        }
        return hits;
    }
    void StartSpawnWatch(std::uint32_t kind, ModelKey model, const Vec3& pos) {
        if (watch_.active) EndWatch();
        watch_ = SpawnWatch{true, kind, model, ctx_.nowMs + 90000, ctx_.nowMs + 1000, pos, CountModelNear(kind, model, pos, 30.0f)};
        watch_.known.resize(1024);
        int n = shvi::world_get_all(kind, watch_.known.data(), 1024);
        watch_.known.resize(static_cast<std::size_t>(n < 0 ? 0 : (n > 1024 ? 1024 : n)));
    }
    // Any entity of the watched kind that did not exist before the request, wherever it is.
    void LogNewEntities() {
        if (!watch_.active || watch_.newLogged >= 12) return;
        std::vector<Ref> all(1024);
        int n = shvi::world_get_all(watch_.kind, all.data(), static_cast<int>(all.size()));
        for (int i = 0; i < n && i < static_cast<int>(all.size()); ++i) {
            const Ref& r = all[static_cast<std::size_t>(i)];
            if (!Valid(r)) continue;
            bool old = false;
            for (const Ref& k : watch_.known)
                if (k.lo == r.lo && k.hi == r.hi) { old = true; break; }
            if (old) continue;
            watch_.known.push_back(r);
            ModelKey mdl = ModelOf(r);
            if (mdl != watch_.model) continue;  // ambient entities come and go; only our model matters
            Vec3 at = A::GET_ENTITY_COORDS(r, true);
            char b[220];
            std::snprintf(b, sizeof(b), "trainer: NEW entity with the requested model 0x%llX: %llX:%llX at %.1f %.1f %.1f (%.1f m from the request point)", static_cast<unsigned long long>(mdl),
                          (unsigned long long)r.lo, (unsigned long long)r.hi, at.x, at.y, at.z, std::sqrt(Dist2(at, watch_.pos)));
            Log(b);
            lastSpawned_ = r;
            ++watch_.newLogged;
        }
    }
    void CheckSpawnWatch() {
        LogNewEntities();
        if (watch_.active && watch_.task && watch_.changes < 14) {  // every tick: log the task whenever it changes
            std::uint64_t q[12] = {};
            if (ReadWordsAt(watch_.task - 0x20, q, 12) && std::memcmp(q, watch_.last, sizeof(q)) != 0) {
                std::memcpy(watch_.last, q, sizeof(q));
                ++watch_.changes;
                DumpTask(watch_.task, "changed");
                Ref made{q[9], q[10]};  // +0x28
                if (Valid(made)) {
                    char d[200];
                    Vec3 at = A::GET_ENTITY_COORDS(made, false);
                    std::snprintf(d, sizeof(d), "trainer: task result %llX:%llX, model 0x%llX, at %.1f %.1f %.1f", (unsigned long long)made.lo,
                                  (unsigned long long)made.hi, static_cast<unsigned long long>(ModelOf(made)), at.x, at.y, at.z);
                    Log(d);
                    lastSpawned_ = made;
                }
            }
        }
        if (!watch_.active || ctx_.nowMs < watch_.next) return;
        watch_.next = ctx_.nowMs + 1000;
        int now = CountModelNear(watch_.kind, watch_.model, watch_.pos, 30.0f);
        char m[200];
        Ref made{};
        if (watch_.task && ReadTaskResult(watch_.task, &made) && Valid(made)) {
            ModelKey mdl = ModelOf(made);
            Vec3 at = A::GET_ENTITY_COORDS(made, false);
            std::snprintf(m, sizeof(m), "trainer: spawn confirmed by the task: reference %llX:%llX, model 0x%llX, at %.1f %.1f %.1f",
                          static_cast<unsigned long long>(made.lo), static_cast<unsigned long long>(made.hi), static_cast<unsigned long long>(mdl), at.x, at.y, at.z);
            Log(m);
            lastSpawned_ = made;
            EndWatch();
            return;
        }
        if (watch_.task && (watch_.dumps < 3 || watch_.dumps % 10 == 0)) {
            DumpTask(watch_.task, "later");
            std::vector<Ref> all(512);
            char d[96];
            std::snprintf(d, sizeof(d), "trainer: %d entities of that kind in the world", shvi::world_get_all(watch_.kind, all.data(), 512));
            Log(d);
        }
        ++watch_.dumps;
        if (now > watch_.before) {
            std::snprintf(m, sizeof(m), "trainer: spawn confirmed: %d entit%s of model 0x%llX near the spawn point (was %d)", now, now == 1 ? "y" : "ies",
                          static_cast<unsigned long long>(watch_.model), watch_.before);
            Log(m);
            EndWatch();
        } else if (ctx_.nowMs >= watch_.until) {
            std::snprintf(m, sizeof(m), "trainer: spawn not seen after 90 s: %d of model 0x%llX near the spawn point (was %d)", now, static_cast<unsigned long long>(watch_.model), watch_.before);
            Log(m);
            EndWatch();
        }
    }
    void SpawnPed(const Ctx& c, ModelKey model) {
        if (!model) { Log("trainer: no model selected"); return; }
        Vec3 p = Ahead(c);
        StartSpawnWatch(SHVI_ENTITY_PED, model, p);
        std::uint64_t a[13] = {};
        a[0] = model;
        a[1] = PackXY(p);
        a[2] = shvi_f2u(p.z);
        a[3] = shvi_f2u(A::GET_ENTITY_HEADING(c.ped));
        a[6] = 1;   // the third flag and the one after the int are 1 where a shipped script calls the native
        a[10] = 1;  // directly (lib_C3607FD5 func_06F906); the two cells and the int between them stay 0
        std::uint64_t task = 0;
        int rc = callScript_(kNativeModule, kCreatePedWrapper, a, 13, &task, 1);
        LogScriptCall("ped creation", model, rc, task);
        watch_.task = rc == SHVI_OK ? task : 0;
        if (FailedAtOnce(watch_.task)) { ScheduleRetry(true, model); return; }
        retry_.active = false;
        PinWatchedTask();
    }
    // A creation that fails one of the engine's gates (0x14115B2F0: no script instance that may own entities on
    // this thread, model, pool) hands back a task that is already complete (status 5) with no entity. The owner
    // depends on the script thread the tick borrowed, so the same request on a later tick can pass.
    static bool FailedAtOnce(std::uint64_t task) {
        std::uint64_t q[4] = {};
        if (!task || !ReadWordsAt(task, q, 4)) return false;
        Ref made{};
        ReadTaskResult(task, &made);
        return ((q[2] >> 16) & 0xFF) == 5 && !Valid(made);
    }
    struct Retry {
        bool active = false, ped = true;
        ModelKey model = 0;
        int attempts = 0;
        std::uint32_t next = 0;
    };
    Retry retry_;
    void ScheduleRetry(bool ped, ModelKey model) {
        if (!retry_.active || retry_.model != model || retry_.ped != ped) retry_ = Retry{true, ped, model, 0, 0};
        if (++retry_.attempts > 30) {
            Log("trainer: creation still refused after 30 tries (model, position or no owning script on any thread)");
            retry_.active = false;
            EndWatch();
            return;
        }
        retry_.next = ctx_.nowMs + 100;
        watch_.task = 0;
        if (retry_.attempts == 1 || retry_.attempts % 10 == 0) {
            char m[128];
            std::snprintf(m, sizeof(m), "trainer: creation refused at once (try %d); trying again on a later tick", retry_.attempts);
            Log(m);
        }
    }
    void StepRetry() {
        if (!retry_.active || ctx_.nowMs < retry_.next || !Valid(ctx_.ped)) return;
        if (retry_.ped) SpawnPed(ctx_, retry_.model);
        else SpawnObject(ctx_, retry_.model);
    }
    // The async native registers its task under a weak handle, so the runtime reclaims it within frames and the
    // request dies with it (runs 18-22, 58). A strong, script-owned pin (ABI 1.7) keeps it until the watch ends.
    void PinWatchedTask() {
        if (!watch_.task) return;
        std::uint64_t pin = 0;
        int rc = pinScript_(watch_.task, &pin);
        watch_.pin = rc == SHVI_OK ? pin : 0;
        char m[128];
        std::snprintf(m, sizeof(m), "trainer: task 0x%llX pinned: status %d, pin %llu", static_cast<unsigned long long>(watch_.task), rc,
                      static_cast<unsigned long long>(pin));
        Log(m);
    }
    void EndWatch() {
        watch_.active = false;
        if (watch_.pin) {
            int rc = unpinScript_(watch_.pin);
            char m[96];
            std::snprintf(m, sizeof(m), "trainer: task unpinned: status %d", rc);
            Log(m);
            watch_.pin = 0;
        }
    }
    void SpawnObject(const Ctx& c, ModelKey model) {
        if (!model) { Log("trainer: no model selected"); return; }
        Vec3 p = Ahead(c);
        StartSpawnWatch(SHVI_ENTITY_OBJECT, model, p);
        std::uint64_t a[10] = {};
        a[0] = model;
        a[1] = PackXY(p);
        a[2] = shvi_f2u(p.z);
        std::uint64_t task = 0;
        int rc = callScript_(kNativeModule, kCreateObjectWrapper, a, 10, &task, 1);
        LogScriptCall("object creation", model, rc, task);
        watch_.task = rc == SHVI_OK ? task : 0;
        if (FailedAtOnce(watch_.task)) { ScheduleRetry(false, model); return; }
        retry_.active = false;
        PinWatchedTask();
    }

    void UpdateStatus() {
        if (!exists_("GET_ENTITY_COORDS")) return;
        Vec3 p = A::GET_ENTITY_COORDS(ctx_.Target(), true);
        float h = exists_("GET_ENTITY_HEADING") ? A::GET_ENTITY_HEADING(ctx_.Target()) : 0.0f;
        float s = exists_("GET_ENTITY_SPEED") ? A::GET_ENTITY_SPEED(ctx_.Target()) : 0.0f;
        char b[160];
        std::snprintf(b, sizeof(b), "X %.1f  Y %.1f  Z %.1f  H %.0f  %.0f km/h%s", p.x, p.y, p.z, h * kRadToDeg, s * 3.6f,
                      ctx_.inVehicle ? "  [vehicle]" : "");
        status_ = b;
    }

    void Register() {
        using C = Category;
        const Ref none{};

        // ============================================================ PLAYER
        Toggle(C::Player, "player.god", "Invincible", {"SET_ENTITY_INVINCIBLE"}, Hold::Interval,
               [](Ctx& c, Feature& f) { A::SET_ENTITY_INVINCIBLE(c.ped, f.on); });
        // The second story character's ped exists all the time, parked off-world (seen in game, run 23).
        Action(C::Player, "player.bringsecond", "Bring the second character here", {"VI_GET_SECONDARY_PLAYER_PED", "SET_ENTITY_COORDS"},
               [this](Ctx& c, Feature&) {
                   Ref o = A::VI_GET_SECONDARY_PLAYER_PED();
                   if (!Valid(o) || (o.lo == c.ped.lo && o.hi == c.ped.hi)) { Log("trainer: no second character ped"); return; }
                   A::SET_ENTITY_COORDS(o, Ahead(c), false, false, false, false, -1.0f);
               });
        Action(C::Teleport, "teleport.tosecond", "Teleport to the second character", {"VI_GET_SECONDARY_PLAYER_PED", "GET_ENTITY_COORDS", "SET_ENTITY_COORDS"},
               [this](Ctx& c, Feature&) {
                   Ref o = A::VI_GET_SECONDARY_PLAYER_PED();
                   if (!Valid(o) || (o.lo == c.ped.lo && o.hi == c.ped.hi)) { Log("trainer: no second character ped"); return; }
                   Vec3 q = A::GET_ENTITY_COORDS(o, false);
                   if (q.z > 2000.0f) { Log("trainer: the second character is parked off-world; bring it here instead"); return; }
                   A::SET_ENTITY_COORDS(c.Target(), Vec3{q.x + 1.0f, q.y, q.z}, false, false, false, false, -1.0f);
               });
        Action(C::Player, "player.heal", "Heal", {"SET_ENTITY_HEALTH", "GET_ENTITY_MAX_HEALTH"}, [none](Ctx& c, Feature&) {
            A::SET_ENTITY_HEALTH(c.ped, A::GET_ENTITY_MAX_HEALTH(c.ped, false), none, false, false);
        });
        Toggle(C::Player, "player.autoheal", "Keep health full", {"SET_ENTITY_HEALTH", "GET_ENTITY_MAX_HEALTH", "GET_ENTITY_HEALTH"},
               Hold::Interval, [none](Ctx& c, Feature& f) {
                   if (!f.on) return;
                   int mx = A::GET_ENTITY_MAX_HEALTH(c.ped, false);
                   if (A::GET_ENTITY_HEALTH(c.ped) < mx) A::SET_ENTITY_HEALTH(c.ped, mx, none, false, false);
               });
        Action(C::Player, "player.vitals", "Restore all vitals (health, armour, ...)", {"VI_SET_PED_VITAL", "VI_GET_PED_VITAL_MAX"},
               [](Ctx& c, Feature&) { RestoreVitals(c.ped); });
        Toggle(C::Player, "player.keepvitals", "Keep all vitals full", {"VI_SET_PED_VITAL", "VI_GET_PED_VITAL_MAX", "VI_GET_PED_VITAL"}, Hold::Interval,
               [](Ctx& c, Feature& f) {
                   if (!f.on) return;
                   for (int i = 0; i < kVitals; ++i) {
                       float mx = A::VI_GET_PED_VITAL_MAX(c.ped, i);
                       if (mx > 0.0f && A::VI_GET_PED_VITAL(c.ped, i) < mx) A::VI_SET_PED_VITAL(c.ped, mx, i);
                   }
               });
        Int(C::Player, "player.vitalindex", "Vital index", {}, 0, 0, kVitals - 1, 1, [](Ctx&, Feature&) {});
        Float(C::Player, "player.vitalvalue", "Set that vital (fraction of max)", {"VI_SET_PED_VITAL", "VI_GET_PED_VITAL_MAX"}, 1.0f, 0.0f, 1.0f, 0.1f,
              [this](Ctx& c, Feature& f) {
                  int i = Find("player.vitalindex")->i;
                  float mx = A::VI_GET_PED_VITAL_MAX(c.ped, i);
                  if (mx > 0.0f) A::VI_SET_PED_VITAL(c.ped, mx * f.f, i);
              });
        Int(C::Player, "player.maxhealth", "Max health", {"SET_PED_MAX_HEALTH", "SET_ENTITY_HEALTH"}, 200, 100, 10000, 100,
            [none](Ctx& c, Feature& f) {
                A::SET_PED_MAX_HEALTH(c.ped, f.i);
                A::SET_ENTITY_HEALTH(c.ped, f.i, none, false, false);
            });
        Float(C::Player, "player.regen", "Health recharge multiplier", {"SET_PLAYER_HEALTH_RECHARGE_MULTIPLIER", "GET_PLAYER_INDEX"},
              1.0f, 0.0f, 20.0f, 0.5f,
              [](Ctx&, Feature& f) { A::SET_PLAYER_HEALTH_RECHARGE_MULTIPLIER(A::GET_PLAYER_INDEX(), f.f); });
        Action(C::Player, "player.clearwanted", "Clear wanted level", {"CLEAR_PLAYER_WANTED_LEVEL"},
               [](Ctx& c, Feature&) { A::CLEAR_PLAYER_WANTED_LEVEL(c.ped); });
        Toggle(C::Player, "player.neverwanted", "Never wanted", {"CLEAR_PLAYER_WANTED_LEVEL", "SET_MAX_WANTED_LEVEL", "GET_PLAYER_WANTED_LEVEL"},
               Hold::Interval, [](Ctx& c, Feature& f) {
                   if (f.on) {
                       A::SET_MAX_WANTED_LEVEL(0);
                       if (A::GET_PLAYER_WANTED_LEVEL(c.ped) > 0) A::CLEAR_PLAYER_WANTED_LEVEL(c.ped);
                   } else {
                       A::SET_MAX_WANTED_LEVEL(5);  // the command clamps at 6; 5 is the stock story maximum
                   }
               });
        Int(C::Player, "player.setwanted", "Set wanted level", {"VI_RAISE_PLAYER_WANTED_LEVEL", "CLEAR_PLAYER_WANTED_LEVEL"}, 0, 0, 5, 1,
            [](Ctx& c, Feature& f) {
                // the raise native never lowers, so clear first
                A::CLEAR_PLAYER_WANTED_LEVEL(c.ped);
                if (f.i > 0) A::VI_RAISE_PLAYER_WANTED_LEVEL(c.ped, f.i);
            });
        Int(C::Player, "player.maxwanted", "Max wanted level", {"SET_MAX_WANTED_LEVEL"}, 5, 0, 6, 1,
            [](Ctx&, Feature& f) { A::SET_MAX_WANTED_LEVEL(f.i); });
        Toggle(C::Player, "player.wantedoff", "Disable wanted system", {"_SET_DISABLE_PLAYER_WANTED_LEVEL"}, Hold::Interval,
               [](Ctx& c, Feature& f) { A::_SET_DISABLE_PLAYER_WANTED_LEVEL(c.ped, f.on); });
        Toggle(C::Player, "player.ignored", "Everyone ignores player", {"SET_EVERYONE_IGNORE_PLAYER"}, Hold::Interval,
               [](Ctx& c, Feature& f) { A::SET_EVERYONE_IGNORE_PLAYER(c.ped, f.on); });
        Toggle(C::Player, "player.policeignore", "Police ignore player", {"SET_POLICE_IGNORE_PLAYER"}, Hold::Interval,
               [](Ctx& c, Feature& f) { A::SET_POLICE_IGNORE_PLAYER(c.ped, f.on); });
        Toggle(C::Player, "player.invisible", "Invisible", {"SET_ENTITY_VISIBLE"}, Hold::Interval,
               [](Ctx& c, Feature& f) { A::SET_ENTITY_VISIBLE(c.ped, !f.on); });
        Int(C::Player, "player.alpha", "Opacity", {"SET_ENTITY_ALPHA"}, 255, 0, 255, 15,
            [](Ctx& c, Feature& f) { A::SET_ENTITY_ALPHA(c.ped, f.i, false); });
        Toggle(C::Player, "player.noragdoll", "No ragdoll", {"SET_PED_CAN_RAGDOLL"}, Hold::Interval,
               [](Ctx& c, Feature& f) { A::SET_PED_CAN_RAGDOLL(c.ped, !f.on); });
        Toggle(C::Player, "player.nodamage", "Cannot be damaged", {"SET_ENTITY_CAN_BE_DAMAGED"}, Hold::Interval,
               [](Ctx& c, Feature& f) { A::SET_ENTITY_CAN_BE_DAMAGED(c.ped, !f.on); });
        Toggle(C::Player, "player.proofs", "All damage proofs", {"SET_ENTITY_PROOFS"}, Hold::Interval,
               [](Ctx& c, Feature& f) { A::SET_ENTITY_PROOFS(c.ped, f.on ? 0x7FFFFFFF : 0, f.on); })
            .guess = true;  // VI takes a bit set here (GTA V took eight bools); which bit is which is not known
        Toggle(C::Player, "player.stamina", "Infinite stamina", {"_RESTORE_PED_STAMINA", "_GET_PED_MAX_STAMINA"}, Hold::Interval,
               [](Ctx& c, Feature& f) {
                   if (f.on) A::_RESTORE_PED_STAMINA(c.ped, A::_GET_PED_MAX_STAMINA(c.ped));
               });
        Float(C::Player, "player.moverate", "Move rate", {"SET_PED_MOVE_RATE_OVERRIDE"}, 1.0f, 0.2f, 5.0f, 0.1f,
              [](Ctx& c, Feature& f) { A::SET_PED_MOVE_RATE_OVERRIDE(c.ped, f.f); });
        Toggle(C::Player, "player.fastrun", "Hold move rate", {"SET_PED_MOVE_RATE_OVERRIDE"}, Hold::Frame,
               [this](Ctx& c, Feature& f) {
                   Feature* r = Find("player.moverate");
                   if (f.on && r) A::SET_PED_MOVE_RATE_OVERRIDE(c.ped, r->f);
               });
        Toggle(C::Player, "player.freeze", "Freeze position", {"FREEZE_ENTITY_POSITION"}, Hold::None,
               [](Ctx& c, Feature& f) { A::FREEZE_ENTITY_POSITION(c.ped, f.on); });
        Toggle(C::Player, "player.nogravity", "No gravity", {"SET_PED_GRAVITY"}, Hold::Interval,
               [](Ctx& c, Feature& f) { A::SET_PED_GRAVITY(c.ped, !f.on); });
        Toggle(C::Player, "player.nocollision", "No collision", {"SET_ENTITY_COLLISION"}, Hold::Interval,
               [](Ctx& c, Feature& f) { A::SET_ENTITY_COLLISION(c.ped, !f.on, false, false); })
            .checked = "2026-11-26: GET_ENTITY_COLLISION_DISABLED read 1 on, 0 off";  // VI added a second bool; both are set alike here
        Action(C::Player, "player.clean", "Clean blood and water", {"CLEAR_PED_BLOOD_DAMAGE", "CLEAR_PED_WETNESS"},
               [](Ctx& c, Feature&) {
                   A::CLEAR_PED_BLOOD_DAMAGE(c.ped);
                   A::CLEAR_PED_WETNESS(c.ped);
               });
        // Risky on the player while a script is driving it: in the game's first scene this killed the game in
        // 3 of 7 uses (runs 35, 36 and 45; a fatal write on an engine thread at image+0x2215C35), with either
        // value of the last flag. It is also what frees the ped from the scripted action there. Experimental.
        Action(C::Player, "player.stoptasks", "Stop current action (can crash in scripted scenes)", {"CLEAR_PED_TASKS_IMMEDIATELY"},
               [](Ctx& c, Feature&) { A::CLEAR_PED_TASKS_IMMEDIATELY(c.ped, false, false); }).guess = true;
        Action(C::Player, "player.suicide", "Suicide", {"SET_ENTITY_HEALTH"},
               [none](Ctx& c, Feature&) { A::SET_ENTITY_HEALTH(c.ped, 0, none, false, false); });

        Action(C::Player, "player.launch", "Launch upwards", {"SET_ENTITY_VELOCITY"},
               [](Ctx& c, Feature&) { A::SET_ENTITY_VELOCITY(c.ped, V(0, 0, 18.0f)); });
        Toggle(C::Player, "player.entitygravity", "No gravity (entity)", {"SET_ENTITY_HAS_GRAVITY"}, Hold::Interval,
               [](Ctx& c, Feature& f) { A::SET_ENTITY_HAS_GRAVITY(c.ped, !f.on); });
        Int(C::Player, "player.accuracy", "Accuracy", {"SET_PED_ACCURACY"}, 100, 0, 100, 10,
            [](Ctx& c, Feature& f) { A::SET_PED_ACCURACY(c.ped, f.i); });
        Float(C::Player, "player.sweat", "Sweat", {"SET_PED_SWEAT"}, 0.0f, 0.0f, 100.0f, 10.0f,
              [](Ctx& c, Feature& f) { A::SET_PED_SWEAT(c.ped, f.f); });
        Action(C::Player, "player.extinguish", "Put out fire on player", {"STOP_ENTITY_FIRE"},
               [](Ctx& c, Feature&) { A::STOP_ENTITY_FIRE(c.ped, false, true); });
        Toggle(C::Player, "player.onlyplayerdamage", "Only damaged by player", {"SET_ENTITY_ONLY_DAMAGED_BY_PLAYER"}, Hold::Interval,
               [](Ctx& c, Feature& f) { A::SET_ENTITY_ONLY_DAMAGED_BY_PLAYER(c.ped, f.on); });

        Action(C::Player, "player.model", "Change player model: model selected in SPAWN",
               {"SET_PLAYER_MODEL", "GET_PLAYER_INDEX", "REQUEST_MODEL", "HAS_MODEL_LOADED", "IS_MODEL_VALID", "SET_MODEL_AS_NO_LONGER_NEEDED"},
               [this](Ctx&, Feature&) {
                   ModelKey model = SelectedModel();
                   using shvi::invoke;
                   using shvi::sdk::EngineHash;
                   if (!model || !ModelQuery("IS_MODEL_VALID", model)) { Log("trainer: model not valid (models are 64-bit keys)"); return; }
                   invoke<void>(EngineHash("REQUEST_MODEL"), model, false);
                   for (int i = 0; i < 200 && !ModelQuery("HAS_MODEL_LOADED", model); ++i) shvi::wait(0);
                   if (ModelQuery("HAS_MODEL_LOADED", model)) invoke<void>(EngineHash("SET_PLAYER_MODEL"), A::GET_PLAYER_INDEX(), model, false);
                   invoke<void>(EngineHash("SET_MODEL_AS_NO_LONGER_NEEDED"), model);
               }).guess = true;  // GTA V's sequence; VI's extra flag arguments are left false

        Toggle(C::Player, "player.breath", "Unlimited breath underwater", {"SET_PED_MAX_TIME_UNDERWATER", "SET_PED_MAX_TIME_IN_WATER"}, Hold::Interval,
               [](Ctx& c, Feature& f) {
                   // seconds; scripts pass values of this order to lift the limit. Off restores a short default.
                   A::SET_PED_MAX_TIME_UNDERWATER(c.ped, f.on ? 100000.0f : 30.0f);
                   A::SET_PED_MAX_TIME_IN_WATER(c.ped, f.on ? 100000.0f : 30.0f);
               }).guess = true;  // the stock limits are not known, so "off" is an assumed 30 s
        Action(C::Player, "player.jump", "Jump", {"TASK_JUMP"}, [](Ctx& c, Feature&) { A::TASK_JUMP(c.ped, true); });
        Action(C::Player, "player.ability", "Activate special ability", {"_SPECIAL_ABILITY_SET_ACTIVATE"},
               [](Ctx& c, Feature&) { A::_SPECIAL_ABILITY_SET_ACTIVATE(c.ped, 0); });
        // The ability meter (types 0 and 1 are the only ones scripts use). Names read from the code, not yet
        // confirmed in game: experimental through their confidence.
        Action(C::Player, "player.abilitycharge", "Charge special ability", {"VI_CHARGE_SPECIAL_ABILITY"}, [](Ctx& c, Feature&) {
            A::VI_CHARGE_SPECIAL_ABILITY(c.ped, 0, 1.0f, false, false);  // the form scripts use
            A::VI_CHARGE_SPECIAL_ABILITY(c.ped, 1, 1.0f, false, false);
        });
        // The amount getter is not a 0..1 fraction (read 10.0 in game), so this charges on every interval.
        Toggle(C::Player, "player.abilityfull", "Keep special ability charged", {"VI_CHARGE_SPECIAL_ABILITY"}, Hold::Interval,
               [](Ctx& c, Feature& f) {
                   if (!f.on) return;
                   for (int t = 0; t < 2; ++t) A::VI_CHARGE_SPECIAL_ABILITY(c.ped, t, 1.0f, false, false);
               });
        Action(C::Player, "player.abilityreset", "Reset special ability", {"VI_RESET_PLAYER_SPECIAL_ABILITY"},
               [](Ctx& c, Feature&) { A::VI_RESET_PLAYER_SPECIAL_ABILITY(c.ped, 0); });
        Toggle(C::Player, "player.abilityoff", "Disable special ability", {"_SPECIAL_ABILITY_SET_DISABLED"}, Hold::Interval,
               [](Ctx& c, Feature& f) { A::_SPECIAL_ABILITY_SET_DISABLED(c.ped, 0, f.on); });
        Toggle(C::Player, "player.autowalk", "Auto-walk forward", {"SIMULATE_PLAYER_INPUT_GAIT", "GET_PLAYER_INDEX"}, Hold::Interval,
               [](Ctx&, Feature& f) {
                   // as most scripts call it: speed 1.0, a duration in ms, heading 0, both flags false
                   if (f.on) A::SIMULATE_PLAYER_INPUT_GAIT(A::GET_PLAYER_INDEX(), 1.0f, 1000, 0.0f, false, false);
               });

        // ============================================================ WEAPONS
        Toggle(C::Weapons, "weapon.infclip", "Infinite ammo (no reload)", {"SET_PED_INFINITE_AMMO_CLIP"}, Hold::Interval,
               [](Ctx& c, Feature& f) { A::SET_PED_INFINITE_AMMO_CLIP(c.ped, f.on); });
        Action(C::Weapons, "weapon.refill", "Refill current weapon", {"_REFILL_AMMO_IN_CURRENT_PED_WEAPON"},
               [](Ctx& c, Feature&) { A::_REFILL_AMMO_IN_CURRENT_PED_WEAPON(c.ped); });
        Toggle(C::Weapons, "weapon.autorefill", "Keep current weapon full", {"_REFILL_AMMO_IN_CURRENT_PED_WEAPON"}, Hold::Interval,
               [](Ctx& c, Feature& f) {
                   if (f.on) A::_REFILL_AMMO_IN_CURRENT_PED_WEAPON(c.ped);
               });
        Action(C::Weapons, "weapon.fillclip", "Fill clip of best weapon",
               {"GET_BEST_PED_WEAPON", "GET_MAX_AMMO_IN_CLIP", "SET_AMMO_IN_CLIP"}, [](Ctx& c, Feature&) {
                   std::uint32_t w = A::GET_BEST_PED_WEAPON(c.ped, false, true);
                   if (w) A::SET_AMMO_IN_CLIP(c.ped, w, A::GET_MAX_AMMO_IN_CLIP(c.ped, w));
               });
        Choice(C::Weapons, "weapon.collection", "Give weapon collection", {"_GIVE_WEAPON_COLLECTION_TO_PED"}, kWeaponCollections,
               [](Ctx& c, Feature& f) {
                   // scripts call it as (ped, collection, 0x066532DD "LoadOut")
                   std::uint32_t h = HashAt(kWeaponCollections, f.i);
                   if (h) A::_GIVE_WEAPON_COLLECTION_TO_PED(c.ped, h, 0x066532DDu);
               }).checked = "runs 47, 50 and 2026-11-26: the listed weapons change with each collection (1 to 4)";
        // A collection replaces the ped's weapons (seen in game, run 47: 1 -> 2 -> 4 -> 2 listed weapons). This
        // applies each collection in turn, counts the listed weapons the ped then has,
        // logs the table and ends on the largest one. Five seconds apart: the weapons arrive late.
        Action(C::Weapons, "weapon.bestcollection", "Give the largest weapon collection",
               {"_GIVE_WEAPON_COLLECTION_TO_PED", "HAS_PED_GOT_WEAPON"}, [this](Ctx&, Feature&) {
                   scan_ = CollectionScan{};
                   scan_.active = true;
                   Log("trainer: trying every weapon collection");
               }).checked = "2026-11-26: scanned all collections, applied the largest (4 listed weapons)";
        Choice(C::Weapons, "weapon.equip", "Equip weapon", {"SET_CURRENT_PED_WEAPON"}, kWeapons, [](Ctx& c, Feature& f) {
            std::uint32_t h = HashAt(kWeapons, f.i);
            if (h) A::SET_CURRENT_PED_WEAPON(c.ped, h, true, 0, false, false, 4);
        });
        Int(C::Weapons, "weapon.givecount", "Give: count", {}, 1, 1, 10, 1, [](Ctx&, Feature&) {});
        Action(C::Weapons, "weapon.give", "Give the weapon selected above (async)", {"VI_GIVE_ITEM_TO_PED_ASYNC"}, [this](Ctx& c, Feature&) {
            std::uint32_t h = HashAt(kWeapons, Find("weapon.equip")->i);
            if (h) GiveItem(c.ped, h, Find("weapon.givecount")->i);
        }).blocked = kAsyncBlocked;
        Action(C::Weapons, "weapon.giveall", "Give every listed weapon (async)", {"VI_GIVE_ITEM_TO_PED_ASYNC"}, [](Ctx& c, Feature&) {
            for (const auto& w : kWeapons)
                if (w.hash) GiveItem(c.ped, w.hash, 1);
        }).blocked = kAsyncBlocked;
        Choice(C::Weapons, "weapon.ammotype", "Ammo type", {}, kAmmoTypes, [](Ctx&, Feature&) {});
        // In game the native only raises the count and stops at the type's maximum: 0 -> 37 -> 60 took, 60 -> 10
        // did not, 250 gave 100. To lower, remove all ammo first.
        Int(C::Weapons, "weapon.setammo", "Raise ammo of that type to", {"SET_PED_AMMO_BY_TYPE"}, 250, 0, 9999, 50,
            [this](Ctx& c, Feature& f) {
                Feature* t = Find("weapon.ammotype");
                std::uint32_t h = t ? HashAt(kAmmoTypes, t->i) : 0;
                if (h) A::SET_PED_AMMO_BY_TYPE(c.ped, h, f.i, 0);
            })
            .checked = "2026-11-26: pistol ammo 0 -> 37 -> 60 by read-back; never lowers, capped at 100";
        Action(C::Weapons, "weapon.drop", "Drop current weapon", {"MAKE_PED_DROP_WEAPON"},
               [](Ctx& c, Feature&) { A::MAKE_PED_DROP_WEAPON(c.ped, true, 0, false, false); });
        Action(C::Weapons, "weapon.removeammo", "Remove all ammo", {"_REMOVE_ALL_PED_AMMO"},
               [](Ctx& c, Feature&) { A::_REMOVE_ALL_PED_AMMO(c.ped); })
            .checked = "2026-11-26: pistol 100 and a second type 12 -> none left by read-back";
        Toggle(C::Weapons, "weapon.hide", "Hide carried weapons", {"_HIDE_PED_WEAPONS"}, Hold::None,
               [](Ctx& c, Feature& f) {
                   if (f.on) A::_HIDE_PED_WEAPONS(c.ped, 2, true);
               });
        Toggle(C::Weapons, "weapon.keepdrop", "Keep weapons when dead", {"SET_PED_DROPS_WEAPONS_WHEN_DEAD"}, Hold::Interval,
               [](Ctx& c, Feature& f) { A::SET_PED_DROPS_WEAPONS_WHEN_DEAD(c.ped, 0, !f.on); });

        // ============================================================ VEHICLE
        Action(C::Vehicle, "veh.repair", "Repair",
               {"SET_VEHICLE_FIXED", "SET_VEHICLE_ENGINE_HEALTH", "SET_VEHICLE_BODY_HEALTH", "SET_VEHICLE_PETROL_TANK_HEALTH"},
               [](Ctx& c, Feature&) {
                   if (!c.inVehicle) return;
                   A::SET_VEHICLE_FIXED(c.veh);
                   // VI healths are 0..1, not GTA V's 0..1000: an untouched vehicle read 1 / 1 / 1 in game (run 25).
                   A::SET_VEHICLE_ENGINE_HEALTH(c.veh, 1.0f);
                   A::SET_VEHICLE_BODY_HEALTH(c.veh, 1.0f);
                   A::SET_VEHICLE_PETROL_TANK_HEALTH(c.veh, 1.0f);
               });
        Action(C::Vehicle, "veh.clean", "Wash", {"SET_VEHICLE_DIRT_LEVEL"}, [](Ctx& c, Feature&) {
            if (c.inVehicle) A::SET_VEHICLE_DIRT_LEVEL(c.veh, 0.0f);
        });
        Toggle(C::Vehicle, "veh.god", "Indestructible",
               {"SET_ENTITY_INVINCIBLE", "SET_VEHICLE_STRONG", "SET_VEHICLE_TYRES_CAN_BURST", "SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED"},
               Hold::Interval, [](Ctx& c, Feature& f) {
                   if (!c.inVehicle) return;
                   A::SET_ENTITY_INVINCIBLE(c.veh, f.on);
                   A::SET_VEHICLE_STRONG(c.veh, f.on);
                   A::SET_VEHICLE_TYRES_CAN_BURST(c.veh, !f.on);
                   A::SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED(c.veh, !f.on);
               });
        Toggle(C::Vehicle, "veh.autorepair", "Auto repair", {"SET_VEHICLE_FIXED", "GET_VEHICLE_ENGINE_HEALTH", "GET_VEHICLE_BODY_HEALTH"},
               Hold::Interval, [](Ctx& c, Feature& f) {
                   if (!f.on || !c.inVehicle) return;
                   if (A::GET_VEHICLE_ENGINE_HEALTH(c.veh) < 900.0f || A::GET_VEHICLE_BODY_HEALTH(c.veh) < 900.0f) A::SET_VEHICLE_FIXED(c.veh);
               });
        Toggle(C::Vehicle, "veh.engine", "Engine running", {"SET_VEHICLE_ENGINE_ON"}, Hold::None, [](Ctx& c, Feature& f) {
            if (c.inVehicle) A::SET_VEHICLE_ENGINE_ON(c.veh, f.on, true, false);
        });
        Float(C::Vehicle, "veh.boostspeed", "Boost speed (m/s)", {}, 40.0f, 5.0f, 150.0f, 5.0f, [](Ctx&, Feature&) {});
        Action(C::Vehicle, "veh.boost", "Boost", {"SET_VEHICLE_FORWARD_SPEED"}, [this](Ctx& c, Feature&) {
            Feature* s = Find("veh.boostspeed");
            if (c.inVehicle) A::SET_VEHICLE_FORWARD_SPEED(c.veh, s ? s->f : 40.0f, false);
        });
        Action(C::Vehicle, "veh.stop", "Stop dead", {"SET_VEHICLE_FORWARD_SPEED"}, [](Ctx& c, Feature&) {
            if (c.inVehicle) A::SET_VEHICLE_FORWARD_SPEED(c.veh, 0.0f, false);
        });
        Action(C::Vehicle, "veh.flip", "Set on wheels", {"SET_VEHICLE_ON_GROUND_PROPERLY"}, [](Ctx& c, Feature&) {
            if (c.inVehicle) A::SET_VEHICLE_ON_GROUND_PROPERLY(c.veh, true, 0, false);
        });
        ChoiceList(C::Vehicle, "veh.lights", "Lights", {"SET_VEHICLE_LIGHTS"}, {"Normal", "Off", "On", "On (3)"},
                   [](Ctx& c, Feature& f) {
                       if (c.inVehicle) A::SET_VEHICLE_LIGHTS(c.veh, f.i);
                   });
        ChoiceList(C::Vehicle, "veh.locks", "Door locks", {"SET_VEHICLE_DOORS_LOCKED"},
                   {"None", "Unlocked", "Locked", "Locked for player", "Locked (player inside)"}, [](Ctx& c, Feature& f) {
                       if (c.inVehicle) A::SET_VEHICLE_DOORS_LOCKED(c.veh, f.i);
                   });
        Action(C::Vehicle, "veh.opendoors", "Open all doors", {"SET_VEHICLE_DOOR_OPEN"}, [](Ctx& c, Feature&) {
            if (!c.inVehicle) return;
            for (int d = 0; d < 6; ++d) A::SET_VEHICLE_DOOR_OPEN(c.veh, d, false, true);
        });
        Action(C::Vehicle, "veh.closedoors", "Close all doors", {"SET_VEHICLE_DOORS_SHUT"}, [](Ctx& c, Feature&) {
            if (c.inVehicle) A::SET_VEHICLE_DOORS_SHUT(c.veh, true);
        });
        Toggle(C::Vehicle, "veh.seatbelt", "Never fall off or fly out", {"SET_PED_CAN_BE_KNOCKED_OFF_VEHICLE"}, Hold::Interval,
               [](Ctx& c, Feature& f) { A::SET_PED_CAN_BE_KNOCKED_OFF_VEHICLE(c.ped, f.on ? 1 : 0); });
        Toggle(C::Vehicle, "veh.nogravity", "No gravity", {"SET_VEHICLE_GRAVITY"}, Hold::Interval, [](Ctx& c, Feature& f) {
            if (c.inVehicle) A::SET_VEHICLE_GRAVITY(c.veh, !f.on);
        });
        Toggle(C::Vehicle, "veh.invisible", "Invisible vehicle", {"SET_ENTITY_VISIBLE"}, Hold::Interval, [](Ctx& c, Feature& f) {
            if (c.inVehicle) A::SET_ENTITY_VISIBLE(c.veh, !f.on);
        });
        Action(C::Vehicle, "veh.bursttyres", "Burst tyres", {"SET_VEHICLE_TYRE_BURST"}, [](Ctx& c, Feature&) {
            if (!c.inVehicle) return;
            for (int t = 0; t < 8; ++t) A::SET_VEHICLE_TYRE_BURST(c.veh, t, true, 1000.0f);
        });
        Action(C::Vehicle, "veh.warpnearest", "Enter nearest vehicle", {"SET_PED_INTO_VEHICLE", "GET_ENTITY_COORDS"},
               [](Ctx& c, Feature&) {
                   auto v = Nearby(c, SHVI_ENTITY_VEHICLE, 80.0f, true);
                   if (!v.empty()) A::SET_PED_INTO_VEHICLE(c.ped, v.front(), -1);
               });
        Action(C::Vehicle, "veh.explode", "Explode", {"EXPLODE_VEHICLE"}, [none](Ctx& c, Feature&) {
            if (c.inVehicle) A::EXPLODE_VEHICLE(c.veh, true, false, none);
        });
        Action(C::Vehicle, "veh.delete", "Delete", {"DELETE_VEHICLE", "SET_ENTITY_AS_MISSION_ENTITY"}, [](Ctx& c, Feature&) {
            if (!c.inVehicle) return;
            Ref v = c.veh;
            A::SET_ENTITY_AS_MISSION_ENTITY(v, true, true);
            A::DELETE_VEHICLE(shvi_p2u(&v));  // takes the address of the script's reference and clears it
        }).guess = true;  // the by-reference argument is assumed to be a pointer to the 16-byte reference

        Toggle(C::Vehicle, "veh.handbrake", "Handbrake", {"SET_VEHICLE_HANDBRAKE"}, Hold::Interval, [](Ctx& c, Feature& f) {
            if (c.inVehicle) A::SET_VEHICLE_HANDBRAKE(c.veh, f.on);
        });
        Toggle(C::Vehicle, "veh.interiorlight", "Interior light", {"SET_VEHICLE_INTERIORLIGHT"}, Hold::None, [](Ctx& c, Feature& f) {
            if (c.inVehicle) A::SET_VEHICLE_INTERIORLIGHT(c.veh, f.on);
        });
        Toggle(C::Vehicle, "veh.fullbeam", "Full beam", {"SET_VEHICLE_FULLBEAM"}, Hold::None, [](Ctx& c, Feature& f) {
            if (c.inVehicle) A::SET_VEHICLE_FULLBEAM(c.veh, f.on);
        });
        Toggle(C::Vehicle, "veh.hazards", "Hazard lights", {"SET_VEHICLE_INDICATOR_LIGHTS"}, Hold::None, [](Ctx& c, Feature& f) {
            if (!c.inVehicle) return;
            A::SET_VEHICLE_INDICATOR_LIGHTS(c.veh, false, f.on);
            A::SET_VEHICLE_INDICATOR_LIGHTS(c.veh, true, f.on);
        });
        Toggle(C::Vehicle, "veh.alarm", "Alarm", {"SET_VEHICLE_ALARM"}, Hold::None, [](Ctx& c, Feature& f) {
            if (c.inVehicle) A::SET_VEHICLE_ALARM(c.veh, f.on);
        });
        Toggle(C::Vehicle, "veh.burnout", "Burnout", {"SET_VEHICLE_BURNOUT"}, Hold::Interval, [](Ctx& c, Feature& f) {
            if (c.inVehicle) A::SET_VEHICLE_BURNOUT(c.veh, f.on);
        });
        Action(C::Vehicle, "veh.fixtyres", "Fix tyres", {"SET_VEHICLE_TYRE_FIXED"}, [](Ctx& c, Feature&) {
            if (!c.inVehicle) return;
            for (int t = 0; t < 8; ++t) A::SET_VEHICLE_TYRE_FIXED(c.veh, t);
        });
        Action(C::Vehicle, "veh.breakdoors", "Break off doors", {"SET_VEHICLE_DOOR_BROKEN"}, [](Ctx& c, Feature&) {
            if (!c.inVehicle) return;
            for (int d = 0; d < 6; ++d) A::SET_VEHICLE_DOOR_BROKEN(c.veh, d, false);
        });
        Float(C::Vehicle, "veh.power", "Engine power boost", {"SET_VEHICLE_CHEAT_POWER_INCREASE"}, 1.0f, 0.5f, 10.0f, 0.5f,
              [](Ctx& c, Feature& f) {
                  if (c.inVehicle) A::SET_VEHICLE_CHEAT_POWER_INCREASE(c.veh, f.f);
              });
        Float(C::Vehicle, "veh.maxspeed", "Top speed limit (m/s)", {"SET_VEHICLE_MAX_SPEED"}, 80.0f, 5.0f, 300.0f, 5.0f,
              [](Ctx& c, Feature& f) {
                  if (c.inVehicle) A::SET_VEHICLE_MAX_SPEED(c.veh, f.f);
              });
        Float(C::Vehicle, "veh.steerbias", "Steering bias", {"SET_VEHICLE_STEER_BIAS"}, 0.0f, -1.0f, 1.0f, 0.1f,
              [](Ctx& c, Feature& f) {
                  if (c.inVehicle) A::SET_VEHICLE_STEER_BIAS(c.veh, f.f);
              });
        Action(C::Vehicle, "veh.own", "Mark as owned, not stolen", {"SET_VEHICLE_HAS_BEEN_OWNED_BY_PLAYER", "SET_VEHICLE_IS_STOLEN"},
               [](Ctx& c, Feature&) {
                   if (!c.inVehicle) return;
                   A::SET_VEHICLE_HAS_BEEN_OWNED_BY_PLAYER(c.veh, true);
                   A::SET_VEHICLE_IS_STOLEN(c.veh, false);
               });
        Action(C::Vehicle, "veh.launch", "Launch upwards", {"SET_ENTITY_VELOCITY"}, [](Ctx& c, Feature&) {
            if (c.inVehicle) A::SET_ENTITY_VELOCITY(c.veh, V(0, 0, 25.0f));
        });
        Action(C::Vehicle, "veh.extinguish", "Put out fire on vehicle", {"STOP_ENTITY_FIRE"}, [](Ctx& c, Feature&) {
            if (c.inVehicle) A::STOP_ENTITY_FIRE(c.veh, false, true);
        });

        // ============================================================ SPAWN
        // Creation is asynchronous in GTA VI (docs/NATIVEDB.md, section 7): the natives return a task, and
        // the entity shows up in its pool some frames later. Nothing here reads the task.
        Feature& model = ChoiceList(C::Spawn, "spawn.model", "Model (shvi\\trainer_models.txt)", {}, {"(list is empty)"}, [](Ctx&, Feature&) {});
        (void)model;
        Float(C::Spawn, "spawn.distance", "Distance ahead (m)", {}, 6.0f, 1.0f, 50.0f, 1.0f, [](Ctx&, Feature&) {});
        const char* vehicleExperiment = std::getenv("SHVI_EXPERIMENTAL_VEHICLE");
        const bool experimentalVehicle = vehicleExperiment && !std::strcmp(vehicleExperiment, "1");
        { Feature& vf = Action(C::Spawn, "spawn.vehicle", "Spawn vehicle: listed model", {"VI_CREATE_VEHICLE_ASYNC", "GET_ENTITY_COORDS", "GET_ENTITY_FORWARD_VECTOR", "GET_ENTITY_HEADING"},
               [this](Ctx& c, Feature&) { SpawnVehicle(c, SelectedModel()); });
          vf.blocked = experimentalVehicle ? nullptr : kAsyncBlocked; vf.scriptCall = true; vf.guess = true; }
        { Feature& vf = Action(C::Spawn, "spawn.vehiclecopy", "Spawn vehicle: same model as mine or nearest",
               {"VI_CREATE_VEHICLE_ASYNC", "GET_ENTITY_MODEL", "GET_ENTITY_COORDS", "GET_ENTITY_FORWARD_VECTOR", "GET_ENTITY_HEADING"},
               [this](Ctx& c, Feature&) {
                   Ref src = c.veh;
                   if (!c.inVehicle) {
                       auto v = Nearby(c, SHVI_ENTITY_VEHICLE, 100.0f, true);
                       if (!v.empty()) src = v.front();
                   }
                   if (Valid(src)) SpawnVehicle(c, ModelOf(src));
                   else Log("trainer: no vehicle to copy");
               });
          vf.blocked = experimentalVehicle ? nullptr : kAsyncBlocked; vf.scriptCall = true; vf.guess = true; }
        Action(C::Spawn, "spawn.collectvehicles", "Add nearby vehicle models to the list", {"GET_ENTITY_MODEL", "GET_ENTITY_COORDS"},
               [this](Ctx& c, Feature&) { CollectVehicleModels(c); });
        { Feature& sf = Action(C::Spawn, "spawn.ped", "Spawn ped: listed model", {"GET_ENTITY_COORDS", "GET_ENTITY_FORWARD_VECTOR", "GET_ENTITY_HEADING"},
               [this](Ctx& c, Feature&) { SpawnPed(c, SelectedModel()); }); sf.scriptCall = true; sf.checked = kSpawnChecked; }
        { Feature& sf = Action(C::Spawn, "spawn.pedcopy", "Spawn ped: same model as nearest",
               {"GET_ENTITY_MODEL", "GET_ENTITY_COORDS", "GET_ENTITY_FORWARD_VECTOR", "GET_ENTITY_HEADING"},
               [this](Ctx& c, Feature&) {
                   auto v = Nearby(c, SHVI_ENTITY_PED, 100.0f, true);
                   if (!v.empty()) SpawnPed(c, ModelOf(v.front()));
                   else Log("trainer: no ped to copy");
               }); sf.scriptCall = true; sf.checked = kSpawnChecked; }
        // Story characters ("persistent characters") have synchronous natives: no Task, so none of the async
        // spawn problems. Flow as in shipped scripts: handle by character hash, force the spawn, take the ped.
        // The names are the guide's (RDR2-style) and unverified: experimental through their confidence.
        Choice(C::Spawn, "spawn.character", "Story character", {"_CREATE_PERSISTENT_CHARACTER"}, kCharacters, [](Ctx&, Feature&) {});
        Action(C::Spawn, "spawn.charspawn", "Spawn that story character here",
               {"_CREATE_PERSISTENT_CHARACTER", "_FORCE_SPAWN_PERSCHAR", "_GET_PERSCHAR_PED_INDEX", "_IS_PERSISTENT_CHARACTER_VALID",
                "SET_ENTITY_COORDS", "GET_ENTITY_COORDS"},
               [this](Ctx& c, Feature&) {
                   std::uint32_t h = HashAt(kCharacters, Find("spawn.character")->i);
                   if (!h) { Log("trainer: no character selected"); return; }
                   char b[220];
                   Ref pc = A::_CREATE_PERSISTENT_CHARACTER(h);
                   bool valid = Valid(pc) && A::_IS_PERSISTENT_CHARACTER_VALID(pc);
                   std::snprintf(b, sizeof(b), "trainer: character 0x%08X -> handle %llX:%llX, valid %d", h, (unsigned long long)pc.lo,
                                 (unsigned long long)pc.hi, valid ? 1 : 0);
                   Log(b);
                   if (!valid) return;
                   Ref before = A::_GET_PERSCHAR_PED_INDEX(pc);
                   A::_FORCE_SPAWN_PERSCHAR(pc);
                   Ref ped = A::_GET_PERSCHAR_PED_INDEX(pc);
                   std::snprintf(b, sizeof(b), "trainer: character ped before the spawn %llX:%llX, after %llX:%llX", (unsigned long long)before.lo,
                                 (unsigned long long)before.hi, (unsigned long long)ped.lo, (unsigned long long)ped.hi);
                   Log(b);
                   lastCharacter_ = pc;
                   if (!Valid(ped)) return;
                   A::SET_ENTITY_COORDS(ped, Ahead(c), false, false, false, false, -1.0f);
                   lastSpawned_ = ped;
               });
        // A handle comes back only for characters whose definition is registered at that point of the game
        // (0x144FCD1C0); the others return zero. This lists which ones are, and whether they have a ped.
        Action(C::Spawn, "spawn.charlist", "Log which story characters are available",
               {"_CREATE_PERSISTENT_CHARACTER", "_GET_PERSCHAR_PED_INDEX", "_IS_PERSISTENT_CHARACTER_VALID"}, [this](Ctx&, Feature&) {
                   int ok = 0, n = 0;
                   for (const HashName& hn : kCharacters) {
                       ++n;
                       Ref pc = A::_CREATE_PERSISTENT_CHARACTER(hn.hash);
                       if (!Valid(pc)) continue;
                       ++ok;
                       Ref ped = A::_GET_PERSCHAR_PED_INDEX(pc);
                       char b[200];
                       std::snprintf(b, sizeof(b), "trainer: character %s (0x%08X): handle %llX:%llX valid %d, ped %s", hn.name, hn.hash,
                                     (unsigned long long)pc.lo, (unsigned long long)pc.hi, A::_IS_PERSISTENT_CHARACTER_VALID(pc) ? 1 : 0,
                                     Valid(ped) ? "exists" : "none");
                       Log(b);
                   }
                   char b[96];
                   std::snprintf(b, sizeof(b), "trainer: %d of %d story characters have a handle here", ok, n);
                   Log(b);
               });
        Action(C::Spawn, "spawn.charinfo", "Log the spawned character", {"_GET_PERSCHAR_PED_INDEX", "GET_ENTITY_COORDS", "GET_ENTITY_MODEL"},
               [this](Ctx& c, Feature&) {
                   char b[220];
                   Ref ped = Valid(lastCharacter_) ? A::_GET_PERSCHAR_PED_INDEX(lastCharacter_) : Ref{};
                   if (!Valid(ped)) { Log("trainer: character ped: none"); return; }
                   Vec3 q = A::GET_ENTITY_COORDS(ped, false), me = A::GET_ENTITY_COORDS(c.ped, false);
                   std::snprintf(b, sizeof(b), "trainer: character ped %llX:%llX model 0x%08X at %.1f %.1f %.1f, %.1f m from the player",
                                 (unsigned long long)ped.lo, (unsigned long long)ped.hi, A::GET_ENTITY_MODEL(ped), q.x, q.y, q.z,
                                 std::sqrt(Dist2(q, me)));
                   Log(b);
               });
        Action(C::Spawn, "spawn.charbring", "Bring the spawned character here", {"_GET_PERSCHAR_PED_INDEX", "SET_ENTITY_COORDS"},
               [this](Ctx& c, Feature&) {
                   Ref ped = Valid(lastCharacter_) ? A::_GET_PERSCHAR_PED_INDEX(lastCharacter_) : Ref{};
                   if (Valid(ped)) A::SET_ENTITY_COORDS(ped, Ahead(c), false, false, false, false, -1.0f);
               });
        Action(C::Spawn, "spawn.chardespawn", "Despawn the spawned character", {"_FORCE_DESPAWN_PERSCHAR"}, [this](Ctx&, Feature&) {
            if (Valid(lastCharacter_)) A::_FORCE_DESPAWN_PERSCHAR(lastCharacter_);
        });
        { Feature& sf = Action(C::Spawn, "spawn.clone", "Spawn ped: same model as player", {"GET_ENTITY_MODEL", "GET_ENTITY_COORDS", "GET_ENTITY_FORWARD_VECTOR", "GET_ENTITY_HEADING"},
               [this](Ctx& c, Feature&) { SpawnPed(c, ModelOf(c.ped)); }); sf.scriptCall = true; sf.checked = kSpawnChecked; }
        { Feature& sf = Action(C::Spawn, "spawn.object", "Spawn object: listed model", {"GET_ENTITY_COORDS", "GET_ENTITY_FORWARD_VECTOR"},
               [this](Ctx& c, Feature&) { SpawnObject(c, SelectedModel()); }); sf.scriptCall = true; sf.checked = kSpawnChecked; }
        { Feature& sf = Action(C::Spawn, "spawn.objectcopy", "Spawn object: same model as nearest", {"GET_ENTITY_MODEL", "GET_ENTITY_COORDS", "GET_ENTITY_FORWARD_VECTOR"},
               [this](Ctx& c, Feature&) {
                   auto v = Nearby(c, SHVI_ENTITY_OBJECT, 60.0f, true);
                   if (!v.empty()) SpawnObject(c, ModelOf(v.front()));
                   else Log("trainer: no object to copy");
               }); sf.scriptCall = true; sf.checked = kSpawnChecked; }
        Action(C::Spawn, "spawn.deletepeds", "Delete nearby peds", {"DELETE_PED", "SET_ENTITY_AS_MISSION_ENTITY", "GET_ENTITY_COORDS"}, [this](Ctx& c, Feature&) {
            for (Ref p : Nearby(c, SHVI_ENTITY_PED, Find("world.radius")->f, true)) {
                A::SET_ENTITY_AS_MISSION_ENTITY(p, true, true);
                A::DELETE_PED(shvi_p2u(&p));
            }
        }).guess = true;
        Action(C::Spawn, "spawn.deletevehicles", "Delete nearby vehicles", {"DELETE_VEHICLE", "SET_ENTITY_AS_MISSION_ENTITY", "GET_ENTITY_COORDS"}, [this](Ctx& c, Feature&) {
            for (Ref v : Nearby(c, SHVI_ENTITY_VEHICLE, Find("world.radius")->f, true)) {
                A::SET_ENTITY_AS_MISSION_ENTITY(v, true, true);
                A::DELETE_VEHICLE(shvi_p2u(&v));
            }
        }).guess = true;

        // ============================================================ TELEPORT
        Action(C::Teleport, "tp.waypoint", "To map waypoint", {"IS_WAYPOINT_ACTIVE", "_GET_WAYPOINT_COORDS", "SET_ENTITY_COORDS", "GET_ENTITY_COORDS"},
               [](Ctx& c, Feature&) {
                   if (!A::IS_WAYPOINT_ACTIVE()) { Log("trainer: no waypoint set"); return; }
                   Vec3 w = A::_GET_WAYPOINT_COORDS();
                   Vec3 me = A::GET_ENTITY_COORDS(c.Target(), true);
                   MoveTo(c, V(w.x, w.y, w.z != 0.0f ? w.z : me.z));
               });
        Action(C::Teleport, "tp.clearwaypoint", "Remove map waypoint", {"SET_WAYPOINT_OFF"}, [](Ctx&, Feature&) { A::SET_WAYPOINT_OFF(); });
        Float(C::Teleport, "tp.step", "Step (m)", {}, 5.0f, 1.0f, 500.0f, 1.0f, [](Ctx&, Feature&) {});
        Action(C::Teleport, "tp.forward", "Step forward", {"GET_ENTITY_FORWARD_VECTOR", "GET_ENTITY_COORDS", "SET_ENTITY_COORDS"},
               [this](Ctx& c, Feature&) {
                   float d = Find("tp.step")->f;
                   MoveTo(c, Add(A::GET_ENTITY_COORDS(c.Target(), true), Mul(A::GET_ENTITY_FORWARD_VECTOR(c.Target()), d)));
               });
        Action(C::Teleport, "tp.up", "Step up", {"GET_ENTITY_COORDS", "SET_ENTITY_COORDS"}, [this](Ctx& c, Feature&) {
            MoveTo(c, Add(A::GET_ENTITY_COORDS(c.Target(), true), V(0, 0, Find("tp.step")->f)));
        });
        Action(C::Teleport, "tp.down", "Step down", {"GET_ENTITY_COORDS", "SET_ENTITY_COORDS"}, [this](Ctx& c, Feature&) {
            MoveTo(c, Add(A::GET_ENTITY_COORDS(c.Target(), true), V(0, 0, -Find("tp.step")->f)));
        });
        Int(C::Teleport, "tp.slot", "Slot", {}, 1, 1, kSlots, 1, [](Ctx&, Feature&) {});
        Action(C::Teleport, "tp.save", "Save position to slot", {"GET_ENTITY_COORDS", "GET_ENTITY_HEADING"}, [this](Ctx& c, Feature&) {
            Slot& s = slots_[Find("tp.slot")->i - 1];
            s.pos = A::GET_ENTITY_COORDS(c.Target(), true);
            s.heading = A::GET_ENTITY_HEADING(c.Target());
            s.used = true;
        });
        Action(C::Teleport, "tp.load", "Go to slot", {"SET_ENTITY_COORDS", "SET_ENTITY_HEADING"}, [this](Ctx& c, Feature&) {
            const Slot& s = slots_[Find("tp.slot")->i - 1];
            if (!s.used) { Log("trainer: slot is empty"); return; }
            MoveTo(c, s.pos);
            A::SET_ENTITY_HEADING(c.Target(), s.heading);
        });
        Float(C::Teleport, "tp.x", "X", {}, 0.0f, -20000.0f, 20000.0f, 10.0f, [](Ctx&, Feature&) {});
        Float(C::Teleport, "tp.y", "Y", {}, 0.0f, -20000.0f, 20000.0f, 10.0f, [](Ctx&, Feature&) {});
        Float(C::Teleport, "tp.z", "Z", {}, 50.0f, -500.0f, 5000.0f, 5.0f, [](Ctx&, Feature&) {});
        Action(C::Teleport, "tp.go", "Go to X Y Z", {"SET_ENTITY_COORDS"}, [this](Ctx& c, Feature&) {
            MoveTo(c, V(Find("tp.x")->f, Find("tp.y")->f, Find("tp.z")->f));
        });
        Action(C::Teleport, "tp.fill", "Copy current position to X Y Z", {"GET_ENTITY_COORDS"}, [this](Ctx& c, Feature&) {
            Vec3 p = A::GET_ENTITY_COORDS(c.Target(), true);
            Find("tp.x")->f = p.x; Find("tp.y")->f = p.y; Find("tp.z")->f = p.z;
        });
        Action(C::Teleport, "tp.streamed", "Go to X Y Z (engine teleport, loads the area)", {"START_PLAYER_TELEPORT", "GET_PLAYER_INDEX", "GET_ENTITY_HEADING"},
               [this](Ctx& c, Feature&) {
                   A::START_PLAYER_TELEPORT(A::GET_PLAYER_INDEX(), V(Find("tp.x")->f, Find("tp.y")->f, Find("tp.z")->f),
                                            A::GET_ENTITY_HEADING(c.ped), false, false, false, false, false, false);
               }).guess = true;  // six flags, their meaning is not known; all false
        Action(C::Teleport, "tp.streamedstop", "Cancel engine teleport", {"STOP_PLAYER_TELEPORT"}, [](Ctx&, Feature&) { A::STOP_PLAYER_TELEPORT(); });
        Float(C::Teleport, "tp.heading", "Heading (degrees)", {"SET_ENTITY_HEADING"}, 0.0f, 0.0f, 359.0f, 15.0f,
              [](Ctx& c, Feature& f) { A::SET_ENTITY_HEADING(c.Target(), f.f * kDegToRad); });  // radians on both sides: the getter is atan2 folded into [0, 2pi) (0x142084FA0), the setter wraps with pi and 2pi (0x143C27550)
        Toggle(C::Teleport, "tp.noclip", "No-clip (walk direction, frozen)", {"FREEZE_ENTITY_POSITION", "SET_ENTITY_COORDS", "GET_ENTITY_COORDS", "GET_GAMEPLAY_CAM_ROT"},
               Hold::Frame, [this](Ctx& c, Feature& f) {
                   Ref t = c.Target();
                   A::FREEZE_ENTITY_POSITION(t, f.on);
                   if (!f.on) return;
                   // glide along the camera direction at `tp.step` metres per second
                   Vec3 rot = A::GET_GAMEPLAY_CAM_ROT(2);
                   float yaw = rot.z * 0.0174532925f, pitch = rot.x * 0.0174532925f;
                   Vec3 dir = V(-std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch));
                   MoveTo(c, Add(A::GET_ENTITY_COORDS(t, true), Mul(dir, Find("tp.step")->f * c.dt * noclipInput_)));
               })
            .guess = true;  // camera rotation order 2 and the Euler convention are GTA V's

        // ============================================================ TIME
        Int(C::Time, "time.hour", "Hour", {"SET_CLOCK_TIME", "GET_CLOCK_MINUTES"}, 12, 0, 23, 1,
            [](Ctx&, Feature& f) { A::SET_CLOCK_TIME(f.i, A::GET_CLOCK_MINUTES(), 0, false); });
        Int(C::Time, "time.minute", "Minute", {"SET_CLOCK_TIME", "GET_CLOCK_HOURS"}, 0, 0, 59, 5,
            [](Ctx&, Feature& f) { A::SET_CLOCK_TIME(A::GET_CLOCK_HOURS(), f.i, 0, false); });
        Action(C::Time, "time.plus1", "Forward one hour", {"ADD_TO_CLOCK_TIME"}, [](Ctx&, Feature&) { A::ADD_TO_CLOCK_TIME(1, 0, 0); });
        Action(C::Time, "time.minus1", "Back one hour", {"SET_CLOCK_TIME", "GET_CLOCK_HOURS", "GET_CLOCK_MINUTES"}, [](Ctx&, Feature&) {
            A::SET_CLOCK_TIME((A::GET_CLOCK_HOURS() + 23) % 24, A::GET_CLOCK_MINUTES(), 0, false);
        });
        ChoiceList(C::Time, "time.preset", "Preset", {"SET_CLOCK_TIME"}, {"Dawn 06:00", "Morning 09:00", "Noon 12:00", "Afternoon 16:00", "Dusk 19:30", "Midnight 00:00"},
                   [](Ctx&, Feature& f) {
                       static const int hm[][2] = {{6, 0}, {9, 0}, {12, 0}, {16, 0}, {19, 30}, {0, 0}};
                       A::SET_CLOCK_TIME(hm[f.i][0], hm[f.i][1], 0, false);
                   });
        Toggle(C::Time, "time.pause", "Freeze clock", {"PAUSE_CLOCK"}, Hold::Interval,
               [](Ctx&, Feature& f) { A::PAUSE_CLOCK(f.on, false); });
        Float(C::Time, "time.scale", "Game speed", {"SET_TIME_SCALE"}, 1.0f, 0.05f, 1.0f, 0.05f,
              [](Ctx&, Feature& f) { A::SET_TIME_SCALE(f.f); });
        Toggle(C::Time, "time.holdscale", "Hold game speed", {"SET_TIME_SCALE"}, Hold::Frame, [this](Ctx&, Feature& f) {
            A::SET_TIME_SCALE(f.on ? Find("time.scale")->f : 1.0f);
        });
        Toggle(C::Time, "time.sync", "Follow system clock", {"SET_CLOCK_TIME"}, Hold::Interval, [](Ctx&, Feature& f) {
            if (!f.on) return;
            std::time_t t = std::time(nullptr);
            std::tm lt{};
#if defined(_WIN32)
            localtime_s(&lt, &t);
#else
            localtime_r(&t, &lt);
#endif
            A::SET_CLOCK_TIME(lt.tm_hour, lt.tm_min, lt.tm_sec, false);
        });

        // ============================================================ WEATHER
        Choice(C::Weather, "weather.type", "Weather", {"SET_WEATHER_TYPE"}, kWeatherTypes, [](Ctx&, Feature& f) {
            // (hash, bool, bool, bool, transition seconds, bool, bool). (h, 1, 1, 0, 0, 0, 1) is the form scripts use most
            // (34 of 55 literal call sites); the earlier (1, 0, 1, 0, 0, 0) had no readable effect in game (run 33).
            std::uint32_t h = HashAt(kWeatherTypes, f.i);
            if (h) A::SET_WEATHER_TYPE(h, true, true, false, 0.0f, false, true);
        }).checked = "2026-11-26: each type gives its own weather read-back (SUNNY 0x57F53C65, OVERCAST 0x5EBC0DF0, FOG 0x9033FF87)";
        // The getter reports a different hash than the one set (above): it names the weather the game resolves to.
        Toggle(C::Weather, "weather.hold", "Hold selected weather", {"SET_WEATHER_TYPE"}, Hold::Interval, [this](Ctx&, Feature& f) {
            std::uint32_t h = HashAt(kWeatherTypes, Find("weather.type")->i);
            if (f.on && h) A::SET_WEATHER_TYPE(h, true, true, false, 0.0f, false, true);
        }).checked = "2026-11-26: same call as Weather, re-applied";
        Action(C::Weather, "weather.random", "Random weather", {"SET_RANDOM_WEATHER_TYPE"}, [](Ctx&, Feature&) { A::SET_RANDOM_WEATHER_TYPE(false, false); });
        Action(C::Weather, "weather.clear", "Clear weather override", {"CLEAR_OVERRIDE_WEATHER", "CLEAR_WEATHER_TYPE_PERSIST"}, [](Ctx&, Feature&) {
            A::CLEAR_OVERRIDE_WEATHER();
            A::CLEAR_WEATHER_TYPE_PERSIST();
        });
        Float(C::Weather, "weather.rain", "Rain level", {"SET_RAIN"}, 0.0f, 0.0f, 1.0f, 0.1f, [](Ctx&, Feature& f) { A::SET_RAIN(f.f); }).guess = true;  // no readable effect in game: GET_RAIN_LEVEL stayed 0 for 36 s (runs 33, 52)
        Float(C::Weather, "weather.wind", "Wind speed", {"SET_WIND_SPEED"}, 0.0f, 0.0f, 12.0f, 0.5f, [](Ctx&, Feature& f) { A::SET_WIND_SPEED(f.f); }).guess = true;  // no readable effect in game: GET_WIND_SPEED kept its own slow drift (runs 33, 52)
        Float(C::Weather, "weather.winddir", "Wind direction", {"SET_WIND_DIRECTION"}, 0.0f, 0.0f, 6.28f, 0.2f, [](Ctx&, Feature& f) { A::SET_WIND_DIRECTION(f.f); });

        // ============================================================ WORLD
        Float(C::World, "world.traffic", "Traffic density", {}, 1.0f, 0.0f, 3.0f, 0.1f, [](Ctx&, Feature&) {});
        Toggle(C::World, "world.holdtraffic", "Apply traffic density",
               {"SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME", "SET_RANDOM_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME", "SET_PARKED_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME"},
               Hold::Frame, [this](Ctx&, Feature& f) {
                   if (!f.on) return;
                   float d = Find("world.traffic")->f;
                   A::SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(d);
                   A::SET_RANDOM_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(d);
                   A::SET_PARKED_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(d);
               });
        Float(C::World, "world.peds", "Pedestrian density", {}, 1.0f, 0.0f, 3.0f, 0.1f, [](Ctx&, Feature&) {});
        Toggle(C::World, "world.holdpeds", "Apply pedestrian density",
               {"SET_PED_DENSITY_MULTIPLIER_THIS_FRAME", "SET_SCENARIO_PED_DENSITY_MULTIPLIER_THIS_FRAME"}, Hold::Frame,
               [this](Ctx&, Feature& f) {
                   if (!f.on) return;
                   float d = Find("world.peds")->f;
                   A::SET_PED_DENSITY_MULTIPLIER_THIS_FRAME(d);
                   A::SET_SCENARIO_PED_DENSITY_MULTIPLIER_THIS_FRAME(d);
               });
        Toggle(C::World, "world.flee", "Everyone flees from player", {"SET_ALL_RANDOM_PEDS_FLEE_THIS_FRAME"}, Hold::Frame,
               [](Ctx& c, Feature& f) {
                   if (f.on) A::SET_ALL_RANDOM_PEDS_FLEE_THIS_FRAME(c.ped);
               });
        Toggle(C::World, "world.holdambient", "Apply pedestrian density to ambient peds too", {"_SET_AMBIENT_PED_DENSITY_MULTIPLIER_THIS_FRAME"}, Hold::Frame,
               [this](Ctx&, Feature& f) {
                   if (f.on) A::_SET_AMBIENT_PED_DENSITY_MULTIPLIER_THIS_FRAME(Find("world.peds")->f);
               });
        Float(C::World, "world.radius", "Radius (m)", {}, 30.0f, 5.0f, 300.0f, 5.0f, [](Ctx&, Feature&) {});
        Action(C::World, "world.cleararea", "Clear area", {"CLEAR_AREA", "GET_ENTITY_COORDS"}, [this](Ctx& c, Feature&) {
            // The last argument is a bit mask of what to clear (0x145A9A300 hands it to the query): 0 clears nothing
            // (runs 33, 52). 0x0B5C0583 is the mask shipped scripts pass most often (44 of 150 literal call sites).
            A::CLEAR_AREA(A::GET_ENTITY_COORDS(c.ped, true), Find("world.radius")->f, 0x0B5C0583);
        });  // confirmed in game with this mask (run 54: 4 vehicles within 80 m -> 0); what each bit clears is not known
        Action(C::World, "world.killpeds", "Kill nearby pedestrians", {"SET_ENTITY_HEALTH", "GET_ENTITY_COORDS"}, [this, none](Ctx& c, Feature&) {
            for (const Ref& p : Nearby(c, SHVI_ENTITY_PED, Find("world.radius")->f, true)) A::SET_ENTITY_HEALTH(p, 0, none, false, false);
        });
        // Deleting takes the address of a 16-byte reference and clears it (0x144AB70F0). The player's own
        // vehicle was not removed in run 55; these work on the entities around the player instead.
        Action(C::World, "world.deletevehicles", "Delete nearby vehicles", {"DELETE_VEHICLE", "SET_ENTITY_AS_MISSION_ENTITY", "GET_ENTITY_COORDS"},
               [this](Ctx& c, Feature&) {
                   int n = 0;
                   for (Ref v : Nearby(c, SHVI_ENTITY_VEHICLE, Find("world.radius")->f, true)) {
                       A::SET_ENTITY_AS_MISSION_ENTITY(v, true, true);
                       A::DELETE_VEHICLE(shvi_p2u(&v));
                       ++n;
                   }
                   char b[80];
                   std::snprintf(b, sizeof(b), "trainer: asked to delete %d vehicles", n);
                   Log(b);
               }).guess = true;  // no effect in game (run 56: 4 asked, 4 still there); the engine's delete may want a script that owns the entity
        { Feature& df = Action(C::World, "world.deletepeds", "Delete nearby pedestrians", {"DELETE_PED", "SET_ENTITY_AS_MISSION_ENTITY", "GET_ENTITY_COORDS"},
               [this](Ctx& c, Feature&) {
                   int n = 0;
                   for (Ref p : Nearby(c, SHVI_ENTITY_PED, Find("world.radius")->f, true)) {
                       A::SET_ENTITY_AS_MISSION_ENTITY(p, true, true);
                       A::DELETE_PED(shvi_p2u(&p));
                       ++n;
                   }
                   char b[80];
                   std::snprintf(b, sizeof(b), "trainer: asked to delete %d pedestrians", n);
                   Log(b);
               });
          df.blocked = "faulted 56 times on 92 pedestrians and then crashed the game (run 56)"; }
        Action(C::World, "world.freezepeds", "Stop nearby pedestrians", {"CLEAR_PED_TASKS_IMMEDIATELY", "GET_ENTITY_COORDS"}, [this](Ctx& c, Feature&) {
            for (const Ref& p : Nearby(c, SHVI_ENTITY_PED, Find("world.radius")->f, true)) A::CLEAR_PED_TASKS_IMMEDIATELY(p, false, false);
        });
        Action(C::World, "world.explodevehicles", "Explode nearby vehicles", {"EXPLODE_VEHICLE", "GET_ENTITY_COORDS"}, [this, none](Ctx& c, Feature&) {
            for (const Ref& v : Nearby(c, SHVI_ENTITY_VEHICLE, Find("world.radius")->f, true)) A::EXPLODE_VEHICLE(v, true, false, none);
        });
        Action(C::World, "world.repairvehicles", "Repair nearby vehicles", {"SET_VEHICLE_FIXED", "GET_ENTITY_COORDS"}, [this](Ctx& c, Feature&) {
            for (const Ref& v : Nearby(c, SHVI_ENTITY_VEHICLE, Find("world.radius")->f, false)) A::SET_VEHICLE_FIXED(v);
        });
        Action(C::World, "world.launchvehicles", "Launch nearby vehicles", {"SET_ENTITY_VELOCITY", "GET_ENTITY_COORDS"}, [this](Ctx& c, Feature&) {
            for (const Ref& v : Nearby(c, SHVI_ENTITY_VEHICLE, Find("world.radius")->f, true)) A::SET_ENTITY_VELOCITY(v, V(0, 0, 25.0f));
        });
        Int(C::World, "world.explosiontype", "Explosion type", {}, 0, 0, 80, 1, [](Ctx&, Feature&) {});
        Action(C::World, "world.explosion", "Explosion ahead", {"ADD_EXPLOSION", "GET_ENTITY_COORDS", "GET_ENTITY_FORWARD_VECTOR"},
               [this](Ctx& c, Feature&) {
                   Vec3 p = Add(A::GET_ENTITY_COORDS(c.ped, true), Mul(A::GET_ENTITY_FORWARD_VECTOR(c.ped), 12.0f));
                   A::ADD_EXPLOSION(p, Find("world.explosiontype")->i, 1.0f, true, false, 1.0f);
               });
        ChoiceList(C::World, "world.gravity", "Gravity level", {"SET_GRAVITY_LEVEL"}, {"Normal", "Low", "Very low", "None"},
                   [](Ctx&, Feature& f) { A::SET_GRAVITY_LEVEL(f.i); });
        Toggle(C::World, "world.pause", "Pause game", {"SET_GAME_PAUSED"}, Hold::None, [](Ctx&, Feature& f) { A::SET_GAME_PAUSED(f.on); });

        Action(C::World, "world.stopfires", "Put out fires nearby", {"STOP_FIRE_IN_RANGE", "GET_ENTITY_COORDS"}, [this](Ctx& c, Feature&) {
            A::STOP_FIRE_IN_RANGE(A::GET_ENTITY_COORDS(c.ped, true), Find("world.radius")->f);
        });
        Toggle(C::World, "world.nocops", "No random police", {"SET_CREATE_RANDOM_COPS"}, Hold::Interval,
               [](Ctx&, Feature& f) { A::SET_CREATE_RANDOM_COPS(!f.on); });
        Toggle(C::World, "world.notrains", "No random trains", {"SET_RANDOM_TRAINS"}, Hold::Interval,
               [](Ctx&, Feature& f) { A::SET_RANDOM_TRAINS(!f.on); });
        Toggle(C::World, "world.noboats", "No random boats", {"SET_RANDOM_BOATS"}, Hold::Interval,
               [](Ctx&, Feature& f) { A::SET_RANDOM_BOATS(!f.on); });

        // ============================================================ MISC
        Float(C::Misc, "misc.camshake", "Camera shake", {"SET_GAMEPLAY_CAM_SHAKE_AMPLITUDE"}, 0.0f, 0.0f, 5.0f, 0.25f,
              [](Ctx&, Feature& f) { A::SET_GAMEPLAY_CAM_SHAKE_AMPLITUDE(f.f); });
        Action(C::Misc, "misc.stopshake", "Stop camera shake", {"STOP_GAMEPLAY_CAM_SHAKING"},
               [](Ctx&, Feature&) { A::STOP_GAMEPLAY_CAM_SHAKING(true); });
        Action(C::Misc, "misc.clearfx", "Clear screen effects", {"ANIMPOSTFX_STOP_ALL", "CLEAR_TIMECYCLE_MODIFIER"}, [](Ctx&, Feature&) {
            A::ANIMPOSTFX_STOP_ALL();
            A::CLEAR_TIMECYCLE_MODIFIER();
        });
        Float(C::Misc, "misc.tcstrength", "Colour filter strength", {"SET_TIMECYCLE_MODIFIER_STRENGTH"}, 1.0f, 0.0f, 2.0f, 0.1f,
              [](Ctx&, Feature& f) { A::SET_TIMECYCLE_MODIFIER_STRENGTH(f.f); });
        Action(C::Misc, "misc.radionext", "Next radio station", {"SKIP_RADIO_FORWARD"}, [](Ctx&, Feature&) { A::SKIP_RADIO_FORWARD(); });
        Action(C::Misc, "misc.logpos", "Log position", {"GET_ENTITY_COORDS", "GET_ENTITY_HEADING"}, [](Ctx& c, Feature&) {
            Vec3 p = A::GET_ENTITY_COORDS(c.Target(), true);
            char b[128];
            std::snprintf(b, sizeof(b), "trainer: position %.3f, %.3f, %.3f heading %.1f deg", p.x, p.y, p.z, A::GET_ENTITY_HEADING(c.Target()) * kRadToDeg);
            Log(b);
        });
        Action(C::Misc, "misc.logmodel", "Log model of player and vehicle", {"GET_ENTITY_MODEL"}, [](Ctx& c, Feature&) {
            char b[128];
            std::snprintf(b, sizeof(b), "trainer: ped model 0x%llX, vehicle model 0x%llX (64-bit keys: use these in trainer_models.txt)",
                          static_cast<unsigned long long>(ModelOf(c.ped)), static_cast<unsigned long long>(c.inVehicle ? ModelOf(c.veh) : 0));
            Log(b);
        });
        Action(C::Misc, "misc.character", "Log current character", {"VI_GET_PLAYER_CHARACTER_HASH", "GET_PLAYER_INDEX"}, [](Ctx&, Feature&) {
            std::uint32_t h = static_cast<std::uint32_t>(A::VI_GET_PLAYER_CHARACTER_HASH(A::GET_PLAYER_INDEX()));
            const char* who = h == HashName32("jason") ? "jason" : h == HashName32("lucia") ? "lucia" : "other";
            char b[96];
            std::snprintf(b, sizeof(b), "trainer: playing as %s (0x%08X)", who, h);
            Log(b);
        });
        // Read-only: logs what the getters return, each only if its native is bound. For checking names in a run.
        Action(C::Misc, "misc.probe", "Log a read-only probe of the player", {"GET_ENTITY_COORDS"}, [this](Ctx& c, Feature&) {
            auto has = [this](const char* n) { return exists_(n); };
            char b[256];
            Vec3 p = A::GET_ENTITY_COORDS(c.ped, true);
            std::snprintf(b, sizeof(b), "trainer: probe pos %.2f %.2f %.2f", p.x, p.y, p.z);
            Log(b);
            if (has("GET_ENTITY_HEALTH") && has("GET_ENTITY_MAX_HEALTH")) {
                std::snprintf(b, sizeof(b), "trainer: probe health %d / %d", A::GET_ENTITY_HEALTH(c.ped), A::GET_ENTITY_MAX_HEALTH(c.ped, false));
                Log(b);
            }
            if (has("VI_GET_SECONDARY_PLAYER_PED")) {
                Ref o = A::VI_GET_SECONDARY_PLAYER_PED();
                if (!Valid(o)) Log("trainer: probe second character: none");
                else {
                    Vec3 q = A::GET_ENTITY_COORDS(o, false);
                    std::snprintf(b, sizeof(b), "trainer: probe second character: %s the player ped, model 0x%08X, at %.1f %.1f %.1f",
                                  (o.lo == c.ped.lo && o.hi == c.ped.hi) ? "IS" : "is NOT", has("GET_ENTITY_MODEL") ? A::GET_ENTITY_MODEL(o) : 0, q.x, q.y, q.z);
                    Log(b);
                }
            }
            if (has("VI_GET_PLAYER_SPECIAL_ABILITY_AMOUNT") && has("IS_SPECIAL_ABILITY_ACTIVE") && has("IS_SPECIAL_ABILITY_ENABLED")) {
                std::snprintf(b, sizeof(b), "trainer: probe ability 0: amount %.3f enabled %d active %d | ability 1: amount %.3f enabled %d active %d",
                              A::VI_GET_PLAYER_SPECIAL_ABILITY_AMOUNT(c.ped, 0), A::IS_SPECIAL_ABILITY_ENABLED(c.ped, 0) ? 1 : 0,
                              A::IS_SPECIAL_ABILITY_ACTIVE(c.ped, 0) ? 1 : 0, A::VI_GET_PLAYER_SPECIAL_ABILITY_AMOUNT(c.ped, 1),
                              A::IS_SPECIAL_ABILITY_ENABLED(c.ped, 1) ? 1 : 0, A::IS_SPECIAL_ABILITY_ACTIVE(c.ped, 1) ? 1 : 0);
                Log(b);
            }
            if (has("VI_GET_PLAYER_CHARACTER_HASH")) {
                std::snprintf(b, sizeof(b), "trainer: probe player character 0x%08X, player model 0x%08X", A::VI_GET_PLAYER_CHARACTER_HASH(0),
                              has("GET_ENTITY_MODEL") ? A::GET_ENTITY_MODEL(c.ped) : 0);
                Log(b);
            }
            if (has("GET_PLAYER_WANTED_LEVEL")) {
                std::snprintf(b, sizeof(b), "trainer: probe wanted level %d", A::GET_PLAYER_WANTED_LEVEL(c.ped));
                Log(b);
            }
            if (has("GET_CLOCK_HOURS") && has("GET_CLOCK_MINUTES")) {
                std::snprintf(b, sizeof(b), "trainer: probe clock %02d:%02d", A::GET_CLOCK_HOURS(), A::GET_CLOCK_MINUTES());
                Log(b);
            }
            if (has("GET_ENTITY_HEADING") && has("GET_ENTITY_SPEED")) {
                float hd = A::GET_ENTITY_HEADING(c.ped);
                std::snprintf(b, sizeof(b), "trainer: probe heading %.3f rad (%.1f deg) speed %.2f", hd, hd * kRadToDeg, A::GET_ENTITY_SPEED(c.ped));
                Log(b);
            }
            if (has("GET_ENTITY_FORWARD_VECTOR")) {
                Vec3 f = A::GET_ENTITY_FORWARD_VECTOR(c.ped);
                std::snprintf(b, sizeof(b), "trainer: probe forward %.3f %.3f %.3f (length %.3f)", f.x, f.y, f.z, std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z));
                Log(b);
            }
            if (has("GET_ENTITY_MODEL")) {
                std::snprintf(b, sizeof(b), "trainer: probe ped model 0x%08X", A::GET_ENTITY_MODEL(c.ped));
                Log(b);
            }
            if (has("GET_PLAYER_INDEX")) {
                int idx = A::GET_PLAYER_INDEX();
                std::snprintf(b, sizeof(b), "trainer: probe player index %d", idx);
                Log(b);
                if (has("VI_GET_PLAYER_CHARACTER_HASH")) {
                    std::uint32_t h = static_cast<std::uint32_t>(A::VI_GET_PLAYER_CHARACTER_HASH(idx));
                    std::snprintf(b, sizeof(b), "trainer: probe character 0x%08X (%s)", h, h == HashName32("jason") ? "jason" : h == HashName32("lucia") ? "lucia" : "other");
                    Log(b);
                }
                if (has("GET_PLAYER_PED")) {
                    Ref pp = A::GET_PLAYER_PED(idx);
                    std::snprintf(b, sizeof(b), "trainer: probe GET_PLAYER_PED(index) %s PLAYER_PED_ID()", Same(pp, c.ped) ? "==" : "!=");
                    Log(b);
                }
            }
            if (has("VI_GET_PED_VITAL") && has("VI_GET_PED_VITAL_MAX"))
                for (int i = 0; i < kVitals; ++i) {
                    std::snprintf(b, sizeof(b), "trainer: probe vital %d = %.1f / %.1f", i, A::VI_GET_PED_VITAL(c.ped, i), A::VI_GET_PED_VITAL_MAX(c.ped, i));
                    Log(b);
                }
            if (has("IS_PED_IN_ANY_VEHICLE") && has("IS_PED_ON_FOOT") && has("IS_ENTITY_DEAD")) {
                std::snprintf(b, sizeof(b), "trainer: probe in vehicle %d, on foot %d, dead %d", A::IS_PED_IN_ANY_VEHICLE(c.ped, false, false, true) ? 1 : 0,
                              A::IS_PED_ON_FOOT(c.ped) ? 1 : 0, A::IS_ENTITY_DEAD(c.ped) ? 1 : 0);
                Log(b);
            }
            if (c.inVehicle && has("GET_ENTITY_SPEED")) {
                Vec3 vp = A::GET_ENTITY_COORDS(c.veh, false);
                std::snprintf(b, sizeof(b), "trainer: probe vehicle model 0x%08X at %.1f %.1f %.1f speed %.2f, engine %.0f body %.0f tank %.0f",
                              has("GET_ENTITY_MODEL") ? A::GET_ENTITY_MODEL(c.veh) : 0, vp.x, vp.y, vp.z, A::GET_ENTITY_SPEED(c.veh),
                              has("GET_VEHICLE_ENGINE_HEALTH") ? A::GET_VEHICLE_ENGINE_HEALTH(c.veh) : -1.0f,
                              has("GET_VEHICLE_BODY_HEALTH") ? A::GET_VEHICLE_BODY_HEALTH(c.veh) : -1.0f,
                              has("GET_VEHICLE_PETROL_TANK_HEALTH") ? A::GET_VEHICLE_PETROL_TANK_HEALTH(c.veh) : -1.0f);
                Log(b);
                if (has("GET_VEHICLE_DOOR_LOCK_STATUS") && has("IS_ENTITY_VISIBLE") && has("IS_ENTITY_DEAD")) {
                    std::snprintf(b, sizeof(b), "trainer: probe vehicle state lock %d visible %d dead %d", A::GET_VEHICLE_DOOR_LOCK_STATUS(c.veh),
                                  A::IS_ENTITY_VISIBLE(c.veh) ? 1 : 0, A::IS_ENTITY_DEAD(c.veh) ? 1 : 0);
                    Log(b);
                }
            } else {
                auto near = Nearby(c, SHVI_ENTITY_VEHICLE, 80.0f, true);
                auto nearPeds = Nearby(c, SHVI_ENTITY_PED, 80.0f, true);
                std::snprintf(b, sizeof(b), "trainer: probe not in a vehicle; %zu vehicles and %zu peds within 80 m", near.size(), nearPeds.size());
                Log(b);
            }
            if (has("IS_ENTITY_VISIBLE") && has("GET_ENTITY_ALPHA") && has("GET_ENTITY_COLLISION_DISABLED") && has("GET_ENTITY_CAN_BE_DAMAGED") &&
                has("GET_PED_ACCURACY")) {
                std::snprintf(b, sizeof(b), "trainer: probe state visible %d alpha %d collision-off %d can-be-damaged %d accuracy %d on-fire %d",
                              A::IS_ENTITY_VISIBLE(c.ped) ? 1 : 0, A::GET_ENTITY_ALPHA(c.ped), A::GET_ENTITY_COLLISION_DISABLED(c.ped) ? 1 : 0,
                              A::GET_ENTITY_CAN_BE_DAMAGED(c.ped) ? 1 : 0, A::GET_PED_ACCURACY(c.ped),
                              has("IS_ENTITY_ON_FIRE") && A::IS_ENTITY_ON_FIRE(c.ped) ? 1 : 0);
                Log(b);
            }
            if (has("GET_RAIN_LEVEL") && has("GET_WIND_SPEED") && has("GET_CLOCK_MINUTES") && has("GET_NEXT_WEATHER_TYPE_HASH_NAME") &&
                has("GET_PREV_WEATHER_TYPE_HASH_NAME")) {
                std::snprintf(b, sizeof(b), "trainer: probe world rain %.2f wind %.2f minutes %d weather prev 0x%08X next 0x%08X snow %.2f",
                              A::GET_RAIN_LEVEL(), A::GET_WIND_SPEED(), A::GET_CLOCK_MINUTES(), A::GET_PREV_WEATHER_TYPE_HASH_NAME(),
                              A::GET_NEXT_WEATHER_TYPE_HASH_NAME(), has("GET_SNOW_LEVEL") ? A::GET_SNOW_LEVEL() : -1.0f);
                Log(b);
            }
            if (has("GET_BEST_PED_WEAPON") && has("HAS_PED_GOT_WEAPON") && has("IS_PED_ARMED")) {
                int have = 0;
                std::uint32_t first = 0;
                for (const HashName& w : kWeapons)
                    if (A::HAS_PED_GOT_WEAPON(c.ped, w.hash, 0, false)) {
                        if (!have) first = w.hash;
                        ++have;
                    }
                std::snprintf(b, sizeof(b), "trainer: probe weapons best 0x%08X, has %d of %zu listed (first 0x%08X), armed %d",
                              A::GET_BEST_PED_WEAPON(c.ped, false, true), have, std::size(kWeapons), first, A::IS_PED_ARMED(c.ped, 7) ? 1 : 0);
                Log(b);
            }
            if (has("GET_GAMEPLAY_CAM_COORD") && has("GET_GAMEPLAY_CAM_FOV")) {
                Vec3 cam = A::GET_GAMEPLAY_CAM_COORD();
                std::snprintf(b, sizeof(b), "trainer: probe camera %.2f %.2f %.2f fov %.1f", cam.x, cam.y, cam.z, A::GET_GAMEPLAY_CAM_FOV());
                Log(b);
            }
            if (has("GET_FRAME_COUNT") && has("GET_GAME_TIMER") && has("GET_FRAME_TIME")) {
                std::snprintf(b, sizeof(b), "trainer: probe frame %d, game timer %d ms, frame time %.4f s", A::GET_FRAME_COUNT(), A::GET_GAME_TIMER(), A::GET_FRAME_TIME());
                Log(b);
            }
            if (has("IS_WAYPOINT_ACTIVE") && has("GET_MAX_WANTED_LEVEL") && has("IS_SCREEN_FADED_IN")) {
                std::snprintf(b, sizeof(b), "trainer: probe waypoint %d, max wanted %d, screen faded in %d", A::IS_WAYPOINT_ACTIVE() ? 1 : 0, A::GET_MAX_WANTED_LEVEL(),
                              A::IS_SCREEN_FADED_IN() ? 1 : 0);
                Log(b);
            }
            if (has("_GET_PED_STAMINA") && has("_GET_PED_MAX_STAMINA")) {
                std::snprintf(b, sizeof(b), "trainer: probe stamina %.1f / %.1f", A::_GET_PED_STAMINA(c.ped), A::_GET_PED_MAX_STAMINA(c.ped));
                Log(b);
            }
            // Read-backs for the experimental features (what each one should change).
            if (has("GET_WANTED_SCORE")) {
                std::snprintf(b, sizeof(b), "trainer: probe wanted score %d", A::GET_WANTED_SCORE(c.ped));
                Log(b);
            }
            if (has("_GET_CURRENT_PED_WEAPON_AMMO_TYPE") && has("GET_PED_AMMO_BY_TYPE")) {
                std::uint32_t at = A::_GET_CURRENT_PED_WEAPON_AMMO_TYPE(c.ped, Ref{});
                std::snprintf(b, sizeof(b), "trainer: probe current ammo type 0x%08X: %d rounds", at, at ? A::GET_PED_AMMO_BY_TYPE(c.ped, at) : -1);
                Log(b);
                int shown = 0;
                for (const HashName& a : kAmmoTypes) {
                    if (!a.hash || shown >= 6) continue;
                    int n = A::GET_PED_AMMO_BY_TYPE(c.ped, a.hash);
                    if (n <= 0) continue;
                    ++shown;
                    std::snprintf(b, sizeof(b), "trainer: probe ammo %s (0x%08X) %d", a.name, a.hash, n);
                    Log(b);
                }
            }
            if (has("IS_PLAYER_TELEPORT_ACTIVE") && has("IS_ENTITY_IN_WATER") && has("GET_ENTITY_HEIGHT_ABOVE_GROUND")) {
                std::snprintf(b, sizeof(b), "trainer: probe teleport active %d, in water %d, height above ground %.2f", A::IS_PLAYER_TELEPORT_ACTIVE() ? 1 : 0,
                              A::IS_ENTITY_IN_WATER(c.ped) ? 1 : 0, A::GET_ENTITY_HEIGHT_ABOVE_GROUND(c.ped, true));
                Log(b);
            }
            if (c.inVehicle && has("IS_VEHICLE_TYRE_BURST") && has("IS_VEHICLE_ALARM_ACTIVATED")) {
                int burst = 0;
                for (int w = 0; w < 8; ++w) burst += A::IS_VEHICLE_TYRE_BURST(c.veh, w, false) ? 1 : 0;
                std::snprintf(b, sizeof(b), "trainer: probe vehicle tyres burst %d, alarm %d, tyres can burst %d", burst, A::IS_VEHICLE_ALARM_ACTIVATED(c.veh) ? 1 : 0,
                              has("GET_VEHICLE_TYRES_CAN_BURST") && A::GET_VEHICLE_TYRES_CAN_BURST(c.veh) ? 1 : 0);
                Log(b);
            }
        });
        Action(C::Misc, "misc.counts", "Log entity counts", {}, [](Ctx&, Feature&) {
            char b[128];
            std::snprintf(b, sizeof(b), "trainer: %d peds, %d vehicles, %d objects", shvi::world_get_all(SHVI_ENTITY_PED, nullptr, 0),
                          shvi::world_get_all(SHVI_ENTITY_VEHICLE, nullptr, 0), shvi::world_get_all(SHVI_ENTITY_OBJECT, nullptr, 0));
            Log(b);
        });
    }

    // Adds the 64-bit model key of every vehicle within 150 m that is not listed yet, to the menu list
    // and to shvi	rainer_models.txt when SetModelFile was called. Names cannot be used: they only give
    // the low half of the key.
    void CollectVehicleModels(const Ctx& c) {
        Feature* m = Find("spawn.model");
        if (models_.empty()) m->options.clear();
        int added = 0;
        for (Ref v : Nearby(c, SHVI_ENTITY_VEHICLE, 150.0f, false)) {
            ModelKey key = ModelOf(v);
            if (!key || std::find(models_.begin(), models_.end(), key) != models_.end()) continue;
            char e[24];
            std::snprintf(e, sizeof(e), "0x%016llX", static_cast<unsigned long long>(key));
            models_.push_back(key);
            m->options.emplace_back(e);
            if (!modelFile_.empty())
                if (std::FILE* f = std::fopen(modelFile_.c_str(), "a")) { std::fprintf(f, "%s\n", e); std::fclose(f); }
            ++added;
        }
        if (m->options.empty()) m->options.emplace_back("(list is empty)");
        if (m->i < 0 || m->i >= static_cast<int>(m->options.size())) m->i = 0;
        char b[96];
        std::snprintf(b, sizeof(b), "trainer: %d new vehicle model(s) listed, %zu in total", added, models_.size());
        Log(b);
    }
    std::string modelFile_;

public:
    void SetModelFile(std::string path) { modelFile_ = std::move(path); }
    // Model list for the spawner: "name" (hashed) or "0xHEX" per entry. Call before BuildMenu().
    void SetModelList(const std::vector<std::string>& entries) {
        models_.clear();
        Feature* m = Find("spawn.model");
        m->options.clear();
        for (const auto& e : entries) {
            if (e.empty() || e[0] == '#' || e[0] == ';') continue;
            models_.push_back(ParseHashEntry(e));
            m->options.push_back(e);
        }
        if (m->options.empty()) m->options.emplace_back("(list is empty)");
        m->i = 0;
    }

    // No-clip speed factor: -1 back, 0 stand, +1 forward. Set from the plugin's input handling.
    void SetNoclipInput(float v) { noclipInput_ = v; }

private:
    static constexpr int kSlots = 9;
    struct Slot {
        Vec3 pos{};
        float heading = 0;
        bool used = false;
    };
    std::vector<Feature> features_;
    std::vector<ModelKey> models_;
    Slot slots_[kSlots]{};
    Ctx ctx_;
    Exists exists_;
    Clock clock_;
    std::function<bool()> canScriptCall_;
    ScriptCall callScript_;
    PinFn pinScript_;
    UnpinFn unpinScript_;
    std::string status_;
    std::uint32_t lastTick_ = 0;
    float noclipInput_ = 0.0f;
    bool haveCore_ = false;
};

}  // namespace trainer
