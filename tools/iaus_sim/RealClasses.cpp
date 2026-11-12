// iaus_sim/RealClasses.cpp -- the sim's class cells, built from the GAME'S OWN class table. HOST-ONLY.
//
// This TU and native/classes/ClassTable.cpp are compiled with tools/iaus_sim/stub/ and the generated Vocabulary.h
// FIRST on the include path (run.sh), so `MFO::Classes::kTable` here is the very array the DLL holds: all 35 rows,
// their weapon sets, default gambit rows, power shares and flags. Nothing is copied by hand.
//
// The gate is SpellBook::GateFor's logic (native/iaus/SpellBook.cpp:374) over a table row, with the tree ON and the
// classes his unlock rule requires counted as mastered. The I8 profile is the mechanical derivation Core.h documents
// for BumpRow (one call per APPLIED default gambit row). I8 has not landed: the profile is used only with --profiles mech.
#include "classes/Classes_internal.h"

#include "iaus_sim/Setup.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace Sim {

    namespace {
        namespace C = MFO::Classes;
        namespace V = MFO::Vocab;

        bool Casting(const C::ClassDef& d) { return d.line == C::Line::Mage || d.line == C::Line::Hybrid; }

        // What he must have mastered to wear this class: allOf, the first needA of poolA, the first needB of poolB, and theirs.
        C::Mask Mastered(const C::ClassDef& d, int depth = 0) {
            C::Mask m = d.unlock.allOf;
            auto take = [&](C::Mask pool, int need) { for (std::size_t i = 0; i < C::kClassCount && need > 0; ++i) if (pool & (C::Mask{ 1 } << i)) { if (!(m & (C::Mask{ 1 } << i))) m |= C::Mask{ 1 } << i; --need; } };
            take(d.unlock.poolA, d.unlock.needA); take(d.unlock.poolB, d.unlock.needB);
            if (depth < 5) { const C::Mask direct = m; for (std::size_t i = 0; i < C::kClassCount; ++i) if (direct & (C::Mask{ 1 } << i)) m |= Mastered(C::kTable[i], depth + 1); }
            return m;
        }

        void Bump(Profile& p, const C::DefaultGambit& g, int index, int rows) {
            const std::string_view act{ g.act };
            Family f = Family::kCount; RoleTag tag = RoleTag::None;
            if (act == V::kActAttack) f = Family::Attack;
            else if (act == V::kActPowerAttack) f = Family::PowerAttack;
            else if (act == V::kActTaunt) f = Family::Taunt;
            else if (act == V::kActFlee) f = Family::Retreat;
            else if (act == V::kActDrinkHealthPotion) f = Family::DrinkHealth;
            else if (act == V::kActDrinkStaminaPotion) f = Family::DrinkStamina;
            else if (act == V::kActDrinkMagickaPotion) f = Family::DrinkMagicka;
            else if (act == V::kActEquipMelee) { f = Family::Posture; tag = RoleTag::Melee; }
            else if (act == V::kActEquipRanged) { f = Family::Posture; tag = RoleTag::Ranged; }
            else if (act == V::kActEquipSilver) { f = Family::Posture; tag = RoleTag::Silver; }
            else if (act == V::kActCastSelf || act == V::kActCastTarget || act == V::kActCastPlayer) {
                using R = C::SpellRole;
                switch (g.role) {
                case R::Fire: f = Family::CastOffense; tag = RoleTag::Fire; break;
                case R::Frost: f = Family::CastOffense; tag = RoleTag::Frost; break;
                case R::Shock: f = Family::CastOffense; tag = RoleTag::Shock; break;
                case R::Attack: case R::Destruction: case R::Area: f = Family::CastOffense; break;
                case R::Heal: f = Family::CastHeal; tag = RoleTag::Heal; break;
                case R::Summon: case R::Reanimate: f = Family::CastSummon; tag = RoleTag::Summon; break;
                case R::Ward: f = Family::CastWard; tag = RoleTag::Ward; break;
                case R::Armor: case R::BoundWeapon: f = Family::CastBuff; tag = RoleTag::Buff; break;
                case R::Paralyze: case R::Fear: case R::Calm: case R::TurnUndead: f = Family::CastControl; tag = RoleTag::Control; break;
                default: f = Family::CastOffense; break;
                }
            }
            if (f != Family::kCount) BumpRow(p, f, tag, index, rows);
        }
    }

    bool ApplyCurveSpecs(const std::vector<std::string>& a_specs, const Tuning& a_shared, std::string& a_err) {
        namespace CC = MFO::Iaus::ClassCurves;
        auto lower = [](std::string x) { for (char& ch : x) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); return x; };
        auto index = [&](const std::string& tok, std::size_t n, auto nameOf) -> long {
            if (!tok.empty() && std::all_of(tok.begin(), tok.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) { const long v = std::atol(tok.c_str()); return static_cast<std::size_t>(v) < n ? v : -1; }
            for (std::size_t i = 0; i < n; ++i) if (lower(std::string(nameOf(i))) == lower(tok)) return static_cast<long>(i);
            return -1; };
        for (const std::string& sp : a_specs) {
            const auto colon = sp.find(':'), eq = sp.find('=');
            if (colon == std::string::npos || eq == std::string::npos || eq < colon) { a_err = "bad --curve '" + sp + "' (use <class>:<family>.<cons>.<param>=<v> or <class>:scalar.<name>=<v>)"; return false; }
            const std::string slug = sp.substr(0, colon), path = sp.substr(colon + 1, eq - colon - 1);
            float v = 0.0f; try { v = std::stof(sp.substr(eq + 1)); } catch (...) { a_err = "bad value in --curve '" + sp + "'"; return false; }
            long ci = BaseOffRow(slug); for (std::size_t i = 0; ci < 0 && i < C::kClassCount; ++i) if (C::kTable[i].slug == slug) ci = static_cast<long>(i);   // a tree-OFF base cell (melee-off ...) is its own row, kBaseOffFirst + root
            if (ci < 0) { a_err = "unknown class '" + slug + "' in --curve (see --print-classes)"; return false; }
            CC::Row& row = g_curveTable[static_cast<std::size_t>(ci)];
            if (path.rfind("scalar.", 0) == 0) {
                if (!CC::SetScalar(row, path.substr(7), v)) { a_err = "unknown scalar in --curve '" + sp + "' (drinkHealthLine | touchSpentSec)"; return false; }
                continue; }
            if (path.rfind("family.", 0) == 0) {   // <class>:family.<family>=<v>: the class's multiplier on a whole action family (Profile::family)
                const long ff = index(path.substr(7), MFO::Iaus::kFamilies, [](std::size_t i) { return MFO::Iaus::Name(static_cast<Family>(i)); });
                if (ff < 0 || !CC::SetFamilyWeight(row, static_cast<Family>(ff), v)) { a_err = "unknown family or bad weight in --curve '" + sp + "' (<class>:family.<family>=<v>, v > 0)"; return false; }
                continue; }
            const auto d1 = path.find('.'), d2 = d1 == std::string::npos ? d1 : path.find('.', d1 + 1);
            if (d2 == std::string::npos) { a_err = "bad --curve path '" + path + "' (<family>.<cons>.<param>)"; return false; }
            const long fi = index(path.substr(0, d1), MFO::Iaus::kFamilies, [](std::size_t i) { return MFO::Iaus::Name(static_cast<Family>(i)); });
            const long ki = index(path.substr(d1 + 1, d2 - d1 - 1), MFO::Iaus::kCons, [](std::size_t i) { return MFO::Iaus::Name(static_cast<Cons>(i)); });
            if (fi < 0 || ki < 0) { a_err = "unknown family or consideration in --curve '" + sp + "'"; return false; }
            if (path.substr(d2 + 1) == "w") {   // <class>:<family>.<cons>.w=<v>: the consideration's exponent for this class (Profile::weights)
                if (!CC::SetWeight(row, static_cast<Family>(fi), static_cast<Cons>(ki), v)) { a_err = "bad weight in --curve '" + sp + "' (v > 0)"; return false; }
                continue; }
            g_curveConsEdited.push_back(static_cast<std::size_t>(ki));   // sim.cpp refuses --opt c.<ki>.* next to it
            if (!CC::SetCurveParam(row, static_cast<Family>(fi), static_cast<Cons>(ki), a_shared.curve[static_cast<std::size_t>(ki)], path.substr(d2 + 1), v)) { a_err = "unknown curve param in --curve '" + sp + "' (a | b | floor | zeroAbove | zeroBelow | w | px<i> / py<i> on a Piece curve)"; return false; }
        }
        return true;
    }

    std::vector<SimClass> RealClasses(bool mechProfiles) {
        std::vector<SimClass> out;
        for (const C::ClassDef& d : C::kTable) {
            SimClass c; c.name = std::string(d.slug); c.tier = d.tier;
            const bool hybrid = d.line == C::Line::Hybrid;
            c.line = d.line == C::Line::Melee ? "Melee" : d.line == C::Line::Ranged ? "Ranged" : d.line == C::Line::Mage ? "Mage" : "Hybrid";
            c.fam = d.line == C::Line::Melee ? Cls::Melee : d.line == C::Line::Ranged ? Cls::Ranged : d.line == C::Line::Mage ? Cls::Mage
                  : d.rootT1 == 2 ? Cls::Ranged : d.rootT1 == 3 ? Cls::Mage : Cls::Melee;
            ClassGate& g = c.gate; g.resolved = true; g.treeOn = true; g.strict = false;
            g.spellsOnClass = Casting(d);
            const C::Mask mastered = Mastered(d);
            for (std::size_t i = 0; i < C::kClassCount; ++i) if ((mastered & (C::Mask{ 1 } << i)) && Casting(C::kTable[i])) { g.spellsFromMastered = true; break; }
            MFO::Iaus::ClassCurves::ApplyMastered(g_curveTable, mastered, g);   // SpellBook::GateFor: each mastered casting class bumps the weight at its home school (marth's fixed 0.15)
            g.scrollUser = g.spellsOnClass;
            const bool any = (d.weapons & C::Weapon::kUnrestricted) != 0;
            g.weaponMelee = any || (d.weapons & (C::Weapon::kDagger | C::Weapon::kOneHand | C::Weapon::kTwoHand)) != 0;
            g.weaponBow = any || (d.weapons & C::Weapon::kBow) != 0; g.weaponCrossbow = any || (d.weapons & C::Weapon::kCrossbow) != 0; g.weaponStaff = any || (d.weapons & C::Weapon::kStaff) != 0;
            g.disengage = (d.flags & C::Flag::kDisengage) != 0; c.shortLeash = (d.flags & C::Flag::kShortLeash) != 0;
            c.sneakAlways = (d.flags & C::Flag::kSneakAlways) != 0; g.sneakClass = c.sneakAlways;   /* SpellBook::GateFor: Flag::kSneakAlways with the tree on (card G3's hide run) */ c.nearPlayer = (d.flags & C::Flag::kNearPlayer) != 0; c.goIn = static_cast<int>(d.goIn); c.partyPct = d.partyHealthyPct;
            c.pureMage = d.line == C::Line::Mage;
            if (const Playstyle* ps = ActivePlaystyle(c.name)) { c.forbid = ps->forbid; c.playstyle = ps->does; }
            for (std::size_t i = 0; i < d.gambitCount; ++i) { using R = C::SpellRole; const char* tok = nullptr; const char* tok2 = nullptr;   // the roles his own rows name (todo rows too: the card names them)
                switch (d.gambits[i].role) { case R::Fire: tok = "fire"; break; case R::Frost: tok = "frost"; break; case R::Shock: tok = "shock"; break; case R::Heal: tok = "healOther"; tok2 = "heal"; break;
                    case R::Summon: tok = "summon"; break; case R::Armor: tok = "armor"; break; case R::Paralyze: tok = "paralyze"; break; case R::Fear: tok = "fear"; break; case R::Reanimate: if (g_t2.reanimate) tok = "reanimate"; break; default: break; }
                for (const char* t : { tok, tok2 }) if (t && std::find(c.ensure.begin(), c.ensure.end(), t) == c.ensure.end()) c.ensure.push_back(t); }
            // The stealth passives and packages the class cards rule (class-walkthrough-decisions.md Thief / Master Thief / Nightingale /
            // Listener; our own design data, at full strength = the class mastered). Thief: sneaking 30% quieter, a free stealth entry 1
            // fight in 5. Master Thief: sneak attacks x1.5, the flanking half bonus (G4), entry 1 in 3, the mastered Thief passive at 1/4.
            // The two T4s hold Master Thief's passive at full. CHOICE: they keep its 1-in-3 entry (their packages do not say).
            const bool inForce = PlaystyleInForce(c.name);   // a PARKED class keeps no stealth passive unless --parked
            if (!inForce) {}
            else if (d.id == C::Id::Thief) { c.entryOdds = 5; c.noiseClass = 0.70f; }
            else if (d.id == C::Id::MasterThief || d.id == C::Id::Nightingale || d.id == C::Id::Listener) { c.entryOdds = 3; c.sneakAtkClass = 1.5f; c.flank = true; c.noiseClass = 0.925f; c.cloak = d.id == C::Id::Nightingale; }
            { const auto& crow = g_curveTable[static_cast<std::size_t>(d.id)]; MFO::Iaus::ClassCurves::Apply(crow, static_cast<std::uint8_t>(d.id), g); c.profile = MFO::Iaus::ClassCurves::Build(crow); }   // per-class curves: this class's row (the shipped one unless --curve edited it)
            g.powerAttack = d.powerShareMax != C::kShareUnset || d.powerShareMin != C::kShareUnset;
            // T2 batch S: the package and claim data, straight from the class table (nothing copied by hand but the three values noted)
            c.shareMax = d.powerShareMax != C::kShareUnset ? d.powerShareMax : -1.0f; c.shareMin = d.powerShareMin != C::kShareUnset ? d.powerShareMin : -1.0f;
            c.leashScaled = d.tier >= 2 && (d.flags & (C::Flag::kShortLeash | C::Flag::kNearPlayer)) != 0;   // Classes::PackageLeashScale
            c.avoidThreat = d.id == C::Id::Knight || d.id == C::Id::GreatKnight || d.id == C::Id::EbonyWarrior ? 0.0f : d.id == C::Id::Mercenary ? 0.35f : d.id == C::Id::Blademaster ? 0.50f : d.id == C::Id::LivingLegend ? 0.55f : d.id == C::Id::Thief ? 0.25f : d.id == C::Id::MasterThief ? 0.30f : 0.20f;   // MFO_GenerateESP.py CLS_AVOID_THREAT / AVOID_THREAT_VANILLA (Q-S-3, marth 11-12), hand-mirrored; the ranged stance adds AVOID_THREAT_BOW_BOOST in Setup.h. Heavy body armor => 0 is modelled in Setup.h MfoPackage (the zero-avoid twin). NOT modelled: Living Legend's H8 auto-block rule (card not built; the lab already skips a dodge behind any guard)
            if (d.id == C::Id::Conjurer || d.id == C::Id::Healer) c.aversion = 0.25f;                         // Config fAversionConjurer / fAversionHealer (Q93), the EQUIPPED class (Q94), hand-mirrored
            // Q107 (marth 11-12): aversion comes ONLY from an equipped Conjurer / Healer; Dragonborn gives none (apmf/AversionClaims.cpp WeightFor)
            c.aimBudget = d.id == C::Id::Archer ? 5 : d.id == C::Id::Scout ? 4 : d.id == C::Id::Witchhunter ? 4 : d.id == C::Id::Ranger ? 3 : d.id == C::Id::Jaqspur ? 1 : -1;   // Config iAimBudget* defaults (apmf/AimClaims.cpp), hand-mirrored
            if (g_t2.backoff) g.backOffPressed = d.id == C::Id::Mercenary;                                    // SpellBook::GateFor (iaus/SpellBook.cpp:502)
            int applied = 0; for (std::size_t i = 0; i < d.gambitCount; ++i) if (d.gambits[i].act && d.gambits[i].IsApplied() && !d.gambits[i].pendingRuling) ++applied;
            int idx = 0;
            for (std::size_t i = 0; i < d.gambitCount; ++i) {
                const auto& row = d.gambits[i]; if (!row.act) continue;
                const std::string_view act{ row.act };
                if (act == V::kActPowerAttack) { g.powerAttack = true; g.powerCooldownSec = std::max(g.powerCooldownSec, row.cooldownSec); }
                else if (act == V::kActTaunt) { g.taunt = true; g.tauntCooldownSec = std::max(g.tauntCooldownSec, row.cooldownSec); }
                else if (act == V::kActBlock && g_t2.block) { g.block = true; g.blockCooldownSec = std::max(g.blockCooldownSec, row.cooldownSec); }   // SpellBook::GateFor (iaus/SpellBook.cpp:512; MFO-B827)
                if (row.IsApplied() && !row.pendingRuling) { if (mechProfiles) Bump(c.profile, row, idx, applied); ++idx; }
                if (row.IsApplied() && row.cond) { GRow gr; gr.cond = row.cond; gr.act = row.act; gr.param = row.condParam; gr.role = static_cast<int>(row.role); gr.cooldown = row.cooldownSec; gr.applied = !row.pendingRuling; c.rows.push_back(std::move(gr)); }
            }
            // the kit the class row lets him carry (unrestricted T1 rows keep today's kit by line)
            KitChoice& k = c.kit; k = KitChoice{}; k.oneHand = k.twoHand = k.dagger = k.bow = k.crossbow = k.staff = k.shield = k.heavy = k.light = false;
            if (any) { if (c.fam == Cls::Melee) { k.oneHand = true; k.heavy = true; } else if (c.fam == Cls::Ranged) { k.bow = true; k.dagger = true; k.light = true; } else { k.dagger = true; k.staff = true; } }
            else {
                k.oneHand = (d.weapons & C::Weapon::kOneHand) != 0; k.twoHand = (d.weapons & C::Weapon::kTwoHand) != 0 && !k.oneHand; k.dagger = (d.weapons & C::Weapon::kDagger) != 0;
                k.bow = (d.weapons & C::Weapon::kBow) != 0; k.crossbow = (d.weapons & C::Weapon::kCrossbow) != 0 && !k.bow; k.staff = (d.weapons & C::Weapon::kStaff) != 0; k.shield = (d.weapons & C::Weapon::kShield) != 0;
                k.heavy = c.fam == Cls::Melee; k.light = c.fam == Cls::Ranged;   // MODEL: armor weight by root line; mages wear cloth
            }
            // THE ARMOR OF THE CLASS CARDS (class-perks-packages.md: L2 Heavy Armor is owned by Knight and Warrior, L5 Light Armor by Mercenary,
            // Archer and Thief, L13 Arcane Robes by the mage line). The root-line model above put the MERCENARY line in heavy armor, against
            // its card, while the game takes the dodge away from anyone in a heavy cuirass (Setup.h MfoPackage): the Mercenary line wears LIGHT.
            // The line is read from the class's own Avoid Threat (CLS_AVOID_THREAT above): a melee class whose card gives it MORE than the
            // vanilla 0.20 is a dodging class (Mercenary 0.35, Blademaster 0.50, Living Legend 0.55), and a dodge in a heavy cuirass is dead.
            if (c.fam == Cls::Melee && c.avoidThreat > 0.20f) { k.heavy = false; k.light = true; }
            // T1 keeps TODAY'S KIT (marth 2026-11-09): the three combat rows every new follower gets, Followers::ApplyDefaultKit
            // (native/Followers.cpp:43-45). Hand-mirrored here (that file is engine code): drink at 30% health, attack the weakest foe, else wait.
            if (d.tier == 1) { c.rows.clear(); c.defaults = "default-kit";
                c.rows.push_back({ V::kCondSelfHpBelow, V::kActDrinkHealthPotion, 0.30f, 0, 0.0f, true });
                c.rows.push_back({ V::kCondFoeLowestHp, V::kActAttack, 0.0f, 0, 0.0f, true });
                c.rows.push_back({ V::kCondAlways, V::kActWait, 0.0f, 0, 0.0f, true }); }
            k.mageBlend = hybrid && c.fam != Cls::Mage ? 0.5f : 0.0f;   // MODEL: a hybrid's attributes and skills are half his school's
            out.push_back(std::move(c));
        }
        // THE TREE-OFF BASE CELLS (marth 11-12: "I dont see the 3 base classes with no tree in the sim. I imagine theyd say Line: Base ... They do
        // technically have weapon gates (mage knife etc), and should still loosely follow tree on versions. They just have all available options
        // at thier disposal"). Each is built FROM its tree-ON T1 row (same family, kit, default kit rows, known-spell tokens: the mage keeps
        // dagger + staff), then opened exactly as SpellBook::GateFor's tree-OFF branch opens it (iaus/SpellBook.cpp: any known spell, power
        // attack, taunt, retreat; scrollUser by the T1 root; no row cooldown, so Generate applies the Tuning rowless floors; the gate's weapon
        // categories open; no sneak class, no pressed back-off, no Block row). In the DLL the tree-OFF weapon choice is the stance's equip side,
        // not the IAUS gate: the lab mirrors it through the T1 kit. Curve row = ClassCurves::kBaseOffFirst + root (its OWN row). No class
        // package (tier 1) and no class flag: the style is MFO's stance style with the vanilla Avoid Threat 0.20, no kDisengage fallback offset.
        // APPENDED after the 35 table rows, so every table class keeps its index (and so its fights, draw for draw).
        static const char* const kOff[3] = { "melee-off", "ranged-off", "mage-off" };
        for (std::size_t i = 0; i < 3; ++i) {
            SimClass c = out[i]; const C::ClassDef& d = C::kTable[i];
            c.name = kOff[i]; c.line = "Base"; c.tier = 1; c.planned = false; c.playstyle.clear(); c.forbid = 0;
            ClassGate& g = c.gate; g = ClassGate{}; g.resolved = true; g.treeOn = false; g.strict = false;
            g.spellsOnClass = true; g.scrollUser = Casting(d); g.powerAttack = true; g.taunt = true; g.disengage = true;
            const std::size_t row = MFO::Iaus::ClassCurves::kBaseOffFirst + i;
            MFO::Iaus::ClassCurves::Apply(g_curveTable[row], static_cast<std::uint8_t>(row), g); c.profile = MFO::Iaus::ClassCurves::Build(g_curveTable[row]);
            c.csDisengage = 0; c.avoidThreat = 0.20f; c.shareMax = c.shareMin = -1.0f; c.leashScaled = false; c.aversion = 0.0f; c.aimBudget = -1;
            c.sneakAlways = c.flank = c.cloak = c.nearPlayer = c.shortLeash = false; c.entryOdds = 0; c.sneakAtkClass = c.noiseClass = 1.0f;
            out.push_back(std::move(c));
        }
        return out;
    }

    void PrintClassTable(const std::vector<SimClass>& v) {
        std::printf("classes from native/classes/ClassTable.cpp: %zu (+ the 3 tree-OFF base cells)\n", v.size() - 3);
        for (const SimClass& c : v)
            std::printf("  T%d %-16s %-6s spells=%d(mastered %d) power=%d(cd %.0f) taunt=%d disengage=%d  weapons m%d b%d x%d s%d  kit:%s%s%s%s%s%s%s\n", c.tier, c.name.c_str(),
                        c.fam == Cls::Melee ? "melee" : c.fam == Cls::Ranged ? "ranged" : "mage", int(c.gate.spellsOnClass), int(c.gate.spellsFromMastered), int(c.gate.powerAttack), c.gate.powerCooldownSec,
                        int(c.gate.taunt), int(c.gate.disengage), int(c.gate.weaponMelee), int(c.gate.weaponBow), int(c.gate.weaponCrossbow), int(c.gate.weaponStaff),
                        c.kit.oneHand ? " 1H" : "", c.kit.twoHand ? " 2H" : "", c.kit.dagger ? " dagger" : "", c.kit.bow ? " bow" : "", c.kit.crossbow ? " xbow" : "", c.kit.staff ? " staff" : "", c.kit.shield ? (c.kit.heavy ? " shield heavy" : c.kit.light ? " shield light" : " shield cloth") : (c.kit.heavy ? " heavy" : c.kit.light ? " light" : " cloth"));
    }

}
