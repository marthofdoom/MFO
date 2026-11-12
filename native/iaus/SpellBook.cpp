// iaus/SpellBook.cpp -- the per-follower main-thread mirror (contract and threading in iaus/SpellBook.h). Card I4.
#include "iaus/SpellBook.h"

#include "CasterConsent.h"
#include "Config.h"
#include "Followers.h"
#include "MainThread.h"
#include "PrecisionBridge.h"   // card I6 (marth Q64): the measured weapon length, main thread
#include "Vocabulary.h"
#include "cast/Actuation.h"
#include "iaus/Fills.h"
#include "logistics/Logistics.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace MFO::Iaus::SpellBook {

    namespace {

        using Clock = std::chrono::steady_clock;

        // Sized from the real cadence (principle 9). An IAUS follower reads his book once per service lap
        // (133 ms x party, about 0.5 s for four). Design 1.5: refresh every 5 s in combat. A lost refresh (a load
        // drops the pump's queue, MainThread::Clear) is re-asked after 3 s, so it can never wedge. The book is
        // trusted for 15 s = three missed refreshes; past that Read answers "no book" (and asks), so a fight that
        // starts minutes after the last one never runs on the old fight's stock and effect reads.
        constexpr double kRefreshSec  = 5.0;
        constexpr double kAskRetrySec = 3.0;
        constexpr double kTrustSec    = 15.0;

        struct Slot {
            std::shared_ptr<const Book> book;      // immutable once published
            Clock::time_point stamp{};             // when the last MAIN-thread measurement finished (epoch = never)
            Clock::time_point asked{};             // when the refresh in flight was posted (epoch = none)
            std::uint32_t     dirtySeq = 0;        // bumped by Invalidate / OnContainerChanged
            std::uint32_t     cleanSeq = 0;        // the dirtySeq the published book was measured at
            std::string       sig;                 // the last logged composition (log dedupe)
        };

        std::mutex                           g_mx;        // LEAF: map and ledger access only
        std::unordered_map<RE::FormID, Slot> g_books;
        std::unordered_set<RE::FormID>       g_dropLogged;   // once per spell per session
        std::unordered_set<RE::FormID>       g_probeLogged;  // the passive probes: once per spell / staff per session
        std::uint32_t                        g_gen = 0;      // bumped by Clear: a pre-Clear refresh is dropped

        double Age(const Clock::time_point& a_then, const Clock::time_point& a_now) {
            return std::chrono::duration<double>(a_now - a_then).count();
        }

        struct SpellVisitor final : RE::Actor::ForEachSpellVisitor {
            std::vector<RE::SpellItem*> out;
            RE::BSContainer::ForEachResult Visit(RE::SpellItem* a_spell) override {
                if (a_spell && std::find(out.begin(), out.end(), a_spell) == out.end()) out.push_back(a_spell);
                return RE::BSContainer::ForEachResult::kContinue;
            }
        };

        // MAIN THREAD: a live (not dispelled, not inactive) effect of a_spell on a_actor (SelfStatus's walk).
        bool EffectLive(RE::Actor* a_actor, const RE::MagicItem* a_spell) {
            auto* mt   = a_actor ? a_actor->AsMagicTarget() : nullptr;
            auto* list = mt ? mt->GetActiveEffectList() : nullptr;
            if (!list) return false;
            for (auto* ae : *list) {
                if (!ae || ae->spell != a_spell) continue;
                if (ae->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled)) continue;
                return true;
            }
            return false;
        }

        // MAIN THREAD. One spell or scroll record -> its entry. The NATURE is cast/'s classifiers' answer fed to
        // Prune::ClassifyNature; nothing here re-derives hostile / heal / buff.
        // PASSIVE PROBES (principle 5: a read nobody has watched run is not a fact yet). Measure collects one line
        // per area spell and per staff; PostRefresh logs each form ONCE per session, outside the leaf mutex.
        using Probes = std::vector<std::pair<RE::FormID, std::string>>;

        Entry MakeEntry(RE::Actor* a_actor, RE::SpellItem* a_sp, Source a_source, const ClassGate& a_gate,
                        const std::vector<RE::Actor*>& a_party, Probes& a_probes) {
            using Arch = RE::EffectArchetypes::ArchetypeID;
            Entry e;
            e.form   = a_sp->GetFormID();
            e.source = a_source;
            e.name   = a_sp->GetName() ? a_sp->GetName() : "?";

            const Actuation::SpellArchetype arch = Actuation::ClassifyArchetype(a_sp);
            const auto delivery = a_sp->GetDelivery();
            e.selfDelivery  = delivery == RE::MagicSystem::Delivery::kSelf;
            e.touch         = delivery == RE::MagicSystem::Delivery::kTouch;
            e.twoHanded     = arch.twoHanded;
            e.concentration = arch.casting == RE::MagicSystem::CastingType::kConcentration;

            Prune::NatureIn in;
            switch (CasterConsent::ClassifySpell(a_sp)) {
            case CasterConsent::SpellKind::Offense: in.kind = Prune::Kind::Offense; break;
            case CasterConsent::SpellKind::Heal:    in.kind = Prune::Kind::Heal; break;
            case CasterConsent::SpellKind::Buff:    in.kind = Prune::Kind::Buff; break;
            }
            float costliestHostile = -1.0f;
            // The BLAST: the effect's own explosion (MGEF data.explosion) and its projectile's (BGSProjectile
            // data.explosionType) carry the real damage radius in world units (BGSExplosion data.radius). It can
            // be larger than the EFIT area, and the never-splash rule must see the larger one.
            // MFO-B661 (card I6): an explosion with a PAYLOAD (BGSExplosion data.damage > 0, or an enchantment) is
            // tracked apart from a cosmetic one. Only a payload explosion can give an area to a spell whose EFIT
            // area is 0; a cosmetic impact explosion can at most enlarge an area the record already states.
            float explosionUnits = 0.0f;          // the largest radius of ANY explosion
            float payloadUnits = 0.0f;            // the largest radius of an explosion that carries a payload
            const char* explosionSrc = "none";
            const auto noteExplosion = [&](const RE::BGSExplosion* ex, const char* a_src) {
                if (!ex || !std::isfinite(ex->data.radius) || !(ex->data.radius > 0.0f)) return;
                const bool payload = (std::isfinite(ex->data.damage) && ex->data.damage > 0.0f) || ex->formEnchanting != nullptr;
                if (payload && ex->data.radius > payloadUnits) payloadUnits = ex->data.radius;
                if (ex->data.radius > explosionUnits) { explosionUnits = ex->data.radius; explosionSrc = a_src; }
            };
            for (auto* eff : a_sp->effects) {
                auto* base = eff ? eff->baseEffect : nullptr;
                if (!base) continue;
                switch (base->GetArchetype()) {
                case Arch::kTurnUndead: e.undeadOnly = true; [[fallthrough]];
                case Arch::kCalm:
                case Arch::kFrenzy:
                case Arch::kDemoralize:
                case Arch::kParalysis:  in.control = true; break;
                default: break;
                }
                noteExplosion(base->data.explosion, "effect");
                if (const auto* proj = base->data.projectileBase) noteExplosion(proj->data.explosionType, "projectile");
                // The element (design fact 7, section 3.1 exploit): the COSTLIEST hostile effect's resist AV.
                if ((base->IsHostile() || base->IsDetrimental()) && eff->cost > costliestHostile) {
                    costliestHostile = eff->cost;
                    const auto rv = base->data.resistVariable;
                    e.resistAv = rv == RE::ActorValue::kNone ? -1 : static_cast<int>(rv);
                }
            }
            in.summon           = Actuation::IsSummonSpell(a_sp);
            in.reanimate        = arch.shape == Actuation::SpellShape::Reanimate;
            in.ward             = arch.shape == Actuation::SpellShape::Ward;
            in.selfCloak        = arch.shape == Actuation::SpellShape::Cloak && e.selfDelivery;
            in.scriptCostliest  = arch.shape == Actuation::SpellShape::Script;
            in.natureUndeclared = Actuation::SpellNatureUndeclared(a_sp);
            e.nature = Prune::ClassifyNature(in);
            e.role   = Prune::RoleOf(e.nature, e.resistAv);
            if (e.nature != Prune::Nature::Offense && e.nature != Prune::Nature::Control) e.resistAv = -1;

            Prune::StaticIn st;
            st.castable = a_source == Source::Spell
                              ? Vocab::IsCastableSpell(a_sp)
                              : a_sp->GetCastingType() != RE::MagicSystem::CastingType::kConstantEffect;
            // The animation rule is about MFO's spell roads; a scroll is used through its own road (card S3).
            st.roadUnsupported = a_source == Source::Spell && Actuation::CastRoadUnsupported(a_sp);
            st.targetLocation  = delivery == RE::MagicSystem::Delivery::kTargetLocation ||
                                 arch.shape == Actuation::SpellShape::Rune ||
                                 arch.shape == Actuation::SpellShape::Wall ||
                                 arch.shape == Actuation::SpellShape::TLBurst;
            st.nature = e.nature;
            e.drop    = Prune::StaticDrop(st);

            e.meleeBuff = arch.row == Actuation::EngineRow::Armor || arch.row == Actuation::EngineRow::Cloak ||
                          arch.shape == Actuation::SpellShape::Cloak;
            // With an EFIT area any explosion may enlarge it (the larger radius is the safe one for the never-splash
            // rule); without one only a payload explosion counts (MFO-B661).
            e.areaUnits = Prune::BlastUnits(static_cast<float>(arch.area), arch.area > 0 ? explosionUnits : payloadUnits,
                                            payloadUnits > 0.0f);
            if (arch.area > 0 || explosionUnits > 0.0f)
                a_probes.emplace_back(e.form, std::format("area probe: {} ({:08X}): EFIT area {} ft = {:.0f} units, explosion radius {:.0f} units ({}), "
                                                          "payload explosion {:.0f} units -> blast {:.0f} units{}", e.name, e.form, arch.area,
                                                          Prune::AreaUnits(static_cast<float>(arch.area)), explosionUnits,
                                                          explosionSrc, payloadUnits, e.areaUnits,
                                                          (e.areaUnits <= 0.0f && explosionUnits > 0.0f)
                                                              ? " (cosmetic explosion: no area, MFO-B661)" : ""));
            // The aimed reach (spell range, else projectile range; Self / Touch = Heal Other's reach, which is
            // what a heal or buff placed on an ally needs). "No cap of its own" is a huge number there: 0 here.
            const float reach = Actuation::HealReach(a_sp);
            e.reach = (std::isfinite(reach) && reach > 0.0f && reach < 1.0e8f) ? reach : 0.0f;
            if (a_source == Source::Spell) {
                const float cost = a_sp->CalculateMagickaCost(a_actor);
                e.cost = (std::isfinite(cost) && cost > 0.0f) ? cost : 0.0f;
            }
            e.silent = Fills::CastsSilently(a_actor, a_sp);   // G2: the engine's cast-sound entry point (main thread, book cadence)
            e.onClass      = a_source == Source::Scroll ? a_gate.scrollUser : a_gate.spellsOnClass;
            e.fromMastered = a_gate.spellsFromMastered;
            if (e.nature == Prune::Nature::Summon) e.liveSummon = Actuation::CasterHasLiveSummon(a_actor, a_sp);
            if (e.drop == Prune::Drop::Keep &&
                (e.nature == Prune::Nature::Buff || e.nature == Prune::Nature::Ward))
                for (auto* p : a_party)
                    if (EffectLive(p, a_sp)) e.activeOn.push_back(p->GetFormID());
            // The engine fills (iaus/Fills.h): what one cast does to health, probe first, written only with
            // [Debug] bIausFillSpells = 1. With both keys off it returns at once.
            if (e.drop == Prune::Drop::Keep &&
                (e.nature == Prune::Nature::Offense || e.nature == Prune::Nature::Control || e.nature == Prune::Nature::Heal)) {
                std::string line;
                std::uint32_t key = 0;
                Fills::MeasureSpell(a_actor, a_sp, e, line, key);
                // The ledger key mixes follower, spell and "the perks ran", so each logs once per session.
                if (!line.empty()) a_probes.emplace_back(e.form ^ (a_actor->GetFormID() * 0x9E3779B1u) ^ key, std::move(line));
            }
            return e;
        }

        // MAIN THREAD. The current charge of one carried staff stack. An EQUIPPED staff's live charge is the
        // actor value of its hand; an unequipped one carries it in ExtraCharge, and a stack with no ExtraCharge
        // has never been used (full: the record's EAMT).
        float StaffCharge(RE::Actor* a_actor, RE::TESObjectWEAP* a_staff, const RE::InventoryEntryData* a_entry,
                          Probes& a_probes) {
            auto* avo = a_actor->AsActorValueOwner();
            {   // MFO-B656: every input of the charge read, so one field log settles which of them is the truth.
                const bool right = a_actor->GetEquippedObject(false) == a_staff;
                const bool left  = a_actor->GetEquippedObject(true) == a_staff;
                const float avR = avo ? avo->GetActorValue(RE::ActorValue::kRightItemCharge) : 0.0f;
                const float avL = avo ? avo->GetActorValue(RE::ActorValue::kLeftItemCharge) : 0.0f;
                bool  hasExtra = false;
                float extra = 0.0f;
                if (a_entry && a_entry->extraLists)
                    for (auto* xl : *a_entry->extraLists)
                        if (auto* xc = xl ? xl->GetByType<RE::ExtraCharge>() : nullptr) { hasExtra = true; extra = xc->charge; break; }
                auto* ench = a_staff->formEnchanting;
                const float per = ench ? ench->CalculateMagickaCost(a_actor) : 0.0f;
                a_probes.emplace_back(a_staff->GetFormID(),
                    std::format("staff probe: {} ({:08X}) on {:08X}: equipped={} AV RightItemCharge={:.1f} LeftItemCharge={:.1f} "
                                "ExtraCharge={} EAMT={} enchant={:08X} CalculateMagickaCost={:.1f}",
                                a_staff->GetName() ? a_staff->GetName() : "?", a_staff->GetFormID(), a_actor->GetFormID(),
                                right ? "right" : (left ? "left" : "no"), avR, avL,
                                hasExtra ? std::format("{:.1f}", extra) : std::string("absent"),
                                a_staff->amountofEnchantment, ench ? ench->GetFormID() : 0u, per));
            }
            if (avo && a_actor->GetEquippedObject(false) == a_staff)
                return avo->GetActorValue(RE::ActorValue::kRightItemCharge);
            if (avo && a_actor->GetEquippedObject(true) == a_staff)
                return avo->GetActorValue(RE::ActorValue::kLeftItemCharge);
            if (a_entry && a_entry->extraLists)
                for (auto* xl : *a_entry->extraLists)
                    if (auto* xc = xl ? xl->GetByType<RE::ExtraCharge>() : nullptr) return xc->charge;
            return static_cast<float>(a_staff->amountofEnchantment);
        }

        std::string Signature(const Book& a_b) {
            int n[7]{};
            int scrolls = 0, staves = 0, dropped = 0;
            for (const auto& e : a_b.entries) {
                if (e.source == Source::Scroll) { scrolls += e.count; continue; }
                if (e.source == Source::Staff) { staves += e.count; continue; }
                if (e.drop != Prune::Drop::Keep) { ++dropped; continue; }
                ++n[static_cast<int>(e.nature)];
            }
            const auto& k = a_b.kit;
            return std::format("spells offense={} control={} heal={} buff={} summon={} ward={} not-candidates={} | "
                               "scrolls={} staves={}{} | potions H{} S{} M{} | carries melee={} silver={} bow={} "
                               "crossbow={} arrows={} bolts={}",
                               n[1], n[2], n[3], n[4], n[5], n[6], dropped, scrolls, staves,
                               k.staff ? (k.staffCharged ? " (charged)" : " (EMPTY)") : "",
                               k.potionsHealth, k.potionsStamina, k.potionsMagicka, k.melee ? 1 : 0, k.silver ? 1 : 0,
                               k.bow ? 1 : 0, k.crossbow ? 1 : 0, k.arrows, k.bolts);
        }

        // MAIN THREAD ONLY.
        std::shared_ptr<const Book> Measure(RE::Actor* a_actor, Probes& a_probes) {
            auto book = std::make_shared<Book>();
            const RE::FormID  fid  = a_actor->GetFormID();
            const ClassGate   gate = GateFor(fid);

            // The party as the buff "already active" mirror sees it: himself, the player, the active followers.
            std::vector<RE::Actor*> party;
            const auto addParty = [&](RE::Actor* a) {
                if (a && a->Is3DLoaded() && !a->IsDead() && std::find(party.begin(), party.end(), a) == party.end())
                    party.push_back(a);
            };
            addParty(a_actor);
            addParty(RE::PlayerCharacter::GetSingleton());
            if (const auto snap = Followers::ActiveSnapshot())
                for (const RE::FormID id : *snap) addParty(RE::TESForm::LookupByID<RE::Actor>(id));

            // Card I6 (marth Q64): Precision's capsule length of the right-hand weapon. MEASURED, not scored (the
            // header of PrecisionBridge.h and the research note say why). PASSIVE PROBE: one line per follower
            // and right-hand weapon per session, with the numbers a ruling on the reach formula needs.
            {
                const float len = PrecisionBridge::RightWeaponLength(a_actor);
                book->kit.precisionWeaponLength = len;
                auto* rw = a_actor->GetEquippedObject(false);
                const RE::FormID wf = rw ? rw->GetFormID() : 0;
                float dist = -1.0f;
                if (auto tp = a_actor->GetActorRuntimeData().currentCombatTarget.get(); tp)
                    dist = a_actor->GetPosition().GetDistance(tp->GetPosition());
                // The ledger key mixes follower and weapon so each pair logs once (a FormID-shaped key, never a form).
                a_probes.emplace_back(fid ^ (wf * 0x9E3779B1u) ^ (dist >= 0.0f ? 0x50524543u : 0x50524542u),   // once without and once with a target
                    std::format("reach probe: {} ({:08X}) right hand {:08X} '{}': Precision capsule length {:.1f} units "
                                "(bridge {}), fMeleeReach {:.0f}, distance to his combat target {:.0f} (-1 = none). "
                                "Measured only: IAUS melee reach is still fMeleeReach (Q64)",
                                a_actor->GetName() ? a_actor->GetName() : "?", fid, wf,
                                rw && rw->GetName() ? rw->GetName() : "-", len, PrecisionBridge::State(),
                                Config::g_meleeReach.load(std::memory_order_relaxed), dist));
            }

            SpellVisitor vis;
            a_actor->VisitSpells(vis);
            for (auto* sp : vis.out) {
                if (!Vocab::IsCastableSpell(sp)) continue;   // abilities, powers, constant effects: never an entry
                book->entries.push_back(MakeEntry(a_actor, sp, Source::Spell, gate, party, a_probes));
            }

            std::vector<Entry> scrolls, staves;
            for (auto& [obj, data] : a_actor->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                if (auto* w = obj->As<RE::TESObjectWEAP>()) {
                    if (w->IsStaff()) {
                        Entry e;
                        e.form   = w->GetFormID();
                        e.source = Source::Staff;
                        e.name   = w->GetName() ? w->GetName() : "?";
                        e.count  = data.first;
                        e.onClass = !gate.strict || gate.weaponStaff;
                        e.fromMastered = false;
                        auto* ench = w->formEnchanting;
                        const float charge = StaffCharge(a_actor, w, data.second.get(), a_probes);
                        const float per    = ench ? ench->CalculateMagickaCost(a_actor) : 0.0f;
                        e.charge  = std::isfinite(charge) ? charge : 0.0f;
                        e.charged = ench && e.charge > 0.0f && (!std::isfinite(per) || e.charge >= per);
                        book->kit.staff = true;
                        if (e.charged) book->kit.staffCharged = true;
                        staves.push_back(std::move(e));
                    } else if (w->IsBow()) {
                        book->kit.bow = true;
                    } else if (w->IsCrossbow()) {
                        book->kit.crossbow = true;
                    } else if (w->IsMelee()) {
                        book->kit.melee = true;
                        if (Logistics::IsSilverMeleeWeapon(w)) book->kit.silver = true;
                    }
                    continue;
                }
                if (auto* sc = obj->As<RE::ScrollItem>()) {
                    Entry e = MakeEntry(a_actor, sc, Source::Scroll, gate, party, a_probes);
                    e.count = data.first;
                    scrolls.push_back(std::move(e));
                    continue;
                }
                if (auto* ammo = obj->As<RE::TESAmmo>()) {
                    (Logistics::AmmoIsBolt(ammo) ? book->kit.bolts : book->kit.arrows) += data.first;
                    continue;
                }
                if (auto* alc = obj->As<RE::AlchemyItem>()) {
                    // The SAME test Logistics::CountPotions makes (the drink executors' stock read).
                    switch (Logistics::PotionRestores(alc)) {
                    case RE::ActorValue::kHealth:  book->kit.potionsHealth += data.first; break;
                    case RE::ActorValue::kStamina: book->kit.potionsStamina += data.first; break;
                    case RE::ActorValue::kMagicka: book->kit.potionsMagicka += data.first; break;
                    default: break;
                    }
                }
            }
            for (auto& e : scrolls) book->entries.push_back(std::move(e));
            for (auto& e : staves) book->entries.push_back(std::move(e));
            return book;
        }

        void PostRefresh(RE::FormID a_fid, std::uint32_t a_gen, std::uint32_t a_dirtySeq) {
            MainThread::Post([a_fid, a_gen, a_dirtySeq]() {
                // A LOADED follower only: an unloaded one has no readable lists. No book is published then (the
                // slot's ask stamp ages out and the next Read asks again), never an empty book that would read
                // as "he knows no spells".
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_fid);
                std::shared_ptr<const Book> book;
                Probes probes;
                if (actor && actor->Is3DLoaded() && !actor->IsDead()) book = Measure(actor, probes);

                std::string sig;
                std::vector<std::string> drops;   // also carries the first-time probe lines
                bool logSig = false;
                if (book) sig = Signature(*book);
                {
                    std::scoped_lock lk(g_mx);
                    if (a_gen != g_gen) return;   // Clear ran while this was queued or running: drop it
                    auto& s = g_books[a_fid];
                    s.asked = Clock::time_point{};
                    if (!book) return;
                    s.book     = book;
                    s.stamp    = Clock::now();
                    s.cleanSeq = a_dirtySeq;
                    if (s.sig != sig) { s.sig = sig; logSig = true; }
                    for (const auto& e : book->entries)
                        if (e.source == Source::Spell && e.drop != Prune::Drop::Keep && g_dropLogged.insert(e.form).second)
                            drops.push_back(std::format("{} ({:08X}): nature={} -- no IAUS candidate: {}", e.name,
                                                        e.form, NatureName(e.nature), Prune::DropName(e.drop)));
                    for (auto& [form, line] : probes)
                        if (g_probeLogged.insert(form).second) drops.push_back(std::move(line));
                }
                // Logged OUTSIDE the leaf mutex.
                const char* name = actor->GetName() ? actor->GetName() : "?";
                if (logSig) spdlog::info("[spellbook] {} ({:08X}): {}", name, a_fid, sig);
                for (const auto& d : drops) spdlog::info("[spellbook] {}", d);
            });
        }

    }

    const char* NatureName(Prune::Nature a_n) {
        switch (a_n) {
        case Prune::Nature::Unknown: return "unknown";
        case Prune::Nature::Offense: return "offense";
        case Prune::Nature::Control: return "control";
        case Prune::Nature::Heal:    return "heal";
        case Prune::Nature::Buff:    return "buff";
        case Prune::Nature::Summon:  return "summon";
        case Prune::Nature::Ward:    return "ward";
        }
        return "?";
    }

    bool CastingClass(const Classes::ClassDef& a_def) {
        return a_def.line == Classes::Line::Mage || a_def.line == Classes::Line::Hybrid;
    }

    ClassGate GateFor(RE::FormID a_follower) {
        namespace W = Classes::Weapon;
        ClassGate g;
        const std::uint8_t eq = Classes::Active() ? Classes::GetEquipped(a_follower) : Classes::kNoClass;
        // Classes are on/off PER FOLLOWER: no equipped class means classes are OFF for this follower, so the
        // open (Q63) branch applies, as in ClassResolve / Carriers (field 2026-11: Auri, no class, lost all 35 spells).
        g.treeOn = Classes::Active() && eq < Classes::kClassCount;
        g.strict = Classes::Strict();
        const Classes::ClassDef* def = nullptr;
        Classes::Mask mastered = 0;
        if (g.treeOn) {
            if (eq != Classes::kNoClass && eq < Classes::kClassCount) def = &Classes::Get(static_cast<Classes::Id>(eq));
            mastered = Classes::GetMastered(a_follower);
        }
        if (!def) def = Classes::T1ForOverride(Followers::ResolvedClassFast(a_follower));
        if (!g.treeOn) {
            // THE CLASS TREE IS OFF: THE CLASS GATE RESTRICTS NOTHING (marth Q63, card I6; closes MFO-B657). Any
            // known spell, power attack, taunt and retreat are open, and no weapon category is gated, exactly as
            // the gambit table leaves them today. The row cooldowns stay 0 (no row): Generate applies the tuning
            // floors (Tuning::rowlessPowerCooldownSec / rowlessTauntCooldownSec). Scrolls keep ruling Q36 (tree
            // OFF: the Mage T1 uses them); no scroll candidate is generated yet (card S3).
            g.resolved      = def != nullptr;
            g.spellsOnClass = true;
            g.scrollUser    = def && CastingClass(*def);
            g.powerAttack   = true;
            g.taunt         = true;
            g.disengage     = true;
            return g;       // weaponMelee / Bow / Crossbow / Staff keep their open defaults
        }
        if (!def) return g;   // not resolved yet: nothing is class-gated (the defaults)
        g.resolved = true;

        g.spellsOnClass = CastingClass(*def);
        for (std::size_t i = 0; i < Classes::kClassCount; ++i) {
            const auto id = static_cast<Classes::Id>(i);
            if ((mastered & Classes::Bit(id)) != 0 && CastingClass(Classes::Get(id))) { g.spellsFromMastered = true; break; }
        }
        g.scrollUser = g.spellsOnClass;
        const bool any = (def->weapons & W::kUnrestricted) != 0;
        g.weaponMelee    = any || (def->weapons & (W::kDagger | W::kOneHand | W::kTwoHand)) != 0;
        g.weaponBow      = any || (def->weapons & W::kBow) != 0;
        g.weaponCrossbow = any || (def->weapons & W::kCrossbow) != 0;
        g.weaponStaff    = any || (def->weapons & W::kStaff) != 0;
        g.disengage   = (def->flags & Classes::Flag::kDisengage) != 0;
        g.sneakClass  = (def->flags & Classes::Flag::kSneakAlways) != 0;   // card G3: only a sneak class runs to hide
        g.powerAttack = def->powerShareMax != Classes::kShareUnset || def->powerShareMin != Classes::kShareUnset;
        for (std::size_t i = 0; i < def->gambitCount; ++i) {
            const auto& row = def->gambits[i];
            if (!row.act) continue;
            const std::string_view act{ row.act };
            if (act == Vocab::kActPowerAttack) {
                g.powerAttack = true;
                g.powerCooldownSec = (std::max)(g.powerCooldownSec, row.cooldownSec);
            } else if (act == Vocab::kActBlock) {
                g.block = true;   // batch I item 4 (MFO-B800): the class's Block row is what lets IAUS raise the guard
                g.blockCooldownSec = (std::max)(g.blockCooldownSec, row.cooldownSec);
            } else if (act == Vocab::kActTaunt) {
                g.taunt = true;
                g.tauntCooldownSec = (std::max)(g.tauntCooldownSec, row.cooldownSec);   // card I6: the row's own, 0 today
            }
        }
        return g;
    }

    std::shared_ptr<const Book> Read(RE::Actor* a_follower) {
        if (!a_follower || !MainThread::IsInstalled()) return nullptr;   // VR: no pump, no book (see the header)
        const RE::FormID fid = a_follower->GetFormID();
        const auto       now = Clock::now();
        std::shared_ptr<const Book> out;
        bool          ask = false;
        std::uint32_t gen = 0, seq = 0;
        {
            std::scoped_lock lk(g_mx);
            auto& s = g_books[fid];
            const bool measured = s.book && s.stamp.time_since_epoch().count() != 0;
            if (measured && Age(s.stamp, now) <= kTrustSec) out = s.book;
            const bool stale    = !measured || Age(s.stamp, now) >= kRefreshSec || s.dirtySeq != s.cleanSeq;
            const bool inFlight = s.asked.time_since_epoch().count() != 0 && Age(s.asked, now) < kAskRetrySec;
            if (stale && !inFlight) { s.asked = now; ask = true; gen = g_gen; seq = s.dirtySeq; }
        }
        if (ask) PostRefresh(fid, gen, seq);   // outside the mutex: Post takes its own queue lock
        return out;
    }

    void Invalidate(RE::FormID a_follower) {
        if (a_follower == 0) return;
        std::scoped_lock lk(g_mx);
        if (auto it = g_books.find(a_follower); it != g_books.end()) ++it->second.dirtySeq;
    }

    void OnContainerChanged(RE::FormID a_old, RE::FormID a_new) {
        std::scoped_lock lk(g_mx);
        if (g_books.empty()) return;
        if (auto it = g_books.find(a_old); a_old != 0 && it != g_books.end()) ++it->second.dirtySeq;
        if (auto it = g_books.find(a_new); a_new != 0 && it != g_books.end()) ++it->second.dirtySeq;
    }

    void Clear() {
        std::scoped_lock lk(g_mx);
        ++g_gen;
        g_books.clear();
        g_dropLogged.clear();
        g_probeLogged.clear();
    }

}
