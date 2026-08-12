#include "PCH.h"
#include "CombatStyle.h"
#include "CasterConsent.h"   // the equip gate exempts the latched gambit spell (#75)
#include "Config.h"
#include "Forms.h"

namespace MFO::CombatStyle {

    namespace {

        // combatStyle sits at 0x38, BELOW the 0x68 AE layout divergence (the
        // pinned NG header guards its AE-only BSSpinLock on a macro NG never
        // defines, so everything past 0x68 is +8 at runtime -- §0.29). 0x38 is
        // identical on SE and AE. These asserts break the build the day that
        // stops being true; the write below is a single aligned pointer store,
        // atomic on x64, through the live controller the engine just handed us.
        static_assert(offsetof(RE::CombatController, combatStyle) == 0x38,
                      "CombatController::combatStyle moved -- re-verify against the "
                      "pinned header (ENGINE_NOTES §0.29) before shipping");
        static_assert(offsetof(RE::CombatController, combatStyle) < 0x68,
                      "combatStyle is past the AE layout divergence point (0x68) "
                      "-- its compiled offset is WRONG on AE runtimes");

        struct Owned {
            Stance                stance   = Stance::None;   // what the gambit wants
            Stance                applied  = Stance::None;   // what we last wrote (handoff detect)
            RE::CombatController*  cc       = nullptr;        // identity ONLY -- never dereferenced
            RE::TESCombatStyle*    saved    = nullptr;        // engine-derived style this fight
            std::uint32_t          rederives = 0;             // engine re-derived under us, times
            bool                   equipOrder = false;        // #75: stance came from an EQUIP gambit
        };

        // Written by Want/Clear (worker) AND ApplyTick (combat thread), so a
        // real lock -- a plain mutex, taken only when AnyActive() (the atomic
        // mirror) already says a stance is live, so a non-combat / no-stance
        // tick never touches it.
        std::mutex g_mx;
        std::unordered_map<RE::FormID, Owned> g_owned;
        std::atomic<std::size_t> g_count{ 0 };

        // #75: how many owned stances are EQUIP ORDERS. Relaxed-atomic mirror
        // so the equip-gate thunk's fast-out never takes g_mx while no order
        // is live -- the same shape as g_count / CasterConsent's g_wantCount.
        // Recomputed under g_mx on every mutation; the map is party-sized.
        std::atomic<std::size_t> g_equipOrders{ 0 };
        void RecountEquipOrders() {   // g_mx held by the caller
            std::size_t n = 0;
            for (const auto& [fid, o] : g_owned)
                if (o.equipOrder) ++n;
            g_equipOrders.store(n, std::memory_order_relaxed);
        }

        RE::TESCombatStyle* StyleFor(Stance a_stance) {
            switch (a_stance) {
                case Stance::Melee:  return Forms::g_meleeStyle;
                case Stance::Ranged: return Forms::g_rangedStyle;
                case Stance::Cast:   return Forms::g_castStyle;
                default:             return nullptr;
            }
        }

        const char* Name(Stance a_stance) {
            switch (a_stance) {
                case Stance::Melee:  return "melee";
                case Stance::Ranged: return "ranged";
                case Stance::Cast:   return "cast";
                default:             return "none";
            }
        }

    }

    bool        AnyActive()  { return g_count.load(std::memory_order_relaxed) != 0; }
    std::size_t OwnedCount() { return g_count.load(std::memory_order_relaxed); }

    void Want(RE::FormID a_follower, Stance a_stance, bool a_equipOrder) {
        if (a_stance == Stance::None) return;
        // NOTE: the FEATURE gate lives at the call site now, not here -- weapon
        // stances (Melee/Ranged) are gated by bWeaponStyleControl, the Cast
        // stance by iCastControl. Want only records intent.
        std::lock_guard<std::mutex> lk(g_mx);
        // Only the desire is set here; cc/saved are filled in on the combat
        // thread by ApplyTick against the live controller. A stance CHANGE on an
        // existing entry (melee<->ranged) keeps cc/saved so the next ApplyTick
        // is a handoff, not a re-baseline.
        auto& o = g_owned[a_follower];
        o.stance = a_stance;
        // #75: only a WEAPON stance can be an equip order, and a later Want
        // that is not one (a cast latch flipping the stance to Cast, the class
        // override) RELEASES the hold -- the flag always tracks the most
        // recent real signal, never latches on its own.
        o.equipOrder = a_equipOrder && (a_stance == Stance::Melee || a_stance == Stance::Ranged);
        g_count.store(g_owned.size(), std::memory_order_relaxed);
        RecountEquipOrders();
    }

