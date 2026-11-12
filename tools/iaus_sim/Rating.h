#pragma once
// iaus_sim/Rating.h -- THE RATING BATTERY: one shared set of fights every class of a tier plays IDENTICALLY, so two classes of
// one tier can be compared (marth 11-12: "a rating(Tx) column. so we can easily see if a class in a tier is less effective than
// another. which means scenarios will have to be even more diverse"). HOST-ONLY.
//
// A CELL is (foe archetype, group size, level band). 10 archetypes x 2 sizes x 2 bands = 40 cells, N fights each (--rating N).
// WHAT IS IDENTICAL for every class of a tier in one fight of one cell (the fight is a function of tier, cell, fight index and
// the run's seed, never of the class):
//   - the party's level (the tier's band: T1 12 / 18, T2 22 / 28, T3 32 / 38, T4 42 / 48) and so the gear ladder rung of every
//     weapon and armor piece he may carry (Load.h LadderAt reads the level only): the EQUAL GEAR BUDGET of a tier;
//   - his potions (2 health, 2 magicka, 1 stamina) and arrows (60), the player (family, build, potions), the second follower;
//   - the foes: which records, how many, their strength (scaled once against a REFERENCE party of that level whose follower is
//     the T1 melee row, FightSpec::refCls, so the scale never reads the rated class), where they stand, the cover, the light;
//   - the opening draw of the fight's random stream (FightSpec::geoSeed).
// What differs is the class: its gate, kit, package, curves, known spells (his own default rows' spells: `ensure`).
//
// FOES are picked by ROLE from the loaded world (Data::roles: the list's own actors when a list is loaded, the fixtures when
// none is). A list's actors are TEST INPUT only; nothing here names a record, and a role the world lacks falls back to the next
// role of the archetype and then to the first spec, exactly as Scenarios.h does.
#include "iaus_sim/Results.h"
#include "iaus_sim/Scenarios.h"

namespace Sim {

    struct RatingArch { const char* name; const char* what; float ratio; };
    inline constexpr RatingArch kRatingArch[] = {
        { "swarm",   "a swarm of weak blades and teeth: many small bodies at once", 0.80f },
        { "archers", "archers at range: they hurt from where he cannot reach at once", 0.85f },
        { "mages",   "hostile casters: elements, wards to break, casts to interrupt", 0.85f },
        { "healer",  "grunts with a healer behind them: nothing dies while the healer lives", 0.90f },
        { "boss",    "one foe far above the party (alone, or with two adds)", 1.00f },
        { "undead",  "draugr: undead, resist frost and poison, never flee", 0.85f },
        { "beast",   "a big creature (troll / giant): slow heavy blows, a lot of health", 0.95f },
        { "ambush",  "the foes open ON him at arm's length: no approach, no first shot", 0.85f },
        { "kite",    "a brute he cannot trade with is on him: distance is the only answer", 1.10f },
        { "mixed",   "a mixed camp: blade, bow, spell (and a healer in the large one)", 0.90f },
    };
    inline constexpr int kRatingArchN = static_cast<int>(sizeof kRatingArch / sizeof kRatingArch[0]);
    inline constexpr int kRatingCells = kRatingArchN * 4;   // x 2 group sizes x 2 level bands
    [[nodiscard]] inline int RatingLevel(int tier, int band) { return 2 + 10 * std::clamp(tier, 1, 4) + (band ? 6 : 0); }   // T1 12 / 18 ... T4 42 / 48

