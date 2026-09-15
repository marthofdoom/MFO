#include "PCH.h"
#include "Loadout.h"
#include "Config.h"
#include "Followers.h"
#include "CasterConsent.h"   // v1.0.32: StartCooldown mirrors into the hook's permit

namespace MFO::Loadout {

    namespace {

        // What MFO displaced, and what it owes back. Transient by construction:
        // this describes a live loadout, and a loadout is engine state that the
        // save already records (INVARIANTS #16). Persisting it would let a stale
        // entry re-equip gear the player has since sold. The obligation is
        // instead RECONSTRUCTED from live state on load -- see Reconcile().
        //
        // MAIN THREAD ONLY: written from Prepare (Scheduler::Tick <- pump task),
        // OnFollowerHit / Tick (queued tasks), ClearTransientState (revert
        // callback). Same discipline as g_followers -- no lock, no cross-thread
        // reader.
        struct Debt {
            RE::TESBoundObject* displacedLeft = nullptr;   // shield or off-hand item
            RE::TESBoundObject* stowedWeapon  = nullptr;   // two-hander taken away
            bool  leftWasShield = false;
            std::chrono::steady_clock::time_point stowedAt{};
        };
        std::unordered_map<RE::FormID, Debt> g_debt;

        // SEPARATE from the debt, deliberately. When the follower's own AI
        // takes its weapon back, the debt is settled and its entry erased --
        // but the debounce must still gate the NEXT stow, or we re-stow
        // immediately, the AI re-equips, and the two of us thrash at tick
        // cadence. Exactly the churn fTwoHandedDebounce exists to prevent.
        std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_lastStow;

        // The AI-grace clock -- ALSO separate from the debt, and for the same
        // reason. The first version stored equippedAt inside the Debt, but a
        // debt entry only survives while gear is owed: an empty-handed or
        // sword-only follower's entry is all-null and Tick() settle-erases it
        // within one pump cycle, and a Grip::Caster / alreadyHolding follower
        // never gets an entry at all. SecondsSinceEquip then returned "forever"
        // and the silent cast fired instantly -- the confound rebuilt for
        // exactly the most common grips. The window MFO promises the follower's
        // AI must not be keyed to whether MFO happens to owe them gear.
        std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_equipClock;

        // When each follower may next hold a gambit spell. Separate from the
        // debt, which is about GEAR -- this is about PACING, and it has to
        // outlive an equip/release cycle to mean anything.
        std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_coolUntil;

        // Which spell MFO put in whose hand. NOT part of Debt: debt is about
        // GEAR and is settled as soon as nothing is owed, which for an
        // empty-off-hand caster is one pump wake after the equip. This has to
        // outlive that or the limiter never fires for the followers it matters
        // most for.
        std::unordered_map<RE::FormID, RE::FormID> g_mfoSpell;

        // The vanilla EQUP forms, read from Skyrim.esm's DOBJ record (DNAM
        // `LHEQ` -> 0x13F43, `RHEQ` -> 0x13F42) and confirmed against the EQUP
        // EDIDs ("LeftHand" / "RightHand" / 0x13F44 "EitherHand"). Skyrim.esm is
        // always load index 00, so the runtime FormID is the file FormID.
        constexpr RE::FormID kLeftHandEquipSlot = 0x00013F43;

        bool IsTwoHanded(RE::TESForm* a_form) {
            auto* weap = a_form ? a_form->As<RE::TESObjectWEAP>() : nullptr;
            if (!weap) return false;
            switch (weap->GetWeaponType()) {
            case RE::WEAPON_TYPE::kTwoHandSword:
            case RE::WEAPON_TYPE::kTwoHandAxe:
            case RE::WEAPON_TYPE::kBow:
            case RE::WEAPON_TYPE::kCrossbow:
                return true;
            default:
                return false;
            }
        }

        bool IsShield(RE::TESForm* a_form) {
            auto* armo = a_form ? a_form->As<RE::TESObjectARMO>() : nullptr;
            return armo && armo->IsShield();
        }
    }
    // Declared in Loadout.h; defined below the anon namespace. Forward-declared
    // here so RestoreOne (anon, above the definition) can name it.
    const RE::BGSEquipSlot* LeftHandSlot();
    namespace {

