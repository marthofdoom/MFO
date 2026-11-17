#include "PCH.h"
#include "iaus/PerkEffects.h"

#include "MainThread.h"
#include "Vocabulary.h"                // Vocab::IsCastableSpell: the spells a follower casts (the SpellBook's own filter)
#include "iaus/Core.h"                 // ShippedTuning: the two gear-pick weights before the first tuning file load
#include "iaus/EffectMirror.h"         // the live effects on him (read in place on the main thread, INVARIANTS #84)
#include "logistics/InvSnapshot.h"     // his inventory (read in place on the main thread) + the worker-tick clock
#include "logistics/Logistics.h"       // WeaponKindBits: THE weapon-kind classifier the weapon pick scores with
#include "logistics/TradePerks.h"      // the guarded HandleEntryPoint (proven argument shapes, the verified seat)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>

// iaus/PerkEffects.cpp -- see PerkEffects.h. Design: Docs/research/2026-11/perk-effects.md.
namespace MFO::PerkEffects {

    static_assert(kEntryPoints == static_cast<std::size_t>(RE::BGSEntryPoint::ENTRY_POINT::kTotal),
                  "the engine's entry-point count moved: the census array is indexed by it");

    namespace {

        using EP = RE::BGSEntryPoint::ENTRY_POINT;
        using Iaus::PerkArmor;
        using Iaus::PerkWeapon;

        // Counted in worker ticks (133 ms, InvSnap's clock). Perks change on a level-up or a respec, the items asked about on a pickup: ~2 s is
        // far inside both, and one follower per pass keeps the main-thread cost of a pass to one build (measured: Table::buildMicros, `[perk-fx]`).
        constexpr std::uint64_t kRefreshTicks = 15;
        constexpr std::uint64_t kWantTtlTicks = 112;   // ~15 s since the party last named him: then his slot is forgotten (EffectMirror's rule)
        constexpr std::size_t   kMaxSpells    = 64;    // spells asked per build (three engine asks each); the rest are counted, not asked
        // A multiplier outside this band is a reader fault or a perk that sets an absolute value against our seed: not a multiplier we can weigh.
        constexpr float kMinMult = 0.0f, kMaxMult = 100.0f;

        struct Slot {
            Ptr           table;
            std::uint64_t builtTick = 0;    // 0 = never built
            std::uint64_t wantedTick = 0;
            bool          saidWaiting = false;   // the "cannot be evaluated now" line was said for the current wait
        };
        std::mutex                           g_mx;   // LEAF: no engine call, no log call, no other MFO lock under it
        std::unordered_map<RE::FormID, Slot> g_slots;

        std::atomic<float> g_armorWeight{ Iaus::ShippedTuning().perkArmorWeight };
        std::atomic<float> g_weaponWeight{ Iaus::ShippedTuning().perkWeaponWeight };

        // The load order's generic item per kind (Startup, main thread, written once before the first BuildPass; read on the main thread only).
        std::array<RE::TESObjectWEAP*, kWeaponKinds> g_genericWeapon{};
        std::array<RE::TESObjectARMO*, kArmorKinds>  g_genericArmor{};

        constexpr const char* kWeaponName[kWeaponKinds] = { "sword", "dagger", "war axe", "mace", "greatsword", "battleaxe", "warhammer", "bow", "crossbow" };
        constexpr const char* kArmorName[kArmorKinds]   = { "light", "heavy", "shield" };
        constexpr const char* kSchoolName[Iaus::kSchools] = { "destruction", "alteration", "restoration", "illusion", "conjuration" };
        static_assert(Iaus::kDestruction == 0 && Iaus::kAlteration == 1 && Iaus::kRestoration == 2 && Iaus::kIllusion == 3 && Iaus::kConjuration == 4,
                      "kSchoolName is indexed by the School identity");

        int KindIndex(std::uint16_t a_bits) {
            for (std::size_t i = 0; i < kWeaponKinds; ++i)
                if (a_bits & (1u << i)) return static_cast<int>(i);
            return -1;
        }