    void Clear(RE::FormID a_follower) {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_owned.erase(a_follower) != 0) {
            g_count.store(g_owned.size(), std::memory_order_relaxed);
            RecountEquipOrders();
        }
    }

    void ClearAll() {
        std::lock_guard<std::mutex> lk(g_mx);
        g_owned.clear();
        g_count.store(0, std::memory_order_relaxed);
        g_equipOrders.store(0, std::memory_order_relaxed);
    }

    void ApplyTick(RE::Actor* a_actor, RE::CombatController* a_cc) {
        if (!a_actor || !a_cc) return;

        // The P1 cast-style probe (CasterConsent, bProbeCastStyle -- dev-only,
        // default OFF) also writes cc->combatStyle, from the caster thunk. While
        // it is armed, defer entirely: two features re-asserting DIFFERENT CSTYs
        // onto the same field every tick would ping-pong and spam both logs. The
        // dev running that experiment wants the probe's style, not ours. In the
        // shipping default (probe off) this branch is never taken.
        if (Config::g_probeCastStyle.load(std::memory_order_relaxed)) return;

        const auto fid = a_actor->GetFormID();

        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_owned.find(fid);
        if (it == g_owned.end()) return;
        auto& o = it->second;

        RE::TESCombatStyle* target = StyleFor(o.stance);
        if (!target) return;   // form missing -> stance disabled, silent per the "one feature" rule

        // A DIFFERENT controller than we last swapped means the fight that swap
        // lived in is over -- its controller (and our swap) died with it, and
        // the engine re-derived this fresh one from the base record. Re-baseline
        // against the LIVE controller: capture its engine-derived style as the
        // restore point, then own it. Identity compare only; the stored pointer
        // is NEVER dereferenced.
        if (o.cc != a_cc) {
            o.cc        = a_cc;
            o.saved     = a_cc->combatStyle;
            o.rederives = 0;
            o.applied   = o.stance;
            a_cc->combatStyle = target;
            spdlog::info("[wstyle] {:08X} {}: combat style OWNED {:08X} -> {:08X} ({} stance)",
                         fid, a_actor->GetName() ? a_actor->GetName() : "?",
                         o.saved ? o.saved->GetFormID() : 0,
                         target->GetFormID(), Name(o.stance));
            return;
        }

        // Same fight. Two reasons the live style can differ from our target:
        //  * a melee<->ranged HANDOFF (the winning gambit flipped) -- expected,
        //    log it once at the transition;
        //  * the engine RE-DERIVED the style under us mid-combat -- re-assert so
        //    the follower stays in the gambit's stance (the cast probe proved
        //    the engine does this; count it).
        if (o.applied != o.stance) {
            a_cc->combatStyle = target;
            o.applied = o.stance;
            spdlog::info("[wstyle] {:08X}: stance HANDOFF -> {:08X} ({})",
                         fid, target->GetFormID(), Name(o.stance));
        } else if (a_cc->combatStyle != target) {
            // The engine re-derived the base style under us mid-combat (the cast
            // probe proved it does). Re-assert EVERY tick, but LOG ONLY THE
            // FIRST -- this is a default-ON feature and a persistent re-derive
            // would otherwise write one MFO.log line per combat tick. The first
            // line proves it happens; the count rides the state report.
            ++o.rederives;
            a_cc->combatStyle = target;
            if (o.rederives == 1)
                spdlog::info("[wstyle] {:08X}: engine RE-DERIVED the style under the swap "
                             "-- re-asserting {} ({}) each tick (silenced after first)",
                             fid, target->GetFormID(), Name(o.stance));
        }
    }

    // ── THE EQUIP GATE (#75) ─────────────────────────────────────────────────
    // See the header for the why: the CSTY swap is a score bias (MFO_MeleeStyle
    // starves magic at 0.1x, it does not forbid it), and the engine re-derives
    // the style mid-combat, so a spell-heavy caster still re-arms a spell over
    // the forced weapon and MFO's equip rule fires again next tick -- the deck's
    // Ebony-Tanto thrash. This is the prohibition: CheckShouldEquip (vtable
    // 0x0F) is the combat AI's per-item "should I put this in my hands?"
    // permission bit, and while an equip order owns the stance the gate answers
    // NO for spell/staff items. Same influence-not-insertion shape, same
    // write_vfunc technique, and the same actor resolution as CasterConsent's
    // CheckStartCast thunk one layer up.

    namespace {

        // The gate reads exactly ONE CombatController member -- attackerHandle
        // (0x28) -- to name the deliberating actor, under the §0.29 layout rule
        // (never touch anything at/after the 0x68 AE divergence). These asserts
        // break the build the day the pinned header moves it.
        static_assert(offsetof(RE::CombatController, attackerHandle) == 0x28,
                      "CombatController::attackerHandle moved -- re-verify the "
                      "SE/AE layout split (ENGINE_NOTES §0.29) before shipping");
        static_assert(offsetof(RE::CombatController, attackerHandle) < 0x68,
                      "attackerHandle is past the AE layout divergence point "
                      "(0x68) -- its compiled offset is WRONG on AE runtimes");

        // The item under deliberation sits right after the NiRefObject header;
        // the pinned CombatInventoryItem has no macro-guarded members at all,
        // so its layout is SE/AE-identical. Assert anyway -- intentional fixes.
        static_assert(offsetof(RE::CombatInventoryItem, item) == 0x10,
                      "CombatInventoryItem::item moved -- re-verify against the "
                      "pinned header before shipping");

        // CombatInventoryItem vtable: 00 dtor .. 0E CheckBusy, 0F CheckShouldEquip.
        constexpr std::size_t kCheckShouldEquip = 0x0F;

        // Originals keyed by vtable pointer, exactly like the caster hook: each
        // concrete item class is a separate vtable, recovered off `this`.
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_gateOrig;
        std::atomic<bool> g_gateHooked{ false };

        // Deduped deny log -- the combat AI re-deliberates at tick rate, so log
        // only when the (follower, denied item) pair CHANGES. Leaf mutex, deny
        // path only; party-sized, and a stale pair costs one suppressed repeat.
        std::mutex g_gateLogMx;
        std::unordered_map<RE::FormID, RE::FormID> g_lastGateDeny;

        using CheckShouldEquip_t = bool (*)(RE::CombatInventoryItem*, RE::CombatController*);

        bool EquipGateThunk(RE::CombatInventoryItem* a_this, RE::CombatController* a_cc) {
            // Recover the original for THIS vtable. Not one we hooked -> this
            // thunk was reached on a foreign object; do NOT read its members.
            // "Don't equip" is the benign answer here (the base impl itself
            // returns false for a fleeing combatant).
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_gateOrig.find(vt);
            if (oit == g_gateOrig.end()) return false;
            const auto original = reinterpret_cast<CheckShouldEquip_t>(oit->second);
            const bool aiSaysYes = original(a_this, a_cc);
            if (!aiSaysYes) return false;   // the AI already declined -> nothing to own

            // FAST OUT: no equip order anywhere -> every combatant's every
            // deliberation costs one relaxed load. The common case, and the
            // whole cost of the gate to a player who never uses equip gambits.
            if (g_equipOrders.load(std::memory_order_relaxed) == 0) return aiSaysYes;
            if (!a_cc) return aiSaysYes;

            auto attPtr = a_cc->attackerHandle.get();   // NiPointer<Actor>
            auto* actor = attPtr.get();
            if (!actor) return aiSaysYes;
            const auto fid = actor->GetFormID();

            bool held = false;
            {
                // Same lock ApplyTick already takes on this (combat) thread.
                std::lock_guard<std::mutex> lk(g_mx);
                const auto it = g_owned.find(fid);
                held = it != g_owned.end() && it->second.equipOrder;
            }
            if (!held) return aiSaysYes;

            // Every vtable this gate patches is a SPELL or STAFF inventory item
            // -- hand-competing magic by construction -- so no type dispatch is
            // needed. ONE exemption: the LATCHED gambit spell. A spellsword
            // list's cast rule below a satisfied equip is a legal off-hand loan
            // (GAMBIT_FLOWS §7.2); Loadout equips it itself, but the AI's own
            // deliberation over that same spell must not be refused either.
            // Sequential locks, never nested: g_mx released above, WantedSpell
            // takes CasterConsent's shared lock on its own.
            auto* item = a_this->item;
            const RE::FormID itemId = item ? item->GetFormID() : 0;
            if (itemId != 0 && itemId == CasterConsent::WantedSpell(fid)) return aiSaysYes;

            {
                std::lock_guard<std::mutex> lk(g_gateLogMx);
                auto& last = g_lastGateDeny[fid];
                if (last != itemId) {
                    last = itemId;
                    spdlog::info("[wstyle] {:08X} {}: AI re-arm of {:08X} DENIED -- an equip "
                                 "order owns the weapon stance", fid,
                                 actor->GetName() ? actor->GetName() : "?", itemId);
                }
            }
            return false;
        }

    }

    void InstallEquipGate() {
        if (g_gateHooked.exchange(true)) return;
        // VR GUARD, mirroring Targeting / CasterConsent: these vtable indices
        // are verified against the SE/AE pinned headers only.
        if (REL::Module::IsVR()) {
            spdlog::warn("[wstyle] VR runtime detected -- CombatInventoryItem vtable indices "
                         "are not verified for VR; equip gate NOT installed.");
            return;
        }

        // The CONCRETE spell/staff item classes. CombatInventoryItemMagic and
        // ...Staff themselves are ABSTRACT (pure CreateCaster) -- no live object
        // ever dispatches through their vtable symbols -- so the gate patches
        // the CombatInventoryItemMagicT<item, caster> INSTANTIATIONS, one per
        // caster category: the same 14-way split the CheckStartCast hook rides,
        // plus Armor (mage-armor spells are exactly what a caster re-arms; the
        // bare VTABLE_CombatMagicCasterArmor symbol has no class -- the §0.28
        // lesson -- but THIS template instantiation derives
        // CombatInventoryItemMagic in the pinned headers, so slot 0x0F is
        // verified by construction). Potion / scroll / shout items are
        // deliberately absent: combat drinking must never be denied (the
        // v1.0.32 lesson), shouts don't occupy a hand, and scrolls are outside
        // this bug's blast radius -- widen only on field evidence. The weapon
        // items (Melee/Ranged) are absent too: their classes are NOT in the
        // pinned headers, and weapon-vs-weapon is what the CSTY swap already
        // demonstrably wins (v1.0.33, the Auri case).
        const REL::VariantID kVtables[] = {
            // spells in hand
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterOffensive_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterRestore_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterWard_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterSummon_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterStagger_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterDisarm_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterCloak_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterLight_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterInvisibility_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterBoundItem_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterTargetEffect_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterParalyze_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterScript_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterReanimate_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterArmor_[0],
            // staves in hand (a staff strips the forced weapon exactly like a
            // spell does, and EquipWeapon counts a staff as NEITHER category)
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterOffensive_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterRestore_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterWard_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterSummon_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterStagger_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterDisarm_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterCloak_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterLight_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterInvisibility_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterBoundItem_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterTargetEffect_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterParalyze_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterScript_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterReanimate_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterArmor_[0],
        };

        int n = 0;
        for (const auto& id : kVtables) {
            REL::Relocation<std::uintptr_t> vt{ id };
            // write_vfunc returns the previous entry -- store it under this
            // vtable's address so the thunk dispatches to the right original.
            g_gateOrig[vt.address()] = vt.write_vfunc(kCheckShouldEquip, &EquipGateThunk);
            ++n;
        }
        spdlog::info("[wstyle] CheckShouldEquip equip gate hooked on {} spell/staff "
                     "inventory-item vtable(s)", n);
    }

}
