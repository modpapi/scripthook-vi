// Game-ready gate (TASKS T1.9/T2.6). Plugin fibers and tick callbacks run only
// while the profile says the game is in play AND the online guard allows it.
// "Unknown" (a profile without the signal, or one it could not read) keeps the
// gate closed: same fail-closed rule as the guard. Pure; unit-tested.
#pragma once

namespace shvi {

enum class Readiness { Unknown, NotReady, Ready };

class ReadyGate {
public:
    // Feed both signals once per tick; returns whether plugins may run now.
    bool Update(Readiness game, bool guardAllowed) {
        bool open = guardAllowed && game == Readiness::Ready;
        changed_ = open != open_;
        open_ = open;
        return open_;
    }
    bool Open() const { return open_; }
    bool Changed() const { return changed_; }  // the last Update flipped the gate

private:
    bool open_ = false;
    bool changed_ = false;
};

}  // namespace shvi