        // The school of a spell: the COSTLIEST effect's MGEF "Magic Skill" (the SpellBook / Loadout / Cast.cpp rule), as a School identity; -1 none.
        int SchoolOf(const RE::SpellItem* a_sp) {
            const auto* ce = a_sp ? a_sp->GetCostliestEffectItem() : nullptr;
            if (!ce || !ce->baseEffect) return -1;
            switch (ce->baseEffect->data.associatedSkill) {
            case RE::ActorValue::kDestruction: return Iaus::kDestruction;
            case RE::ActorValue::kAlteration:  return Iaus::kAlteration;
            case RE::ActorValue::kRestoration: return Iaus::kRestoration;
            case RE::ActorValue::kIllusion:    return Iaus::kIllusion;
            case RE::ActorValue::kConjuration: return Iaus::kConjuration;
            default:                           return -1;
            }
        }

        PerkArmor ArmorKindOf(const RE::TESObjectARMO* a_armo, bool& a_ok) {
            a_ok = true;
            if (a_armo->IsShield()) return PerkArmor::Shield;
            switch (a_armo->GetArmorType()) {
            case RE::BGSBipedObjectForm::ArmorType::kLightArmor: return PerkArmor::Light;
            case RE::BGSBipedObjectForm::ArmorType::kHeavyArmor: return PerkArmor::Heavy;
            default: a_ok = false; return PerkArmor::Light;   // clothing: no rating for a perk to multiply
            }
        }

        struct SpellVisitor final : RE::Actor::ForEachSpellVisitor {
            std::vector<RE::SpellItem*> out;
            RE::BSContainer::ForEachResult Visit(RE::SpellItem* a_spell) override {
                if (a_spell && std::find(out.begin(), out.end(), a_spell) == out.end()) out.push_back(a_spell);
                return RE::BSContainer::ForEachResult::kContinue;
            }
        };

        // value out / value in, or nullopt when the engine gave no answer or one that is not a multiplier. a_refused counts those.
        std::optional<float> Ratio(std::optional<float> a_out, float a_seed, std::uint16_t& a_refused) {
            if (!a_out || !(a_seed > 0.0f)) { ++a_refused; return std::nullopt; }
            const float r = *a_out / a_seed;
            if (!std::isfinite(r) || r < kMinMult || r > kMaxMult) { ++a_refused; return std::nullopt; }
            return r;
        }

        // ── the census: record data only ─────────────────────────────────────────────────────────────────
        // His perks are the base's perk array (a follower's perks live on the TESNPC: Progression.cpp OwnsExactPerk). Each perk's entries by
        // type; an entry point entry is counted under its entry point id; an ability entry's spell is collected for the live-effect read.
        void Census(RE::Actor* a_actor, Table& a_t, std::vector<const RE::MagicItem*>& a_abilities) {
            auto* base = a_actor->GetActorBase();
            if (!base || !base->perks) return;
            for (std::uint32_t i = 0; i < base->perkCount; ++i) {
                auto* perk = base->perks[i].perk;
                if (!perk) continue;
                ++a_t.perks;
                for (auto* entry : perk->perkEntries) {
                    if (!entry) continue;
                    const int type = static_cast<int>(entry->GetType());   // int compares: the Progression::WalkPerkEntries idiom
                    if (type == static_cast<int>(RE::PERK_ENTRY_TYPE::kEntryPoint)) {
                        ++a_t.entryPointEntries;
                        const auto ep = static_cast<std::size_t>(static_cast<RE::BGSEntryPointPerkEntry*>(entry)->entryData.entryPoint.get());
                        if (ep < kEntryPoints) ++a_t.entryCount[ep];
                    } else if (type == static_cast<int>(RE::PERK_ENTRY_TYPE::kAbility)) {
                        ++a_t.abilityEntries;
                        if (const auto* ab = static_cast<RE::BGSAbilityPerkEntry*>(entry)->ability) a_abilities.push_back(ab);
                    } else if (type == static_cast<int>(RE::PERK_ENTRY_TYPE::kQuest)) {
                        ++a_t.questEntries;
                    }
                }
            }
        }

