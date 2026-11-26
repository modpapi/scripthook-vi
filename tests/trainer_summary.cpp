// Prints how many trainer features exist per category and how strong their weakest native name is.
#include "../samples/trainer/trainer.hpp"
#include <cstdio>
#include <cstring>
#include <map>
int main(int argc, char** argv) {
    trainer::Trainer t;
    t.SetExists([](const char*) { return true; });
    t.Refresh();
    if (argc > 1 && std::strcmp(argv[1], "--list") == 0) {  // every feature that is not usable by default, and why
        for (auto& f : t.Features()) {
            if (t.Usable(f)) continue;
            std::string natives;
            for (const char* n : f.needs) { if (!natives.empty()) natives += ","; natives += n; }
            std::printf("%-9s %-30s %-8s %-12s %s | %s\n", trainer::CategoryName(f.cat), f.id.c_str(),
                        f.blocked ? "blocked" : "locked", trainer::ConfName(f.weakest), f.guess ? "guess" : (f.scriptCall ? "script" : "-"),
                        natives.c_str());
        }
        return 0;
    }
    std::map<int, int> per, exp;
    int byConf[8] = {};
    for (auto& f : t.Features()) { per[(int)f.cat]++; exp[(int)f.cat] += f.experimental; byConf[(int)f.weakest]++; }
    int n = 0, e = 0;
    for (auto& [c, k] : per) { std::printf("%-9s %3d features, %2d experimental\n", trainer::CategoryName((trainer::Category)c), k, exp[c]); n += k; e += exp[c]; }
    std::printf("total %d, experimental %d\nweakest native: verified %d, corroborated %d, inferred %d, guide %d, conflict %d\n", n, e, byConf[0], byConf[1], byConf[2], byConf[3], byConf[4]);
}
