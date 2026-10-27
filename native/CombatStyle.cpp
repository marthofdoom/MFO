#include "PCH.h"
#include "Runtime.h"   // SeatVerified(): the mit-3.7 F1 self-check gate
#include "CombatStyle.h"
#include "CasterConsent.h"   // the equip gate exempts the latched gambit spell (T#75)
#include "Config.h"
#include "Forms.h"
#include "apmf/APMFBridge.h"      // IsOwnedCastActive -- stand the gate down where APMF's
                             // T2a CheckShouldEquip hook now owns it (Phase 2)
#include "cast/Actuation.h"       // ForcedHoldFor -- client mode: a ledger hold's ch.15 claim is
                             // ReconcileForcedWeapon's; ServiceOrderClaim owns only the rest

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
            bool                  dual     = false;          // Want: melee stance may dual wield (perk vote)
            bool                  appliedDual = false;       // the dual flavour we last wrote (handoff detect)
            RE::CombatController*  cc       = nullptr;        // identity ONLY -- never dereferenced
            RE::TESCombatStyle*    saved    = nullptr;        // engine-derived style this fight
            std::uint32_t          rederives = 0;             // engine re-derived under us, times
            bool                   equipOrder = false;        // T#75: stance came from an EQUIP gambit
            // feat/mfo-substrate-followup (review F1): an OOC substrate cast pair's OWN ownership of the cast
            // stance, separate from `stance` (the combat table's). Applied only by the substrate-only ApplyTick.
            bool                   substrateCast  = false;
            std::uint64_t          substrateSeq   = 0;   // the owning pair's seq (stale-call guard, see the header)
            // Review F2: a real fight took the substrate controller while our cast style stood on it -- the next
            // ApplyTick on `cc` writes `saved` back. Cancelled by any Want / WantSubstrateCast.
            bool                   restorePending = false;
        };

        // Written by Want/Clear (worker) AND ApplyTick (combat thread), so a
        // real lock -- a plain mutex, taken only when AnyActive() (the atomic
        // mirror) already says a stance is live, so a non-combat / no-stance
        // tick never touches it.
        std::mutex g_mx;
        std::unordered_map<RE::FormID, Owned> g_owned;
        std::atomic<std::size_t> g_count{ 0 };

        // DUAL-WIELD GATE (SetDualGate): followers whose perks do not vote dual wield. Value = the controller we
        // last logged a swap on (identity ONLY, never dereferenced) so the log fires once per fight. Under g_mx.
        std::unordered_map<RE::FormID, RE::CombatController*> g_gated;
        std::atomic<std::size_t> g_gatedCount{ 0 };

        // T#75: how many owned stances are EQUIP ORDERS. Relaxed-atomic mirror
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

        // Drop the COMBAT-TABLE stance of an entry (g_mx held). The entry itself survives while the substrate pair
        // owns its bit or a restore is pending; cc/saved/applied are kept so the substrate path does not re-baseline
        // against its own MFO style.
        void DropTableStanceLocked(std::unordered_map<RE::FormID, Owned>::iterator a_it) {
            auto& o = a_it->second;
            if (o.substrateCast || o.restorePending) {
                o.stance     = Stance::None;
                o.equipOrder = false;
            } else {
                g_owned.erase(a_it);
            }
            g_count.store(g_owned.size(), std::memory_order_relaxed);
            RecountEquipOrders();
        }

        // HARBINGER CLIENT MODE (fix/mfo-gates-client-only, 2026-10-11). Set ONCE at
        // kDataLoaded by InstallEquipGate, from APMFBridge::Available() (the bridge is
        // acquired earlier in the same handler, plugin.cpp). true = Harbinger is
        // present, the equip gate (InstallEquipGate) is NOT installed, and an equip order is expressed as
        // Harbinger's ch.15 kIntent_Equipment claim instead (ServiceOrderClaim). Read
        // on the worker only.
        std::atomic<bool> g_gateClientMode{ false };

        // Client mode: the weapon the ch.15 claim names when no ledger hold exists
        // (the equip rule is satisfied by a weapon the AI itself equipped, so
        // ReconcileForcedWeapon has nothing to claim). Harbinger's t2a denies every
        // spell/staff while the claim stands whatever WEAP it names (a spell/staff
        // FormID never equals it), so the form only has to be a real weapon in hand.
        // Right hand first, then left; a staff is magic, never the order's weapon.
        RE::FormID HeldOrderWeapon(RE::Actor* a_actor) {
            for (const bool left : { false, true }) {
                auto* w = a_actor->GetEquippedObject(left);
                auto* weap = w ? w->As<RE::TESObjectWEAP>() : nullptr;
                if (weap && !weap->IsStaff()) return weap->GetFormID();
            }
            return 0;
        }

        // Client mode: drop the ch.15 claim ServiceOrderClaim made, IF it is ours. A
        // ledger hold's claim belongs to ReconcileForcedWeapon / ReleaseForcedWeapon
        // (cast/Equip.cpp), so with a ledger standing nothing is touched here. Without
        // one, a standing claim can only be ServiceOrderClaim's (every ledger release
        // path releases the claim with it). Worker; never under g_mx.
        void ReleaseOrderClaimIfOurs(RE::FormID a_follower) {
            const auto hold = Actuation::ForcedHoldFor(a_follower);
            if (hold.first != 0 || hold.second != 0) return;
            if (!APMFBridge::IsEquipmentClaimActive(a_follower)) return;
            APMFBridge::ReleaseEquipment(a_follower);
            spdlog::info("[wstyle] {:08X}: equip order over -- Harbinger ch.15 equipment claim released "
                         "(client mode)", a_follower);
        }

        RE::TESCombatStyle* StyleFor(Stance a_stance, bool a_dual) {
            switch (a_stance) {
                // Dual-wield gate: 0x833 (DATA 1|4) only for a perk-voted dual wielder; everyone else gets the
                // DATA-1 twin so the engine AI cannot dual-wield two carried one-handers.
                case Stance::Melee:  return a_dual ? Forms::g_meleeStyle : Forms::g_meleeSingleStyle;
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
    bool        AnyGated()   { return g_gatedCount.load(std::memory_order_relaxed) != 0; }

    void SetDualGate(RE::FormID a_follower, bool a_gate) {
        std::lock_guard<std::mutex> lk(g_mx);
        if (a_gate) g_gated.try_emplace(a_follower, nullptr);
        else        g_gated.erase(a_follower);
        g_gatedCount.store(g_gated.size(), std::memory_order_relaxed);
    }

    void ClearDualGate(RE::FormID a_follower) { SetDualGate(a_follower, false); }

    void Want(RE::FormID a_follower, Stance a_stance, bool a_equipOrder, bool a_allowDual) {
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
        o.dual   = a_allowDual && a_stance == Stance::Melee;
        o.restorePending = false;   // review F2: the combat table owns the controller now -- no restore over it
        // T#75: only a WEAPON stance can be an equip order, and a later Want
        // that is not one (a cast latch flipping the stance to Cast, the class
        // override) RELEASES the hold -- the flag always tracks the most
        // recent real signal, never latches on its own.
        o.equipOrder = a_equipOrder && (a_stance == Stance::Melee || a_stance == Stance::Ranged);
        g_count.store(g_owned.size(), std::memory_order_relaxed);
        RecountEquipOrders();
    }

    void Clear(RE::FormID a_follower) {
        bool wasOrder = false;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            if (const auto it = g_owned.find(a_follower); it != g_owned.end()) {
                wasOrder = it->second.equipOrder;
                DropTableStanceLocked(it);   // the substrate bit / a pending restore survive (review F1)
            }
        }
        // Client mode: the order's ch.15 claim ends with it (combat end, death,
        // dismissal), not at the bridge's expiry sweep. Outside g_mx.
        if (wasOrder && g_gateClientMode.load(std::memory_order_relaxed))
            ReleaseOrderClaimIfOurs(a_follower);
    }

    void ClearAll() {
        std::lock_guard<std::mutex> lk(g_mx);
        g_owned.clear();
        g_count.store(0, std::memory_order_relaxed);
        g_equipOrders.store(0, std::memory_order_relaxed);
    }

    void ReleaseEquipOrder(RE::FormID a_follower) {
        std::lock_guard<std::mutex> lk(g_mx);
        // T#76: ONLY an equip-order stance releases here -- a cast latch (which
        // Want re-asserts every tick with equipOrder=false), the class override,
        // and the magicka-dry melee fallback all set equipOrder=false and so are
        // left standing. This scopes the release exactly to the equip gambit's
        // true->false lifecycle.
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end() || !it->second.equipOrder) return;
        DropTableStanceLocked(it);
    }

    void WantSubstrateCast(RE::FormID a_follower, std::uint64_t a_seq) {
        std::lock_guard<std::mutex> lk(g_mx);
        if (const auto it = g_owned.find(a_follower);
            it != g_owned.end() && it->second.substrateCast && a_seq < it->second.substrateSeq)
            return;   // a late Want for an ENDED pair: a newer pair owns the bit
        auto& o = g_owned[a_follower];   // a new entry has stance None: the table's stance is never touched here
        o.substrateCast  = true;
        o.substrateSeq   = a_seq;
        o.restorePending = false;
        g_count.store(g_owned.size(), std::memory_order_relaxed);
    }

    void ClearSubstrateCast(RE::FormID a_follower, std::uint64_t a_seq, bool a_restore) {
        std::lock_guard<std::mutex> lk(g_mx);
        const auto it = g_owned.find(a_follower);
        // Not the stored seq = a late Clear for an ended pair after a newer pair's Want: the bit is not ours.
        if (it == g_owned.end() || !it->second.substrateCast || it->second.substrateSeq != a_seq) return;
        auto& o = it->second;
        o.substrateCast = false;
        // Restore only what WE left standing: our cast style is the last write on a controller, and no table stance
        // owns that controller (a table stance hands off from Cast on its own next tick).
        if (a_restore && o.stance == Stance::None && o.cc && o.applied == Stance::Cast) {
            o.restorePending = true;
            spdlog::info("[wstyle] {:08X}: substrate cast stance ENDED by a real fight -- restore of the saved "
                         "engine style {:08X} armed for the next combat tick on that controller",
                         a_follower, o.saved ? o.saved->GetFormID() : 0);
            return;
        }
        if (o.stance == Stance::None && !o.restorePending) {
            g_owned.erase(it);
            g_count.store(g_owned.size(), std::memory_order_relaxed);
        }
    }

    bool HoldsEquipOrder(RE::FormID a_follower) {
        // Fast-out on the module's own discipline (mirrors the gate at :255 and
        // AnyActive): no equip order anywhere -> keep this per-acting-tick call
        // off the combat-thread-contended g_mx entirely.
        if (g_equipOrders.load(std::memory_order_relaxed) == 0) return false;
        std::lock_guard<std::mutex> lk(g_mx);
        const auto it = g_owned.find(a_follower);
        return it != g_owned.end() && it->second.equipOrder;
    }

    void ServiceOrderClaim(RE::Actor* a_follower) {
        if (!a_follower || !g_gateClientMode.load(std::memory_order_relaxed)) return;
        const auto id = a_follower->GetFormID();
        if (!HoldsEquipOrder(id)) {
            ReleaseOrderClaimIfOurs(id);   // the order released / flipped to a cast stance
            return;
        }
        // A ledger hold: ReconcileForcedWeapon (run just before this, same lap)
        // already claimed and refreshed ch.15 naming the forced weapon.
        const auto hold = Actuation::ForcedHoldFor(id);
        if (hold.first != 0 || hold.second != 0) return;
        const RE::FormID weap = HeldOrderWeapon(a_follower);
        if (weap == 0) {
            // Nothing in hand to name: no claim this lap (the equip rule's own
            // EquipWeapon forces one, and that ledger hold is claimed by Reconcile).
            if (APMFBridge::IsEquipmentClaimActive(id)) {
                APMFBridge::ReleaseEquipment(id);
                spdlog::info("[wstyle] {:08X}: equip order holds but no weapon is in hand -- Harbinger "
                             "ch.15 equipment claim released until one is (client mode)", id);
            }
            return;
        }
        const bool fresh = !APMFBridge::IsEquipmentClaimActive(id);
        APMFBridge::ClaimEquipment(id, weap);   // claims, re-points or refreshes (FacetExpiry)
        if (fresh && APMFBridge::IsEquipmentClaimActive(id))
            spdlog::info("[wstyle] {:08X}: equip order held on the AI's own weapon {:08X} -- Harbinger ch.15 "
                         "equipment claim made, so its t2a denies spell/staff re-arms (client mode)", id, weap);
    }

    void ApplyTick(RE::Actor* a_actor, RE::CombatController* a_cc, bool a_substrateOnly) {
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
        if (it == g_owned.end()) {
            // No owned stance: the dual-wield gate. Composition, not substitution: only a controller whose style
            // ALLOWS dual wielding is touched, and only for a follower the scheduler found perk-ineligible.
            if (a_substrateOnly) return;
            const auto gt = g_gated.find(fid);
            if (gt == g_gated.end() || !Forms::g_meleeSingleStyle) return;
            auto* cs = a_cc->combatStyle;
            if (!cs || cs == Forms::g_meleeSingleStyle ||
                !cs->flags.any(RE::TESCombatStyle::FLAG::kAllowDualWielding)) return;
            a_cc->combatStyle = Forms::g_meleeSingleStyle;
            if (gt->second != a_cc) {
                gt->second = a_cc;
                spdlog::info("[wstyle] {:08X} {}: dual-wield GATE -- style {:08X} allows dual wielding but his perks "
                             "do not vote it; MFO_MeleeSingleStyle {:08X} written (re-asserted silently)",
                             fid, a_actor->GetName() ? a_actor->GetName() : "?", cs->GetFormID(),
                             Forms::g_meleeSingleStyle->GetFormID());
            }
            return;
        }
        auto& o = it->second;

        // Review F2: a real fight took the substrate controller while our cast style stood on it. Write the engine's
        // own style back on THAT controller (identity compare only; `saved` is a form, never freed), then drop the
        // entry if nothing else owns it. Any other controller: the old one is gone and its style with it.
        if (o.restorePending) {
            o.restorePending = false;
            if (o.cc == a_cc && o.saved) {
                a_cc->combatStyle = o.saved;
                o.applied = Stance::None;
                spdlog::info("[wstyle] {:08X} {}: substrate cast stance over -- engine combat style {:08X} RESTORED "
                             "on the controller the real fight kept", fid, a_actor->GetName() ? a_actor->GetName() : "?",
                             o.saved->GetFormID());
            } else {
                spdlog::info("[wstyle] {:08X}: substrate cast style restore DROPPED ({})", fid,
                             o.cc != a_cc ? "a different controller: the substrate one is gone, its style with it"
                                          : "no saved engine style to write back");
            }
            if (o.stance == Stance::None && !o.substrateCast) {
                g_owned.erase(it);
                g_count.store(g_owned.size(), std::memory_order_relaxed);
                RecountEquipOrders();
            }
            return;
        }

        // Review F1: the two owners never mix. A substrate controller gets the substrate pair's cast stance iff its
        // bit is set (a combat-table stance, stale or live, is ignored there); a real fight gets the table's stance
        // only (the bit is ignored; its Cast write hands off or is restored when the pair closes).
        const Stance want = a_substrateOnly ? (o.substrateCast ? Stance::Cast : Stance::None) : o.stance;
        const bool dual = (want == Stance::Melee) && !a_substrateOnly && o.dual;
        RE::TESCombatStyle* target = StyleFor(want, dual);
        if (!target) return;   // no owner on this path, or form missing -> stance disabled, silent per the "one feature" rule

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
            o.applied   = want;
            o.appliedDual = dual;
            a_cc->combatStyle = target;
            spdlog::info("[wstyle] {:08X} {}: combat style OWNED {:08X} -> {:08X} ({} stance{})",
                         fid, a_actor->GetName() ? a_actor->GetName() : "?",
                         o.saved ? o.saved->GetFormID() : 0,
                         target->GetFormID(), Name(want),
                         want == Stance::Melee ? (dual ? ", dual-wield allowed (perks)" : ", single-weapon (no dual-wield perks)") : "");
            return;
        }

        // Same fight. Two reasons the live style can differ from our target:
        //  * a melee<->ranged HANDOFF (the winning gambit flipped) -- expected,
        //    log it once at the transition;
        //  * the engine RE-DERIVED the style under us mid-combat -- re-assert so
        //    the follower stays in the gambit's stance (the cast probe proved
        //    the engine does this; count it).
        if (o.applied != want || o.appliedDual != dual) {
            a_cc->combatStyle = target;
            o.applied = want;
            o.appliedDual = dual;
            spdlog::info("[wstyle] {:08X}: stance HANDOFF -> {:08X} ({}{})",
                         fid, target->GetFormID(), Name(want),
                         want == Stance::Melee ? (dual ? ", dual-wield allowed (perks)" : ", single-weapon") : "");
        } else if (a_cc->combatStyle != target) {
            // The engine re-derived the base style under us mid-combat (the cast
            // probe proved it does). COUNT it regardless of the toggle below --
            // the state report's rederive count stays meaningful whether or not
            // MFO is fighting it.
            ++o.rederives;

            // bCstyReassert (Docs/SPEC-COMBATSTYLE-SOURCEGATE.md), default ON.
            // OFF = PROOF/FIX MODE: do NOT fight the engine every tick. The
            // one-shot ownership set from branch A stands; the equip gate
            // (EquipGateThunk, below) is relied on to hold the weapon
            // instead. This is the ONLY gated write in ApplyTick -- branch A
            // (initial ownership) and branch B (stance handoff) are untouched,
            // so ON stays byte-identical to pre-toggle behavior.
            if (!Config::g_cstyReassert.load(std::memory_order_relaxed)) {
                if (o.rederives == 1)
                    spdlog::info("[wstyle] {:08X}: re-assert SUPPRESSED (bCstyReassert off) -- "
                                 "one-shot stance held, equip gate holds", fid);
                return;
            }

            // Re-assert EVERY tick, but LOG ONLY THE FIRST -- this is a
            // default-ON feature and a persistent re-derive would otherwise
            // write one MFO.log line per combat tick. The first line proves it
            // happens; the count rides the state report.
            a_cc->combatStyle = target;
            if (o.rederives == 1)
                spdlog::info("[wstyle] {:08X}: engine RE-DERIVED the style under the swap "
                             "-- re-asserting {} ({}) each tick (silenced after first)",
                             fid, target->GetFormID(), Name(want));
        }
    }

    // ── THE EQUIP GATE (T#75) ─────────────────────────────────────────────────
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

            // Phase 2 (APMF ALLOWANCE-TEMPLATE.md §7), ch.8b since feat/offense-
            // cast-seats (2026-09-05): while this follower's cast is APMF-owned,
            // APMF's OWN CheckShouldEquip hook (0x0F, one of the kIntent_Cast
            // engine seats, EquipGate.cpp) already denies any spell/staff that
            // isn't the claimed one -- covering exactly what this gate's
            // WantedSpell exemption protects, sourced from the SAME claim
            // (ClaimOffenseCast passes the identical spell). Standing down here
            // avoids two independently-sourced equip denies (MFO's own
            // weapon-equip-order gate vs. APMF's claim gate) disagreeing; APMF's
            // hook keeps the gambit spell equippable even while an unrelated
            // weapon equip-order is held on this follower.
            if (APMFBridge::IsOwnedCastActive(fid)) return aiSaysYes;

            // Phase 2 (APMF ch.15, kIntent_Equipment): while this follower's OWN
            // weapon-order is APMF-claimed (APMFBridge::ClaimEquipment, engaged/
            // refreshed by Actuation::ReconcileForcedWeapon for as long as the
            // force-hold survives), APMF's OWN CheckShouldEquip hook (T2a,
            // EquipGate.cpp) already denies any spell/staff re-arm for the exact
            // same reason this gate would (replicating this gate's own
            // WantedSpell exemption, so the off-hand loan below still works) --
            // standing down here avoids two independently-sourced equip denies
            // (this gate vs. APMF's claim gate) disagreeing. Independent of the
            // cast-active stand-down above: either, both, or neither may be true
            // for a given follower on a given tick. IsEquipmentClaimActive is
            // false whenever APMF is absent/off or no claim was made, so the
            // APMF-absent path below is untouched -- byte-identical native
            // enforcement.
            if (APMFBridge::IsEquipmentClaimActive(fid)) return aiSaysYes;

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
        // HARBINGER CLIENT MODE (fix/mfo-gates-client-only). With Harbinger present
        // this gate's slot 0x0F is Harbinger's own t2a seat, and writing it as well
        // produced the seat-conflict errors in Harbinger's census (LoreRim). Every
        // case this gate decides is a Harbinger request instead: an equip order is
        // the ch.15 kIntent_Equipment claim (ReconcileForcedWeapon for a ledger hold,
        // ServiceOrderClaim for an order on the AI's own weapon), the latched-spell
        // loan is the ch.8b cast claim t2a exempts, and the owned-cast / equipment
        // stand-downs were already Harbinger's. The gate's code below runs ONLY when
        // Harbinger is absent (MFO's legacy road, never a fallback when Harbinger
        // declines). Below the ABI floor the bridge requests (10, apmf/Bridge.cpp
        // Acquire), Harbinger reads as absent here, and ch.15 is older than that.
        if (APMFBridge::Available()) {
            g_gateClientMode.store(true, std::memory_order_relaxed);
            spdlog::info("[wstyle] CheckShouldEquip equip gate (0x0F): not installed (Harbinger client mode) "
                         "-- equip orders go to Harbinger as the ch.15 equipment claim");
            return;
        }
        // VR GUARD, mirroring Targeting / CasterConsent: these vtable indices
        // are verified against the SE/AE pinned headers only.
        // G1: EXACT builds only (was VR-only).
        if (!Runtime::Known()) {
            spdlog::warn("[wstyle] runtime {} not supported ({}) -- CombatInventoryItem vtable indices "
                         "are not verified there; equip gate NOT installed.",
                         REL::Module::get().version().string("."), Runtime::GateReason());
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
        int i = 0;
        for (const auto& id : kVtables) {
            REL::Relocation<std::uintptr_t> vt{ id };
            // mit-3.7 F1: a vtable the self-check did not verify is refused alone.
            if (!Runtime::SeatVerified(vt.address(), fmt::format("CombatStyle.CheckShouldEquip kVtables[{}]", i++))) {
                continue;
            }
            // write_vfunc returns the previous entry -- store it under this
            // vtable's address so the thunk dispatches to the right original.
            g_gateOrig[vt.address()] = vt.write_vfunc(kCheckShouldEquip, &EquipGateThunk);
            ++n;
        }
        spdlog::info("[wstyle] CheckShouldEquip equip gate (0x0F): installed (Harbinger absent) on {} "
                     "spell/staff inventory-item vtable(s)", n);
    }

}