        // `a_slot` (Fable F2 on f771399): a displaced LEFT-hand WEAPON must go back
        // to the LEFT -- a slot-less EquipObject on a one-hander lands it in the
        // RIGHT hand (the engine's default), which for a dual-wielder displaces the
        // main-hand weapon at combat end. Shields and a stowed two-hander keep the
        // slot-less call (a shield has its own slot, a two-hander takes both).
        void EquipBack(RE::Actor* a_actor, RE::TESBoundObject* a_obj,
                       const RE::BGSEquipSlot* a_slot = nullptr) {
            if (!a_actor || !a_obj) return;
            auto* mgr = RE::ActorEquipManager::GetSingleton();
            if (!mgr) return;
            mgr->EquipObject(a_actor, a_obj, nullptr, 1, a_slot);
        }

        // Hand everything back for ONE follower. Returns what it restored, for
        // the log.
        int RestoreOne(RE::FormID a_id) {
            auto it = g_debt.find(a_id);
            if (it == g_debt.end()) return 0;

            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
            int n = 0;
            if (actor) {
                if (it->second.stowedWeapon)  {
                    EquipBack(actor, it->second.stowedWeapon);  ++n;
                    // ARM THE DEBOUNCE HERE, NOT AT STOW TIME. Both used to read
                    // the same clock, so the weapon was handed back at exactly
                    // the moment a re-stow became legal again -- a still-true
                    // rule re-stowed on the very next tick, and the follower
                    // held their greatsword for one tick in every six seconds.
                    // The debounce is a floor on how often we TAKE it, so it
                    // starts when they get it back.
                    g_lastStow[a_id] = std::chrono::steady_clock::now();
                }
                if (auto* left = it->second.displacedLeft) {
                    // A weapon displaced from the LEFT hand returns to the LEFT hand
                    // (F2); anything else (shield, torch) keeps the default call.
                    EquipBack(actor, left, left->As<RE::TESObjectWEAP>() ? LeftHandSlot() : nullptr);
                    ++n;
                }
            }
            g_debt.erase(it);
            // The restore displaced MFO's spell, so the AI window it measured is
            // over; the next equip arms a fresh one.
            g_equipClock.erase(a_id);
            return n;
        }

    }

    // PUBLIC since 2026-09-13 (was file-local in the anon namespace above):
    // Actuation.cpp's dual-wield LEFT-hand weapon equip targets the very slot
    // form the spell equips in Prepare() below use. ONE implementation, so the
    // form it names is the same in every caller.
    const RE::BGSEquipSlot* LeftHandSlot() {
        // Resolved by FormID, NOT through BGSDefaultObjectManager::GetObject.
        // The pinned CommonLib (3.7.0) implements IsObjectInitialized(idx) as
        // `RelocateMember<bool*>(this, 0xB80, 0xBA8)[idx]`: it DEREFERENCES
        // the 8 bytes at +0xB80 as a `bool*` and indexes THAT. Measured
        // against both unpacked binaries (ctor memset + Load + InitItemImpl;
        // Docs/ADDRESS-TABLE-2026-09-15.md row "BGSDefaultObjectManager::
        // objects[] / objectInit[] count": 364 on 1.5.97, 366 on 1.6.1170, 372
        // on 1.7.104; the engine's own GetLeftHandSlot, SE 0x3315F0, reads
        // `[dom+0xB93]` = objectInit[19] then `[dom+0xB8]` = objects[19]):
        //   1.5.97   objects[364] @ +0x20..+0xB80, objectInit[364] @ +0xB80
        //   1.6.1170 objects[366] @ +0x20..+0xB90, objectInit[366] @ +0xB90
        // So on 1.5.97 the read turns the first eight init bools into the
        // poison pointer 0x0101010101010101 and faults (the T#67 crash reports).
        //
        // ON 1.6.1170 IT ALWAYS RETURNED nullptr IN THE FIELD (Fable, round 2 of
        // feat/mfo-1.5.97-pass, measured 2026-09-15): +0xB80 there is
        // objects[364], and the engine's DOBJ tag table (stride 24, .data; AE
        // index 19 = LHEQ, 20 = RHEQ, 364 = HMAE, 365 = MHFL, 366 entries)
        // makes that DOBJ `HMAE` = Update.esm FLST 0x01003275, whose record
        // header flags are 0x0 -- so `((bool*)objects[364])[19]` is byte 3 of a
        // zero formFlags word: 0, "not initialized", nullptr. Every spell equip
        // on 1.6.1170 therefore ran `EquipSpell(actor, spell, nullptr)` (the
        // engine's helper, AE 0x6CAB20, then picks the hand itself for an
        // EitherHand spell) and Actuation's EquipLeftHeld (`if (!slot) return
        // false`) never fired. THIS FormID lookup is the first build where the
        // LeftHand slot actually reaches the engine on 1.6.1170; MAP.md's
        // RUNTIME GATES entry lists what that switches on there.
        //
        // The engine's own InitItemImpl fills objects[19] with LookupByID
        // (DOBJ.LHEQ), i.e. exactly this lookup, so the slot the engine calls
        // "left" IS this form on both runtimes and the lookup has no layout to
        // get wrong.
        // (The <d3d11.h> GetObject -> GetObjectW hijack noted in Board.cpp's
        // banner, ENGINE_NOTES §9, no longer matters here: nothing in this TU
        // names GetObject any more.)
        return RE::TESForm::LookupByID<RE::BGSEquipSlot>(kLeftHandEquipSlot);
    }

