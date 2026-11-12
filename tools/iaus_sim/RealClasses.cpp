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

#include <cstdio>

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
            g.powerAttack = d.powerShareMax != C::kShareUnset || d.powerShareMin != C::kShareUnset;
            // T2 batch S: the package and claim data, straight from the class table (nothing copied by hand but the three values noted)
            c.shareMax = d.powerShareMax != C::kShareUnset ? d.powerShareMax : -1.0f; c.shareMin = d.powerShareMin != C::kShareUnset ? d.powerShareMin : -1.0f;
            c.leashScaled = d.tier >= 2 && (d.flags & (C::Flag::kShortLeash | C::Flag::kNearPlayer)) != 0;   // Classes::PackageLeashScale
            if (d.id == C::Id::Mercenary) c.avoidThreat = 0.3f;                                               // MFO_GenerateESP.py CLS_AVOID_THREAT (Q-melee-7), hand-mirrored
            if (d.id == C::Id::Conjurer || d.id == C::Id::Healer) c.aversion = 0.25f;                         // Config fAversionConjurer / fAversionHealer (Q93), the EQUIPPED class (Q94), hand-mirrored
            else if (d.id == C::Id::Dragonborn && (Mastered(d) & (C::Mask{ 1 } << static_cast<std::size_t>(C::Id::Conjurer) | C::Mask{ 1 } << static_cast<std::size_t>(C::Id::Healer)))) c.aversion = 0.25f;   // Q107 default: Dragonborn holds a mastered passive at full
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
            // T1 keeps TODAY'S KIT (marth 2026-11-09): the three combat rows every new follower gets, Followers::ApplyDefaultKit
            // (native/Followers.cpp:43-45). Hand-mirrored here (that file is engine code): drink at 30% health, attack the weakest foe, else wait.
            if (d.tier == 1) { c.rows.clear(); c.defaults = "default-kit";
                c.rows.push_back({ V::kCondSelfHpBelow, V::kActDrinkHealthPotion, 0.30f, 0, 0.0f, true });
                c.rows.push_back({ V::kCondFoeLowestHp, V::kActAttack, 0.0f, 0, 0.0f, true });
                c.rows.push_back({ V::kCondAlways, V::kActWait, 0.0f, 0, 0.0f, true }); }
            k.mageBlend = hybrid && c.fam != Cls::Mage ? 0.5f : 0.0f;   // MODEL: a hybrid's attributes and skills are half his school's
            out.push_back(std::move(c));
        }
        return out;
    }

    void PrintClassTable(const std::vector<SimClass>& v) {
        std::printf("classes from native/classes/ClassTable.cpp: %zu\n", v.size());
        for (const SimClass& c : v)
            std::printf("  T%d %-16s %-6s spells=%d(mastered %d) power=%d(cd %.0f) taunt=%d disengage=%d  weapons m%d b%d x%d s%d  kit:%s%s%s%s%s%s%s\n", c.tier, c.name.c_str(),
                        c.fam == Cls::Melee ? "melee" : c.fam == Cls::Ranged ? "ranged" : "mage", int(c.gate.spellsOnClass), int(c.gate.spellsFromMastered), int(c.gate.powerAttack), c.gate.powerCooldownSec,
                        int(c.gate.taunt), int(c.gate.disengage), int(c.gate.weaponMelee), int(c.gate.weaponBow), int(c.gate.weaponCrossbow), int(c.gate.weaponStaff),
                        c.kit.oneHand ? " 1H" : "", c.kit.twoHand ? " 2H" : "", c.kit.dagger ? " dagger" : "", c.kit.bow ? " bow" : "", c.kit.crossbow ? " xbow" : "", c.kit.staff ? " staff" : "", c.kit.shield ? " shield" : "");
    }

}