    // One fight of the battery. cell = arch * 4 + size * 2 + band. `R` must be seeded from (seed, tier, cell, fight) ONLY.
    inline FightSpec RatingFight(const Data& D, const SimClass& cls, const std::vector<SimClass>& all, int cell, int f, Rng& R) {
        const int arch = cell / 4, size = (cell / 2) % 2, band = cell % 2, L = RatingLevel(cls.tier, band);
        FightSpec fs; fs.cls = &cls; fs.refCls = &all[0]; fs.level = L; fs.rank = 1 + f % 5; fs.ensure = cls.ensure;
        fs.playerFam = static_cast<Cls>((cell + f) % 3);
        if (size == 1) fs.others.push_back(&all[static_cast<std::size_t>((cell / 4 + f) % 3)]);   // the large cells: a second (T1) follower
        fs.subjectPotionsH = 2; fs.subjectPotionsM = 2; fs.subjectPotionsS = 1; fs.subjectArrows = 60; fs.playerPotions = 3;
        fs.interior = (arch + band) % 2 == 0; fs.light = fs.interior ? 0.35f : 0.9f; fs.occluders = fs.interior ? 2 : 0; fs.foeDist = 1300; fs.foeSpread = 250;
        fs.targetRatio = kRatingArch[arch].ratio * (size ? 1.05f : 0.95f);
        auto add = [&](std::initializer_list<const char*> roles, int level, const char* tag, float hpX = 1, float dmgX = 1, const char* name = nullptr) { fs.customFoes.push_back(Foe(D, roles, level, R, hpX, dmgX, name)); fs.foeTags.push_back(tag); };
        switch (arch) {
        case 0: for (int i = 0; i < (size ? 6 : 4); ++i) add({ "wolf", "bandit" }, std::max(1, L - 8), "grunt"); fs.foeSpread = 350; break;
        case 1: for (int i = 0; i < (size ? 4 : 2); ++i) add({ "bandit_archer", "draugr_archer" }, L, "archer"); fs.foeDist = 1900; break;
        case 2: for (int i = 0; i < (size ? 3 : 2); ++i) add({ "bandit_mage", "mage" }, L, "caster"); fs.foeDist = 1600; break;
        case 3: for (int i = 0; i < (size ? 3 : 2); ++i) add({ "bandit", "draugr" }, L, "grunt", 1.3f); add({ "healer" }, L, "healer"); break;
        case 4: add({ "bandit", "draugr" }, L + 8, "boss", 5.0f, 2.0f, "Boss"); if (size) for (int i = 0; i < 2; ++i) add({ "bandit", "draugr" }, std::max(1, L - 4), "grunt"); break;
        case 5: for (int i = 0; i < (size ? 4 : 3); ++i) add({ "draugr" }, L, "grunt"); if (size) add({ "draugr_archer", "draugr" }, L, "archer"); break;
        case 6: for (int i = 0; i < (size ? 2 : 1); ++i) add({ "troll", "giant", "frost_troll" }, L, "boss", size ? 1.0f : 1.6f); break;
        case 7: for (int i = 0; i < (size ? 4 : 2); ++i) add({ "bandit", "draugr" }, L, "grunt"); fs.onSubject = true; fs.foeDist = 450; fs.foeSpread = 150; fs.freeEntry = 0; break;
        case 8: add({ "troll", "giant", "bandit" }, L + 4, "boss", 2.0f, 2.0f, "Brute"); if (size) add({ "bandit_archer", "bandit" }, L, "archer"); fs.onSubject = true; fs.foeDist = 900; break;
        default: add({ "bandit", "draugr" }, L, "grunt"); add({ "bandit_archer", "draugr_archer" }, L, "archer"); add({ "bandit_mage", "mage" }, L, "caster");
                 if (size) { add({ "bandit", "draugr" }, L, "grunt"); add({ "healer" }, L, "healer"); } break;
        }
        fs.seed = R.g(); fs.geoSeed = R.g() | 1u;
        return fs;
    }

    // Rank inside each tier. BELOW-TIER flag: the class is under its tier's mean by more than 2.0 U AND by more than two standard
    // errors of the difference (the fights are the same for the whole tier, so the difference is paired per fight: `pairedSe`).
    inline void RankRatings(std::vector<RatingRow>& rows, const std::vector<std::vector<double>>& perFight) {
        std::map<int, std::vector<std::size_t>> byTier; for (std::size_t i = 0; i < rows.size(); ++i) byTier[rows[i].tier].push_back(i);
        for (auto& kv : byTier) { const auto& ix = kv.second; const std::size_t n = ix.size();
            double mean = 0; for (std::size_t i : ix) mean += rows[i].score; mean /= static_cast<double>(n);
            std::map<std::string, double> archMean; for (std::size_t i : ix) for (const auto& a : rows[i].byArch) archMean[a.first] += a.second / static_cast<double>(n);
            std::vector<std::size_t> order = ix; std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return rows[a].score > rows[b].score; });
            const std::size_t nf = perFight[ix[0]].size(); std::vector<double> tierFight(nf, 0.0);
            for (std::size_t i : ix) for (std::size_t k = 0; k < nf && k < perFight[i].size(); ++k) tierFight[k] += perFight[i][k] / static_cast<double>(n);
            for (std::size_t r = 0; r < order.size(); ++r) { RatingRow& x = rows[order[r]]; x.rank = static_cast<int>(r) + 1; x.of = static_cast<int>(n); x.tierMean = mean; x.delta = x.score - mean;
                double pairedSe = 0; if (n > 1 && nf > 1) { const auto& pf = perFight[order[r]]; double m = 0, v = 0; for (std::size_t k = 0; k < nf; ++k) m += (pf[k] - tierFight[k]) / static_cast<double>(nf);
                    for (std::size_t k = 0; k < nf; ++k) { const double d = pf[k] - tierFight[k] - m; v += d * d; } pairedSe = std::sqrt(v / static_cast<double>(nf - 1) / static_cast<double>(nf)); }
                x.below = n > 1 && x.delta < -2.0 && x.delta < -2.0 * pairedSe;
                x.weakest.clear(); x.weakestDelta = 0; for (const auto& a : x.byArch) { const double d = a.second - archMean[a.first]; if (d < x.weakestDelta) { x.weakestDelta = d; x.weakest = a.first; } } }
        }
    }

}