    Hands Read(RE::Actor* a_actor, RE::SpellItem* a_spell) {
        Hands h;
        if (!a_actor) return h;

        h.right = a_actor->GetEquippedObject(false);
        h.left  = a_actor->GetEquippedObject(true);
        h.leftIsShield = IsShield(h.left);

        // Already holding the exact spell we want: the FREE case this whole
        // policy is built around.
        if (a_spell && (h.left == a_spell || h.right == a_spell)) {
            h.alreadyHolding = true;
            h.grip = Grip::Caster;
            return h;
        }

        // A follower already holding a spell CHOSE that, or MFO gave it to them
        // last tick. Either way their hands are not ours to rearrange
        // (DESIGN §4.5b's "caster: nothing to do" row).
        if ((h.left  && h.left->As<RE::SpellItem>()) ||
            (h.right && h.right->As<RE::SpellItem>())) {
            h.grip = Grip::Caster;
            return h;
        }

        if (IsTwoHanded(h.right)) h.grip = Grip::TwoHanded;
        else if (h.right)         h.grip = Grip::OneHanded;
        else                      h.grip = Grip::Empty;
        return h;
    }

    namespace {

        // Vanilla Skyrim.esm Dual Casting perks, one per school (verified
        // against the shipped ESM's own FormIDs -- a contiguous authoring
        // block 000153CD..000153D1: Alteration, Conjuration, Destruction,
        // Illusion, Restoration, immediately followed by Impact 000153D2).
        // No INI override: unlike Config::g_merchantPerkID (Logistics_
        // Economy.cpp), which was made overridable because something already
        // consumed it, PlanCastHand/CanDualCast have no live executing
        // caller yet (see Loadout.h's HandPick::DualCast doc) -- add one if
        // a perk-overhaul's relocated forms ever need it, matching that
        // exact idiom.
        RE::TESForm* DualCastPerkForSchool(RE::ActorValue a_school) {
            using AV = RE::ActorValue;
            std::uint32_t id = 0;
            switch (a_school) {
            case AV::kAlteration:  id = 0x000153CD; break;
            case AV::kConjuration: id = 0x000153CE; break;
            case AV::kDestruction: id = 0x000153CF; break;
            case AV::kIllusion:    id = 0x000153D0; break;
            case AV::kRestoration: id = 0x000153D1; break;
            default: return nullptr;
            }
            return RE::TESForm::LookupByID(id);
        }

    }

