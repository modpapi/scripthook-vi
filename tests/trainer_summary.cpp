// Prints how many trainer features exist per category and how strong their weakest native name is.
#include "../samples/trainer/trainer.hpp"
#include <cstdio>
#include <map>
int main() {
    trainer::Trainer t;
    t.SetExists([](const char*) { return true; });
    t.Refresh();
    std::map<int, int> per, exp;
    int byConf[8] = {};
    for (auto& f : t.Features()) { per[(int)f.cat]++; exp[(int)f.cat] += f.experimental; byConf[(int)f.weakest]++; }
    int n = 0, e = 0;
    for (auto& [c, k] : per) { std::printf("%-9s %3d features, %2d experimental\n", trainer::CategoryName((trainer::Category)c), k, exp[c]); n += k; e += exp[c]; }
    std::printf("total %d, experimental %d\nweakest native: verified %d, corroborated %d, inferred %d, guide %d, conflict %d\n", n, e, byConf[0], byConf[1], byConf[2], byConf[3], byConf[4]);
}