        // The actor values his perk abilities change on him NOW: the live effects (not dispelled, not inactive: the engine has already run each
        // effect's conditions) whose source spell is one of those abilities and whose archetype modifies a value.
        void AbilityValues(RE::Actor* a_actor, const std::vector<const RE::MagicItem*>& a_abilities, Table& a_t) {
            if (a_abilities.empty()) return;
            const auto fx = EffectMirror::Get(a_actor);   // main thread: read in place (INVARIANTS #84 rule 5)
            if (!fx || !fx->hasList) return;
            for (const auto& f : fx->fx) {
                if (!f.Live() || !f.base || !f.spell) continue;
                if (std::find(a_abilities.begin(), a_abilities.end(), f.spell) == a_abilities.end()) continue;
                switch (f.base->GetArchetype()) {
                case RE::EffectArchetypes::ArchetypeID::kValueModifier:
                case RE::EffectArchetypes::ArchetypeID::kPeakValueModifier:
                case RE::EffectArchetypes::ArchetypeID::kDualValueModifier:
                    break;
                default: continue;
                }
                const int av = static_cast<int>(f.base->data.primaryAV);
                if (av < 0 || !std::isfinite(f.magnitude)) continue;
                const float mag = f.base->IsDetrimental() ? -f.magnitude : f.magnitude;
                auto it = std::find_if(a_t.abilityAv.begin(), a_t.abilityAv.end(), [av](const auto& p) { return p.first == av; });
                if (it == a_t.abilityAv.end()) a_t.abilityAv.emplace_back(av, mag);
                else it->second += mag;
            }
            std::sort(a_t.abilityAv.begin(), a_t.abilityAv.end());
        }

        // ── the engine's answers ───────────────────────────────────────────────────────────────────────────────
        // WEAPONS: entry point 35 ModAttackDamage (owner, weapon, target). Asked once per fine kind, about the best weapon of the kind he carries,
        // else about the generic one. The target is HIMSELF: the engine's own display read (PlayerCharacter::GetDamage, id 40291 / 39215) asks
        // with the perk owner as the target, so a bonus conditioned on the victim counts only when it would hold against a body like his own.
        void Weapons(RE::Actor* a_actor, const InvSnap::View& a_inv, Table& a_t) {
            std::array<RE::TESObjectWEAP*, kWeaponKinds> own{};
            for (const auto& [obj, data] : a_inv) {
                if (!obj || data.first <= 0) continue;
                auto* w = obj->As<RE::TESObjectWEAP>();
                if (!w || !w->GetPlayable()) continue;
                const int k = KindIndex(Logistics::WeaponKindBits(w));
                if (k < 0) continue;
                auto*& best = own[static_cast<std::size_t>(k)];
                if (!best || w->GetAttackDamage() > best->GetAttackDamage() ||
                    (w->GetAttackDamage() == best->GetAttackDamage() && w->GetFormID() < best->GetFormID()))
                    best = w;
            }
            for (std::size_t k = 0; k < kWeaponKinds; ++k) {
                auto* w = own[k] ? own[k] : g_genericWeapon[k];
                if (!w) continue;
                const float seed = (std::max)(1.0f, static_cast<float>(w->GetAttackDamage()));
                const auto  r = Ratio(Logistics::TradePerks::EntryPointValue(EP::kModAttackDamage, a_actor, w, a_actor, seed), seed, a_t.refused);
                if (!r) continue;
                a_t.weaponKind[k]  = *r;
                a_t.weaponForm[k]  = w->GetFormID();
                a_t.weaponProbe[k] = own[k] ? Probe::Own : Probe::Generic;
            }
            // The classes the posture layer reads: the best kind of the class among the kinds he CARRIES, else among the generic answers.
            const auto cls = [&](std::size_t a_from, std::size_t a_to) {
                float ownBest = 0.0f, anyBest = 0.0f;
                for (std::size_t k = a_from; k <= a_to; ++k) {
                    if (a_t.weaponProbe[k] == Probe::None) continue;
                    anyBest = (std::max)(anyBest, a_t.weaponKind[k]);
                    if (a_t.weaponProbe[k] == Probe::Own) ownBest = (std::max)(ownBest, a_t.weaponKind[k]);
                }
                return ownBest > 0.0f ? ownBest : anyBest > 0.0f ? anyBest : 1.0f;
            };
            auto& wd = a_t.read.weaponDamage;
            wd[static_cast<std::size_t>(PerkWeapon::OneHand)]  = cls(0, 3);
            wd[static_cast<std::size_t>(PerkWeapon::TwoHand)]  = cls(4, 6);
            wd[static_cast<std::size_t>(PerkWeapon::Bow)]      = cls(7, 7);
            wd[static_cast<std::size_t>(PerkWeapon::Crossbow)] = cls(8, 8);
            a_t.read.read = true;
        }

