// The seam between the generic core and one game build family.
// Everything that knows a byte pattern, a struct offset, a native hash or a
// script name lives behind this interface (src/games/<game>/).
#pragma once
#include "game_ready.h"
#include "invoker.h"
#include "native_context.h"
#include "online_guard.h"
#include "pattern.h"
#include "pe.h"

#include <memory>
#include <string>
#include <vector>

namespace shvi {

struct HostInfo {
    HMODULE exe = nullptr;
    std::wstring exePath, exeName, exeDir;
    pe::FileVersion version;
};

enum class ResolveResult { Ok, Retry /* code not unpacked yet */, Unsupported };

class GameProfile {
public:
    using TickFn = void (*)();
    virtual ~GameProfile() = default;

    virtual const char* Name() const = 0;
    virtual bool Matches(const HostInfo& host) const = 0;
    virtual std::uint32_t Build() const = 0;

    // Preflight guard inputs.
    virtual std::vector<std::wstring_view> AntiCheatModules() const = 0;
    virtual std::vector<std::wstring_view> OnlineCommandLineFlags() const = 0;

    // Find everything the core needs. Called repeatedly while it returns Retry.
    virtual ResolveResult Resolve(const HostInfo& host, const pe::Module& exe, std::string* log) = 0;

    // What the invoker calls for a hash: an engine thunk or a VI script native
    // (engine_call.h), or nothing.
    virtual NativeTarget FindTarget(std::uint64_t hash) = 0;

    // The signature set, exposed so tools/scan can check it against an exe on
    // disk. Profiles that keep their targets private return {}.
    virtual std::vector<scan::Target> ScanTargets() const { return {}; }
    virtual rage::VectorFixupFn VectorFixup() const { return rage::GenericVectorFixup; }

    // Arrange for tick() to be called once per frame on the game's script
    // thread, with a valid script context active.
    virtual bool InstallTick(TickFn tick) = 0;
    virtual void RemoveTick() = 0;

    // Runtime guard probe; uses InvokeUnchecked. Game thread.
    virtual GameMode QueryMode(Invoker& inv) = 0;

    // Is the game in play (GTA V's eGameState::Playing)? Game thread, each tick.
    // Unknown keeps plugins off (ReadyGate).
    virtual Readiness GameReady() { return Readiness::Unknown; }

    virtual std::uint64_t* GlobalPtr(std::int32_t /*index*/) { return nullptr; }

    // Entities (ABI 1.2). A reference is the game's 16-byte script entity
    // reference (two u64). WorldGetAll returns the live count or a negative
    // SHVI_E_* status (-4 = unsupported). Game thread, inside a script callback.
    virtual int WorldGetAll(std::uint32_t /*kind*/, std::uint64_t* /*refs*/, int /*max*/) { return -4; }
    virtual void* EntityPtr(const std::uint64_t* /*ref*/) { return nullptr; }

    // Module statics (ABI 1.3, VI's getGlobalPtr): field `field` of module
    // `module`'s statics as seen by the first live script instance running
    // program `program` (module 0 = that program's own statics). Game thread.
    virtual void* StaticPtr(std::uint32_t /*program*/, std::uint32_t /*module*/, std::uint32_t /*field*/) { return nullptr; }

    // ABI 1.6: run a function of a linked script program on the script thread this tick
    // borrowed. A negative SHVI_E_* status when it cannot (-4 = unsupported).
    virtual int CallScriptFunction(std::uint32_t /*program*/, std::uint32_t /*offset*/, const std::uint64_t* /*args*/,
                                   std::uint32_t /*argc*/, std::uint64_t* /*ret*/, std::uint32_t /*retc*/) {
        return -4;
    }

    // The game's WAIT function, if one is bound. Plugin calls to it are turned
    // into a fiber yield: calling the real one would put the borrowed game
    // thread to sleep instead of the plugin.
    virtual std::uint64_t WaitNativeHash() const { return 0; }
};

std::vector<std::unique_ptr<GameProfile>> CreateGta6Profiles();

}  // namespace shvi