    bool CanDualCast(RE::Actor* a_actor, RE::SpellItem* a_spell) {
        if (!a_actor || !a_spell) return false;

        // The spell's OWN school (same read as Logistics_Cast.cpp's
        // TargetMagicSchool: the costliest effect's base MGEF "Magic Skill").
        // A spell with no school (kNone) never dual-casts.
        const auto* eff  = a_spell->GetCostliestEffectItem();
        const auto* mgef = eff ? eff->baseEffect : nullptr;
        const auto school = mgef ? mgef->data.associatedSkill : RE::ActorValue::kNone;

        auto* perk = DualCastPerkForSchool(school);
        auto* perkForm = perk ? perk->As<RE::BGSPerk>() : nullptr;
        if (!perkForm) return false;

        // Followers can carry a perk on their base TESNPC template (shared by
        // multiple NPCs) as well as their own runtime perk list -- HasPerk
        // alone under-reports the template case (the OwnsExactPerk idiom
        // already used for the merchant-perk-bypass check, Logistics_
        // Economy.cpp / ProgAllocator.cpp).
        auto* base = a_actor->GetActorBase();
        const bool owns = a_actor->HasPerk(perkForm) ||
                          (base && base->GetPerkIndex(perkForm).has_value());
        if (!owns) return false;

        auto* avo = a_actor->AsActorValueOwner();
        if (!avo) return false;

        // Dual-casting's real cost is CalculateMagickaCost's single-hand
        // figure (skill-discounted, per-actor, the SAME call every other
        // affordability gate in this codebase uses) times the vanilla GMST
        // fMagicDualCastingCostMult -- hardcoded to Skyrim.esm's shipped
        // value (2.8, not the Creation Kit's 1.5 default) rather than read
        // live, matching how fBarterMax/fBarterMin are already hardcoded to
        // their vanilla GMST values elsewhere (Logistics_Economy.cpp) instead
        // of introducing a new live-GMST-read pattern for one call site.
        constexpr float kDualCastCostMult = 2.8f;
        const float singleCost = a_spell->CalculateMagickaCost(a_actor);
        const float have       = avo->GetActorValue(RE::ActorValue::kMagicka);
        return have >= singleCost * kDualCastCostMult;
    }

    HandPick PlanCastHand(RE::Actor* a_actor, RE::SpellItem* a_spell, bool a_weaponHandActive) {
        // HARD RULE, non-negotiable: a weapon (or an equip gambit about to
        // reassert one) owns the right hand -- spells claim LEFT ONLY,
        // always. See Loadout.h's HandPick::Left / APMFBridge::
        // kApmfHandLeft's doc for the deck-proven failure this closes.
        if (a_weaponHandActive) return HandPick::Left;

        // Both hands genuinely free: a pure caster. One spell wanted -> a
        // free off-hand with the real dual-cast perk and pool to back it is
        // the wrong case to default to a single-hand cast in -- assume
        // dual-cast. Otherwise either hand serves (a second, different
        // spell wanted the same tick takes the other -- the caller's call).
        return CanDualCast(a_actor, a_spell) ? HandPick::DualCast : HandPick::EitherFree;
    }