        // ARMOR: entry point 85 ModArmorRating (owner, armor), seeded with the piece's rating as the engine seeds it (its two call sites add the
        // result into the actor's total). Per type the MEAN over the rated pieces of the type he carries, else the generic piece.
        void Armor(RE::Actor* a_actor, const InvSnap::View& a_inv, Table& a_t) {
            std::array<float, kArmorKinds> sum{};
            std::array<int, kArmorKinds>   n{};
            const auto ask = [&](RE::TESObjectARMO* a_armo, std::size_t a_kind) {
                const float seed = a_armo->GetArmorRating();
                if (!(seed > 0.0f)) return false;
                const auto r = Ratio(Logistics::TradePerks::EntryPointValue(EP::kModArmorRating, a_actor, a_armo, seed), seed, a_t.refused);
                if (!r) return false;
                sum[a_kind] += *r; ++n[a_kind];
                return true;
            };
            for (const auto& [obj, data] : a_inv) {
                if (!obj || data.first <= 0) continue;
                auto* a = obj->As<RE::TESObjectARMO>();
                if (!a || !a->GetPlayable()) continue;
                bool ok = false;
                const auto kind = static_cast<std::size_t>(ArmorKindOf(a, ok));
                if (ok && n[kind] < 255) ask(a, kind);
            }
            for (std::size_t k = 0; k < kArmorKinds; ++k) {
                if (n[k] > 0) a_t.armorProbe[k] = Probe::Own;
                else if (g_genericArmor[k] && ask(g_genericArmor[k], k)) a_t.armorProbe[k] = Probe::Generic;
                if (n[k] > 0) a_t.read.armorRating[k] = sum[k] / static_cast<float>(n[k]);
                a_t.armorPieces[k] = static_cast<std::uint8_t>((std::min)(n[k], 255));
            }
            a_t.read.armorRead = true;
        }

        // SPELLS: per spell he knows, entry point 38 ModSpellCost (owner, spell) on a seed of 100, entry point 29 ModSpellMagnitude (owner, spell,
        // target = himself) on the costliest effect's magnitude, and entry point 75 CanDualCastSpell (owner, spell; the engine's seed is 0 and any
        // other value is "yes"). Per school the MEAN of the spells of that school.
        void Spells(RE::Actor* a_actor, Table& a_t) {
            SpellVisitor vis;
            a_actor->VisitSpells(vis);
            std::array<float, Iaus::kSchools> cost{}, mag{};
            std::array<int, Iaus::kSchools>   nc{}, nm{}, asked{};
            for (auto* sp : vis.out) {
                if (!Vocab::IsCastableSpell(sp) || sp->IsDynamicForm()) continue;   // abilities / powers, and runtime-minted proxies he does not KNOW
                ++a_t.spellsKnown;
                const int school = SchoolOf(sp);
                if (school < 0) continue;
                if (a_t.spellsAsked >= kMaxSpells) continue;
                ++a_t.spellsAsked;
                const auto s = static_cast<std::size_t>(school);
                ++asked[s];
                if (const auto r = Ratio(Logistics::TradePerks::EntryPointValue(EP::kModSpellCost, a_actor, sp, 100.0f), 100.0f, a_t.refused)) {
                    cost[s] += *r; ++nc[s];
                }
                const auto* ce    = sp->GetCostliestEffectItem();
                const float mag0  = ce ? ce->GetMagnitude() : 0.0f;
                const float seedM = std::isfinite(mag0) && mag0 > 0.0f ? mag0 : 100.0f;
                if (const auto r = Ratio(Logistics::TradePerks::EntryPointValue(EP::kModSpellMagnitude, a_actor, sp, a_actor, seedM), seedM, a_t.refused)) {
                    mag[s] += *r; ++nm[s];
                }
                if (const auto d = Logistics::TradePerks::EntryPointValue(EP::kCanDualCastSpell, a_actor, sp, 0.0f)) {
                    if (std::isfinite(*d) && *d != 0.0f) a_t.read.dualCast[s] = true;
                } else {
                    ++a_t.refused;
                }
            }
            for (std::size_t s = 0; s < Iaus::kSchools; ++s) {
                // A school counts as asked only when BOTH means exist: the edge is their quotient.
                if (nc[s] > 0 && nm[s] > 0) {
                    a_t.read.spellCost[s]      = cost[s] / static_cast<float>(nc[s]);
                    a_t.read.spellMagnitude[s] = mag[s] / static_cast<float>(nm[s]);
                    a_t.read.spellCount[s]     = static_cast<std::uint8_t>((std::min)(asked[s], 255));
                }
            }
            a_t.read.spellsRead = true;
        }

        // BLOCK: entry point 39 ModPercentBlocked (owner), seeded 1.0 (the engine seeds the share of the blow his guard stops; a multiply entry
        // reads the same on either seed).
        void Block(RE::Actor* a_actor, Table& a_t) {
            if (const auto r = Ratio(Logistics::TradePerks::EntryPointValue(EP::kModPercentBlocked, a_actor, 1.0f), 1.0f, a_t.refused)) {
                a_t.read.blockPct  = *r;
                a_t.read.blockRead = true;
            }
        }

        const char* ProbeTag(Probe a_p) { return a_p == Probe::Own ? "own" : a_p == Probe::Generic ? "generic" : "-"; }

        float Weighted(float a_mult, float a_weight) {
            if (!std::isfinite(a_mult) || !std::isfinite(a_weight) || a_weight <= 0.0f) return 1.0f;
            return (std::max)(0.1f, 1.0f + a_weight * (a_mult - 1.0f));   // never zero or negative: a factor multiplies a score, it never vetoes
        }

        // Lowest FormID wins, so the pick is the same on every run of one load order (no list-specific data: it is whatever the order holds).
        template <class T>
        void Lower(T*& a_slot, T* a_form) {
            if (!a_slot || a_form->GetFormID() < a_slot->GetFormID()) a_slot = a_form;
        }

    }

    std::string Describe(const Table& a_t, std::string_view a_name) {
        std::string s = std::format("{} ({:08X}): {} perk(s): {} entry point entr{}, {} abilit{}, {} quest", a_name, a_t.actor, a_t.perks,
                                    a_t.entryPointEntries, a_t.entryPointEntries == 1 ? "y" : "ies", a_t.abilityEntries,
                                    a_t.abilityEntries == 1 ? "y" : "ies", a_t.questEntries);
        s += " | weapon damage x";
        for (std::size_t k = 0; k < kWeaponKinds; ++k)
            if (a_t.weaponProbe[k] != Probe::None)
                s += std::format(" {} {:.2f} ({} {:08X})", kWeaponName[k], a_t.weaponKind[k], ProbeTag(a_t.weaponProbe[k]), a_t.weaponForm[k]);
        const auto& wd = a_t.read.weaponDamage;
        s += std::format(" -> posture one-hand {:.2f} two-hand {:.2f} bow {:.2f} crossbow {:.2f}", wd[0], wd[1], wd[2], wd[3]);
        s += " | armor rating x";
        for (std::size_t k = 0; k < kArmorKinds; ++k)
            if (a_t.armorProbe[k] != Probe::None)
                s += std::format(" {} {:.2f} ({}, {} piece(s))", kArmorName[k], a_t.read.armorRating[k], ProbeTag(a_t.armorProbe[k]), a_t.armorPieces[k]);
        s += std::format(" | spells ({} of {} known asked) cost x / magnitude x:", a_t.spellsAsked, a_t.spellsKnown);
        bool any = false;
        for (std::size_t i = 0; i < Iaus::kSchools; ++i) {
            if (a_t.read.spellCount[i] == 0) continue;
            any = true;
            s += std::format(" {} {:.2f} / {:.2f} ({} spell(s), edge {:.2f}{})", kSchoolName[i], a_t.read.spellCost[i], a_t.read.spellMagnitude[i],
                             a_t.read.spellCount[i], Iaus::PerkSpellEdge(a_t.read, static_cast<int>(i)), a_t.read.dualCast[i] ? ", dual cast" : "");
        }
        if (!any) s += " none";
        s += a_t.read.blockRead ? std::format(" | block x{:.2f}", a_t.read.blockPct) : std::string(" | block not read");
        if (!a_t.abilityAv.empty()) {
            s += " | perk ability actor values (index:sum)";
            for (const auto& [av, m] : a_t.abilityAv) s += std::format(" {}:{:+.1f}", av, m);
        }
        s += " | entries by entry point";
        any = false;
        for (std::size_t e = 0; e < kEntryPoints; ++e)
            if (a_t.entryCount[e] > 0) { any = true; s += std::format(" {}x{}", e, a_t.entryCount[e]); }
        if (!any) s += " none";
        if (a_t.refused > 0) s += std::format(" | {} engine ask(s) gave no usable answer (their multiplier stays 1.00)", a_t.refused);
        return s;
    }