    Ready Prepare(RE::Actor* a_actor, RE::SpellItem* a_spell, std::string& a_why,
                  LeftHandYield a_yieldLeft) {
        if (!a_actor || !a_spell) { a_why = "no actor or spell"; return Ready::Failed; }

        // NEVER grant a spell the follower does not know (INVARIANTS #20,
        // DESIGN §5.4). A rule they cannot run FAILS and says so -- that is the
        // competence gate, not a problem to paper over.
        if (!a_actor->HasSpell(a_spell)) {
            a_why = "follower does not know this spell";
            return Ready::Failed;
        }

        const auto id  = a_actor->GetFormID();
        const auto now = std::chrono::steady_clock::now();

        const auto hands = Read(a_actor, a_spell);
        if (hands.alreadyHolding) {
            // Spell already in hand: the AI window starts the first time a rule
            // WANTS this cast, not before. try_emplace -- re-arming every tick
            // would mean the window never elapses.
            g_equipClock.try_emplace(id, now);
            return Ready::AlreadyReady;
        }

        auto existing = g_debt.find(id);

        // Already holding a DIFFERENT spell (alreadyHolding was false above, so
        // it is NOT the gambit spell). Historically MFO left their hands alone
        // to avoid orphaning a gear ledger entry -- but that left a cast-control
        // follower standing there with their AI's OWN spell in hand, denied/
        // aborted, inactive (deck 2026-08-06, "wrong spell equipped, doing
        // nothing"). A spell->spell swap displaces NO gear, so there is no ledger
        // to orphan: when cast control is engaged, REPLACE their spell with the
        // gambit's so their AI casts what the gambit dictates (active, right
        // animation, nothing to suppress). EquipSpell is a no-op once it sticks,
        // so this only re-fires while the AI keeps swapping back. (v1.0.36.)
        if (hands.grip == Grip::Caster) {
            const int castLvl = Config::g_castControl.load();
            if (castLvl > 0) {
                if (auto* m = RE::ActorEquipManager::GetSingleton()) {
                    // POINT OF NO RETURN (F1): the left hand is taken on the next
                    // line, so MFO's own left weapon hold yields here and not
                    // earlier. No debt is booked on this branch (spell->spell swap).
                    if (a_yieldLeft) a_yieldLeft(a_actor);
                    m->EquipSpell(a_actor, a_spell, LeftHandSlot());
                    a_actor->DrawWeaponMagicHands(true);

                    // EXACT MODE (level 4) ONLY: neutralize a competing spell left in
                    // the OTHER (right) hand. EquipSpell above only ever targets the
                    // LEFT slot, so a follower's own spell sitting in the RIGHT hand
                    // survives the swap untouched -- their AI keeps re-selecting it
                    // and getting denied by CasterConsent's exclusivity gate
                    // (CasterConsent.cpp ~:560), charging and never releasing instead
                    // of ever casting the gambit spell (the owned-cast regression,
                    // fixed 2026-09-02). DeselectSpell, NOT UnequipObject -- a
                    // hand-held spell is *selected*, not inventory-equipped
                    // (Reconcile's note below has the same call). Gated to level 4
                    // only: looser levels deliberately leave a category (heals,
                    // buffs...) to the AI's own hand (CastExempt), and stripping the
                    // other hand there would take that category away too.
                    if (castLvl >= 4 && hands.right && hands.right != a_spell) {
                        if (auto* other = hands.right->As<RE::SpellItem>())
                            a_actor->DeselectSpell(other);
                    }

                    g_equipClock.try_emplace(id, now);
                    a_why = "replaced their own spell with the gambit spell";
                    return Ready::AlreadyReady;
                }
            }
            g_equipClock.try_emplace(id, now);   // control off -> leave their hands alone
            a_why = "already holding a spell -- not rearranging their hands";
            return Ready::AlreadyReady;
        }

        // RATE LIMIT (fCastCooldown). After a cast, StartCooldown stamps
        // g_coolUntil; until it expires MFO does not put a spell back into an
        // empty/weapon hand, so casts pace at the configured interval and the
        // follower fights normally in between. Placed AFTER the already-holding
        // and own-spell returns above, so a spell currently in hand is left
        // alone -- this only gates a NEW equip. This is what makes fCastCooldown
        // an actual knob instead of a timer nothing reads.
        if (CoolingDown(id)) {
            a_why = "cast cooling down";
            return Ready::Debounced;
        }

        if (hands.grip == Grip::TwoHanded) {
            // A two-hander must be STOWED to cast: visible, interrupts the
            // attack, and a frequently-firing rule would leave them endlessly
            // sheathing. The debounce is a floor on how often MFO will pay
            // that, independent of the rule's own suppression window.
            const auto stow = g_lastStow.find(id);
            const float since = stow != g_lastStow.end()
                                  ? std::chrono::duration<float>(now - stow->second).count()
                                  : 1.0e9f;
            if (since < Config::g_twoHandedDebounce.load()) {
                a_why = "two-handed debounce";   // stable: the transition logger compares reasons
                return Ready::Debounced;
            }
        }

        // NEVER overwrite an unpaid debt. If we already owe this follower
        // something, we are not taking anything else from them.
        if (existing != g_debt.end() &&
            (existing->second.displacedLeft || existing->second.stowedWeapon)) {
            a_why = "already owe this follower gear -- not displacing more";
            return Ready::Debounced;
        }

        auto* mgr = RE::ActorEquipManager::GetSingleton();
        if (!mgr) { a_why = "no equip manager"; return Ready::Failed; }

        // Work out what we are about to displace, but do NOT record it until the
        // equip actually happens -- a ledger entry for a displacement that never
        // occurred makes us "restore" gear the follower never lost.
        RE::TESBoundObject* willDisplaceLeft = nullptr;
        RE::TESBoundObject* willStowWeapon   = nullptr;
        if (hands.grip == Grip::TwoHanded) {
            willStowWeapon = hands.right ? hands.right->As<RE::TESBoundObject>() : nullptr;
        } else if (hands.left) {
            willDisplaceLeft = hands.left->As<RE::TESBoundObject>();
        }

        // POINT OF NO RETURN (F1): every refusal above has had its say, the spell
        // takes the left hand on the next line -- MFO's own left weapon hold
        // yields NOW. Its unequip is queued, so Read()'s synchronous view above
        // still showed that weapon in the left: a true return means the left item
        // was MFO's OWN hold, not gear the follower loses -- book NO debt for it
        // (F2), or the repay hands MFO's released weapon back at combat end.
        if (a_yieldLeft && a_yieldLeft(a_actor)) willDisplaceLeft = nullptr;

        mgr->EquipSpell(a_actor, a_spell, LeftHandSlot());
        a_actor->DrawWeaponMagicHands(true);

        auto& debt = g_debt[id];
        debt.displacedLeft = willDisplaceLeft;
        debt.stowedWeapon  = willStowWeapon;
        debt.leftWasShield = hands.leftIsShield;
        debt.stowedAt      = now;
        // A FRESH equip gets a FRESH window -- plain assignment, unlike the
        // AlreadyReady try_emplace above.
        g_equipClock[id] = now;
        // Only a TWO-HANDED stow arms the debounce. The off-hand swap is free
        // by design, and letting it set the clock would spuriously block a real
        // stow for the next 6 s.
        if (willStowWeapon) g_lastStow[id] = now;

        g_mfoSpell[id] = a_spell->GetFormID();
        return Ready::Equipped;
    }