    Ptr BuildLive(RE::Actor* a_actor) {
        if (!a_actor || !Logistics::TradePerks::EntryPointEvaluable(a_actor)) return nullptr;
        const auto t0 = std::chrono::steady_clock::now();
        auto t   = std::make_shared<Table>();
        t->actor = a_actor->GetFormID();

        std::vector<const RE::MagicItem*> abilities;
        Census(a_actor, *t, abilities);
        AbilityValues(a_actor, abilities, *t);

        const auto inv = InvSnap::Items(a_actor);   // main thread: read in place (INVARIANTS #84 rule 5); an empty view asks about the generic items
        Weapons(a_actor, inv, *t);
        Armor(a_actor, inv, *t);
        Spells(a_actor, *t);
        Block(a_actor, *t);

        t->text        = Describe(*t, a_actor->GetName() ? a_actor->GetName() : "?");
        t->buildMicros = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
        return t;
    }

    Ptr Get(RE::FormID a_actor) {
        if (!a_actor) return nullptr;
        const auto now = InvSnap::CurrentTick();
        std::scoped_lock lk(g_mx);
        auto& s = g_slots[a_actor];
        s.wantedTick = now;
        return s.table;
    }

    void Want(RE::FormID a_actor) {
        if (!a_actor || !MainThread::IsInstalled()) return;   // no pump: nothing would ever build it (the documented VR degrade: not known)
        const auto now = InvSnap::CurrentTick();
        std::scoped_lock lk(g_mx);
        g_slots[a_actor].wantedTick = now;
    }

    void SetWeights(float a_armor, float a_weapon) {
        g_armorWeight.store(a_armor, std::memory_order_relaxed);
        g_weaponWeight.store(a_weapon, std::memory_order_relaxed);
    }

    float ArmorFactor(RE::FormID a_actor, Iaus::PerkArmor a_kind) {
        const float w = g_armorWeight.load(std::memory_order_relaxed);
        if (!(w > 0.0f)) return 1.0f;
        const auto t = Get(a_actor);
        const auto k = static_cast<std::size_t>(a_kind);
        if (!t || !t->read.armorRead || k >= kArmorKinds || t->armorProbe[k] == Probe::None) return 1.0f;
        return Weighted(t->read.armorRating[k], w);
    }

    std::array<float, kWeaponKinds> WeaponFactors(RE::FormID a_actor) {
        std::array<float, kWeaponKinds> out;
        out.fill(1.0f);
        const float w = g_weaponWeight.load(std::memory_order_relaxed);
        if (!(w > 0.0f)) return out;
        const auto t = Get(a_actor);
        if (!t || !t->read.read) return out;
        for (std::size_t k = 0; k < kWeaponKinds; ++k)
            if (t->weaponProbe[k] != Probe::None) out[k] = Weighted(t->weaponKind[k], w);
        return out;
    }

    // MAIN THREAD.
    void Startup() {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (dh) {
            for (auto* w : dh->GetFormArray<RE::TESObjectWEAP>()) {
                if (!w || !w->GetPlayable() || w->IsBound() || w->formEnchanting || w->templateWeapon || w->GetAttackDamage() == 0) continue;
                const int k = KindIndex(Logistics::WeaponKindBits(w));
                if (k >= 0) Lower(g_genericWeapon[static_cast<std::size_t>(k)], w);
            }
            for (auto* a : dh->GetFormArray<RE::TESObjectARMO>()) {
                if (!a || !a->GetPlayable() || a->formEnchanting || a->templateArmor || !(a->GetArmorRating() > 0.0f)) continue;
                bool ok = false;
                const auto kind = ArmorKindOf(a, ok);
                if (!ok) continue;
                // A body piece for light / heavy (the piece every armor perk is written for); any shield for the shield kind.
                if (kind != PerkArmor::Shield && !a->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kBody)) continue;
                Lower(g_genericArmor[static_cast<std::size_t>(kind)], a);
            }
        }
        std::string probes;
        for (std::size_t k = 0; k < kWeaponKinds; ++k)
            probes += g_genericWeapon[k] ? std::format(" {}={:08X}", kWeaponName[k], g_genericWeapon[k]->GetFormID()) : std::format(" {}=none", kWeaponName[k]);
        for (std::size_t k = 0; k < kArmorKinds; ++k)
            probes += g_genericArmor[k] ? std::format(" {}={:08X}", kArmorName[k], g_genericArmor[k]->GetFormID()) : std::format(" {}=none", kArmorName[k]);
        const bool seat = Logistics::TradePerks::EntryPointSeatVerified();
        if (seat)
            spdlog::info("[perk-fx] perk-effect reader ON: the engine is asked per entry point (35 weapon damage, 85 armor rating, 38 spell cost, 29 spell "
                         "magnitude, 75 dual cast, 39 block) on the main thread, one follower per pass, each every ~{:.0f} s | gear-pick weights armor {:.2f} "
                         "weapon {:.2f} (t.perkArmor / t.perkWeapon) | generic items asked about when he carries none of a kind:{}",
                         static_cast<double>(kRefreshTicks) * 0.133, g_armorWeight.load(std::memory_order_relaxed),
                         g_weaponWeight.load(std::memory_order_relaxed), probes);
        else
            spdlog::error("[perk-fx] perk-effect reader OFF: the HandleEntryPoint row did not verify on this runtime (the seat's own refusal line says why). "
                          "No table is built: every perk consideration is not applicable and the gear picks weigh no perk. Nothing is guessed.");
    }

    // MAIN THREAD.
    void BuildPass(std::uint64_t) {
        if (!Logistics::TradePerks::EntryPointSeatVerified()) return;
        const auto now = InvSnap::CurrentTick();
        RE::FormID pick = 0;
        {
            std::uint64_t oldest = 0;
            bool          have = false;
            std::scoped_lock lk(g_mx);
            for (auto it = g_slots.begin(); it != g_slots.end();) {
                const Slot& s = it->second;
                if (s.wantedTick == 0 || now > s.wantedTick + kWantTtlTicks) { it = g_slots.erase(it); continue; }
                const bool due = s.builtTick == 0 || now >= s.builtTick + kRefreshTicks;
                if (due && (!have || s.builtTick < oldest || (s.builtTick == oldest && it->first < pick))) {
                    have = true; oldest = s.builtTick; pick = it->first;
                }
                ++it;
            }
        }
        if (!pick) return;

        auto* actor = RE::TESForm::LookupByID<RE::Actor>(pick);
        Ptr   built = actor ? BuildLive(actor) : nullptr;

        bool        changed = false, sayWaiting = false;
        std::string was;
        {
            std::scoped_lock lk(g_mx);
            auto it = g_slots.find(pick);
            if (it == g_slots.end()) return;   // Clear ran between the pick and here
            Slot& s = it->second;
            s.builtTick = (std::max)(now, std::uint64_t{ 1 });   // the attempt counts: the next follower gets the next pass either way
            if (built) {
                changed = !s.table || s.table->text != built->text;
                s.table = built;
                s.saidWaiting = false;
            } else if (!s.saidWaiting) {
                s.saidWaiting = sayWaiting = true;
            }
        }
        // Logged outside the leaf mutex. ONE line per follower per CHANGE of the table (the text is the signature).
        if (built && changed)
            spdlog::info("[perk-fx] {} | built in {} us", built->text, built->buildMicros);
        else if (sayWaiting)
            spdlog::info("[perk-fx] {:08X}: perks cannot be evaluated now ({}); {} (said once per wait)", pick,
                         !actor ? "the actor does not resolve" : "no middle-high process or perk data: he is not loaded",
                         "his last table stands if he has one, else his perks are not known and weigh nothing");
    }

    void Clear() {
        std::scoped_lock lk(g_mx);
        g_slots.clear();
    }

}