    void ReleaseSpell(RE::FormID a_actorID) {
        const auto it = g_mfoSpell.find(a_actorID);
        if (it == g_mfoSpell.end()) return;

        // MINIMUM HOLD. Releasing inside the grace would yank the spell before
        // the window their AI was just promised, and can interrupt a cast
        // already in flight. It also bounds churn: a condition flickering at
        // service cadence can cost at most one equip/release per grace period,
        // where before it could cost one per tick.
        if (SecondsSinceEquip(a_actorID) < Config::g_aiCastGrace.load()) return;

        auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_actorID);
        auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(it->second);
        g_mfoSpell.erase(it);
        g_equipClock.erase(a_actorID);
        if (!actor || !spell) return;

        // Only if it is still OUR spell in that hand. Their own AI may have
        // swapped since, and unequipping a choice they made is not ours to do.
        if (actor->GetEquippedObject(true) != spell) return;

        actor->DeselectSpell(spell);
        spdlog::debug("[loadout] {:08X} -- {} taken back", a_actorID,
                      spell->GetName() ? spell->GetName() : "?");

        // DELIBERATELY NOT restoring displaced gear here.
        //
        // A cooldown cycle releases and re-equips every few seconds, so forcing
        // a shield back each time would equip/unequip it for the whole fight --
        // exactly the churn §4.5b's deferred restore exists to avoid. The gear
        // has its own triggers and they are better ones: on the next hit, at
        // combat end, and on dismissal. This only takes back the SPELL.
    }

    void StartCooldown(RE::FormID a_actorID) {
        const float cd = Config::g_castCooldown.load();
        if (cd <= 0.0f) return;
        g_coolUntil[a_actorID] = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(static_cast<int>(cd * 1000.0f));
        // MIRROR INTO THE CONSENT HOOK (v1.0.32). "Taking the spell back" was
        // never the whole rate limit: ReleaseSpell's minimum hold (the grace)
        // leaves the spell in hand for seconds after a cast, and the thunk's
        // force-YES was firing on every caster tick of that window -- the
        // deck's 4-casts-in-2.2s burst. The permit itself must know the
        // deadline; the thunk reads this mirror under its own lock (never
        // Loadout's non-atomic maps from the combat thread).
        CasterConsent::NoteCooldown(a_actorID, cd);
        // Taking the spell back IS the rate limit -- they cannot cast what they
        // are not holding. Leaving it in hand and merely declining to re-equip
        // would pace MFO and do nothing about their AI.
        ReleaseSpell(a_actorID);
    }

    bool CoolingDown(RE::FormID a_actorID) {
        const auto it = g_coolUntil.find(a_actorID);
        return it != g_coolUntil.end() && std::chrono::steady_clock::now() < it->second;
    }

    void ArmGrace(RE::FormID a_actorID) {
        // Deliberately an overwrite, not try_emplace. The clock is armed at
        // equip with try_emplace so that re-querying cannot reset it mid-window
        // -- but once MFO has actually cast, the window is spent and the next
        // one must start fresh.
        g_equipClock[a_actorID] = std::chrono::steady_clock::now();
    }

    float SecondsSinceEquip(RE::FormID a_actorID) {
        const auto it = g_equipClock.find(a_actorID);
        if (it == g_equipClock.end()) return 1.0e9f;
        return std::chrono::duration<float>(
                   std::chrono::steady_clock::now() - it->second).count();
    }

    void OnFollowerHit(RE::FormID a_actorID) {
        auto it = g_debt.find(a_actorID);
        if (it == g_debt.end() || !it->second.leftWasShield || !it->second.displacedLeft) return;

        auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_actorID);
        if (!actor) return;

        EquipBack(actor, it->second.displacedLeft);
        spdlog::info("[loadout] {:08X} took a hit -- shield restored", a_actorID);
        it->second.displacedLeft = nullptr;
        it->second.leftWasShield = false;
    }

    void Tick() {
        // The clock outlives the debt. A follower who displaced nothing settles
        // immediately, so without this their clock keeps its last timestamp into
        // the NEXT fight -- Prepare returns AlreadyReady, try_emplace no-ops on
        // the stale entry, held is already past the grace, and MFO casts with
        // ZERO window for their AI. That is the one-shot bug again, per episode
        // instead of per session. Combat ending is the honest scope.
        if (!g_equipClock.empty()) {
            std::vector<RE::FormID> expired;
            for (const auto& [id, t] : g_equipClock) {
                auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
                if (!a || !a->IsInCombat()) expired.push_back(id);
            }
            for (const auto id : expired) g_equipClock.erase(id);
        }

        if (g_debt.empty()) return;
        const auto now  = std::chrono::steady_clock::now();
        const float hold = Config::g_twoHandedDebounce.load();

        // COLLECT, THEN ACT. EquipObject dispatches engine events synchronously;
        // mutating g_debt from inside its own iteration is the shape INVARIANTS
        // #2 exists to forbid (MEO's use-after-free).
        std::vector<RE::FormID> restoreAll;

        std::vector<RE::FormID> settled;

        for (auto& [id, debt] : g_debt) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
            if (!actor) continue;

            // THE DEBT MAY ALREADY BE PAID. Skyrim's combat AI re-evaluates
            // equipment and will happily take its own shield back mid-fight,
            // displacing MFO's spell. If we keep the entry open, the "already
            // owe this follower gear" guard turns every later Prepare into a
            // Debounce -- a sword-and-board healer heals ONCE PER FIGHT.
            if (debt.displacedLeft && actor->GetEquippedObject(true) == debt.displacedLeft) {
                debt.displacedLeft = nullptr;
                debt.leftWasShield = false;
            }
            if (debt.stowedWeapon && actor->GetEquippedObject(false) == debt.stowedWeapon) {
                debt.stowedWeapon = nullptr;
            }
            if (!debt.displacedLeft && !debt.stowedWeapon) {
                settled.push_back(id);    // MINOR: do not keep empty entries alive
                continue;
            }

            // Two-hander comes back once the cast has had time to happen.
            if (debt.stowedWeapon &&
                std::chrono::duration<float>(now - debt.stowedAt).count() >= hold) {
                restoreAll.push_back(id);
                continue;
            }

            // THE SHIELD BACKSTOP. §4.5b defers the shield to the moment they
            // are hit, because that is when it matters -- but §4.5b's FIRST
            // invariant is that MFO restores what it displaced, and a follower
            // who is never hit again would otherwise be permanently benched.
            // Combat ending is the honest deadline.
            if (debt.displacedLeft && !actor->IsInCombat()) {
                restoreAll.push_back(id);
            }
        }

        // Erase the clock alongside the debt. Leaving it behind is what made
        // the AI's window a one-shot: a follower who displaced nothing settles
        // immediately, the clock survives with its original timestamp, and
        // every later cast sees a huge elapsed time and skips the grace.
        for (const auto id : settled) { g_debt.erase(id); g_equipClock.erase(id); }

        for (const auto id : restoreAll) {
            if (const int n = RestoreOne(id); n > 0) {
                spdlog::info("[loadout] {:08X} restored {} displaced item(s)", id, n);
            }
        }
    }

    void Restore(RE::FormID a_actorID) {
        // Take our spell back before they leave -- the hit sink and every other
        // restore trigger is gated on them still being ours (#55).
        if (const auto sp = g_mfoSpell.find(a_actorID); sp != g_mfoSpell.end()) {
            if (auto* a = RE::TESForm::LookupByID<RE::Actor>(a_actorID)) {
                if (auto* s2 = RE::TESForm::LookupByID<RE::SpellItem>(sp->second);
                    s2 && a->GetEquippedObject(true) == s2) {
                    a->DeselectSpell(s2);
                }
            }
            g_mfoSpell.erase(sp);
        }
        g_coolUntil.erase(a_actorID);
        if (const int n = RestoreOne(a_actorID); n > 0) {
            spdlog::info("[loadout] {:08X} left the party -- restored {} item(s)", a_actorID, n);
        }
    }

    void Reconcile() {
        // LOAD-TIME RECONCILIATION. The ledger is deliberately NOT in the
        // co-save (#16: it describes live engine state, and a persisted entry
        // could re-equip gear the player has since sold). But "not persisted"
        // must not quietly become "not restored": a save taken mid-cast records
        // MFO's spell in the follower's hand and their shield in the pack.
        //
        // So rebuild the obligation from live state instead of storing it. Any
        // tracked follower holding a spell that one of THEIR OWN gambits names
        // is holding MFO's choice; unequip it and let their AI take its own
        // gear back.
        int fixed = 0;
        for (const auto& h : Followers::g_active) {
            auto* a = h.get().get();
            if (!a) continue;
            auto* left = a->GetEquippedObject(true);
            auto* spell = left ? left->As<RE::SpellItem>() : nullptr;
            if (!spell) continue;

            const auto rec = g_followers.find(a->GetFormID());
            if (rec == g_followers.end()) continue;

            bool ours = false;
            for (const auto& g : rec->second.combat()) {
                if (g.actionParamForm == spell->GetFormID()) { ours = true; break; }
            }
            if (!ours) continue;

            // DeselectSpell, NOT UnequipObject: a hand-held spell is
            // *selected*, not inventory-equipped, so UnequipObject looks up an
            // InventoryEntryData that does not exist and silently does nothing
            // -- while the log claims success. This is the call Papyrus's
            // UnequipSpell makes.
            a->DeselectSpell(spell);
            ++fixed;

            // MAJOR: removing the spell is only half the obligation. The ledger
            // died with the revert, so this is the ONLY restore agent left for
            // a save taken mid-cast -- and vanilla follower AI is notoriously
            // bad at re-equipping a player-given shield. Put it back ourselves.
            // Bounded and safe: we only reach here because MFO's own spell was
            // provably in that hand, and we only touch what is still in their
            // inventory, so nothing sold can be re-equipped (#16).
            if (!a->GetEquippedObject(true)) {
                RE::TESObjectARMO* shield = nullptr;
                auto inv = a->GetInventory([](RE::TESBoundObject& o) { return o.IsArmor(); });
                for (auto& [obj, data] : inv) {
                    auto* armo = obj ? obj->As<RE::TESObjectARMO>() : nullptr;
                    if (armo && armo->IsShield() && data.first > 0) { shield = armo; break; }
                }
                if (shield) {
                    EquipBack(a, shield);
                    spdlog::info("[loadout] reconciled {:08X} {} -- spell removed, shield restored",
                                 a->GetFormID(), a->GetName());
                    continue;
                }
            }
            spdlog::info("[loadout] reconciled {:08X} {} -- removed a spell matching one of their "
                         "own gambits (assumed MFO's, from a save taken mid-cast)",
                         a->GetFormID(), a->GetName());
        }
        if (fixed == 0) spdlog::info("[loadout] reconcile: nothing to undo");
    }

    void ClearTransientState() {
        g_debt.clear(); g_lastStow.clear(); g_equipClock.clear();
        g_coolUntil.clear(); g_mfoSpell.clear();
    }

    int PendingRestores() {
        int n = 0;
        for (const auto& [id, debt] : g_debt) {
            if (debt.stowedWeapon || debt.displacedLeft) ++n;
        }
        return n;
    }

    bool OwesLeft(RE::FormID a_actorID) {
        const auto it = g_debt.find(a_actorID);
        return it != g_debt.end() && it->second.displacedLeft != nullptr;
    }

}
