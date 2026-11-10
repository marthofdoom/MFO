// classes/Carriers.cpp -- the class passive carriers (card C6).
//
// WHAT IT DOES. MFO_Classes.esl ships ONE PERK per class passive whose entries are gated on the follower's rank in the class faction
// MFOC_P_<Name> (generator RANK SPACE: 0 off, 1..cap active at class level N, 100 mastered-and-unequipped = 1/4), plus ability SPELs for
// the two actor-value passives (Mercenary stamina, Scout speed). This file turns each follower's MCLS state into those ranks
// (CarrierRanks, pure), then, on the MAIN thread, (1) grants the perks ONCE per session to the follower's base NPC (TESNPC::AddPerk is a
// runtime base mutation, never persisted, so it is re-granted every session, presence-checked like progression/PerkGate.cpp's
// SetLivePerkRank), (2) writes the faction ranks (diff only), (3) adds / removes the ability spells. It is re-run at load
// (RefreshAllCarriers from MCLS CoLoad) and whenever a State.cpp writer succeeds (equip / level / mastery), so a class switch costs a
// few faction writes and at most two spell swaps: the perks are never churned.
//
// INERT without the ESL (Present()). With the ESL present but bClassTree = 0 the pass is a TEARDOWN: ranks zeroed, carrier abilities
// removed, no perk granted (an ability spell is save-persisted, a faction rank is too, so a disabled tree must not leave them behind).
//
// THREADING. RefreshCarriers / RefreshAllCarriers / ResetCarriers are callable from ANY thread and only touch g_pm (the pending set, the
// granted-perk memory, the pump generation); g_pm is NEVER held across an engine call. All actor / form work runs in PumpTick on the
// MAIN thread (MainThread::Post), which retries a follower whose Actor form or base NPC does not resolve (every kRetryFrames frames, kMaxTries times). A
// revert retires the pump by bumping g_gen (ResetCarriers via Classes::ClearAll), the Allocator poll-chain precedent.
//
// MISSING CARRIERS are logged once per session, loudly, naming the facet (kMissing). They are a documented degrade, not a mask: the
// class still works for every effect that has a road, and the log says exactly which does not.
#include "Classes_internal.h"
#include "Config.h"
#include "MainThread.h"
#include <bit>
#include <set>

namespace MFO::Classes {

    // ── ranks (pure) ──────────────────────────────────────────────────────────────────────────────
    std::array<std::uint8_t, kClassCount> CarrierRanks(const FollowerClasses& a_fc) {
        std::array<std::uint8_t, kClassCount> r{};
        // marth 2026-11-10: classes are ON / OFF PER FOLLOWER; equipped == kNoClass means OFF, so nothing at all applies, mastered
        // quarters included.
        if (a_fc.equipped >= kClassCount) return r;
        for (std::size_t c = 0; c < kClassCount; ++c)
            if (((a_fc.mastered >> c) & 1u) && LevelCap(static_cast<Id>(c)) != 0) r[c] = kRankMasteredUnequipped;
        const auto eq = static_cast<Id>(a_fc.equipped);
        if (eq == Id::Dragonborn) {   // no passive and no level of its own: every mastered passive at FULL (its cap)
            for (std::size_t c = 0; c < kClassCount; ++c)
                if (((a_fc.mastered >> c) & 1u) && LevelCap(static_cast<Id>(c)) != 0) r[c] = LevelCap(static_cast<Id>(c));
            return r;
        }
        const std::uint8_t cap = LevelCap(eq);
        if (cap == 0) return r;
        const std::uint8_t level = a_fc.progress[static_cast<std::size_t>(eq)].level;
        const bool mastered = ((a_fc.mastered >> static_cast<unsigned>(eq)) & 1u) != 0;
        r[static_cast<std::size_t>(eq)] = mastered ? cap : static_cast<std::uint8_t>(std::clamp<int>(level, 1, cap));
        const ClassDef& d = Get(eq);
        if (d.tier == 4) {   // a T4's T3 parents are at FULL (they are mastered by definition)
            Mask chosen = d.unlock.allOf & a_fc.mastered;
            auto fill = [&](Mask a_pool, unsigned a_need) {
                unsigned have = static_cast<unsigned>(std::popcount(chosen & a_pool));
                for (unsigned c = 0; c < kClassCount && have < a_need; ++c) {
                    const Mask b = Mask{ 1 } << c;
                    if ((a_pool & b) && (a_fc.mastered & b) && !(chosen & b)) { chosen |= b; ++have; }
                }
            };
            fill(d.unlock.poolA, d.unlock.needA);
            fill(d.unlock.poolB, d.unlock.needB);
            for (std::size_t c = 0; c < kClassCount; ++c)
                if (((chosen >> c) & 1u) && c != static_cast<std::size_t>(eq) && Get(static_cast<Id>(c)).tier == 3)
                    r[c] = LevelCap(static_cast<Id>(c));
        }
        return r;
    }

    namespace {
        // ── local ids in MFO_Classes.esl (generator CGID_*; the audit and Carriers resolve the same numbers) ──
        constexpr RE::FormID kFactionBase = 0x840;     // + Id
        constexpr RE::FormID kPerkBase    = 0xA10;     // + Id
        constexpr RE::FormID kMercFull = 0xA50, kMercQuarter = 0xA51;
        constexpr RE::FormID kScoutBase = 0xA60;       // + (level - 1), level 1..20
        constexpr RE::FormID kScoutQuarter = 0xA74;
        constexpr RE::FormID kMarkerSpellBase = 0xA80; // Marked, Flanked, Guarded
        constexpr int        kScoutLevels = 20;        // the T3 cap (Mastery.cpp LevelCap)

        // The classes that carry a perk (the generator's CLASS_PASSIVE_FX). A drift between this list and the ESL is logged by Resolve.
        constexpr std::array<Id, 21> kPerkClasses = {
            Id::Knight, Id::Warrior, Id::Archer, Id::Evoker, Id::Conjurer, Id::Healer, Id::Warlock, Id::GreatKnight,
            Id::Champion, Id::Blademaster, Id::Ranger, Id::MasterThief, Id::Wizard, Id::Priest, Id::Spellsword,
            Id::Sorcerer, Id::Templar, Id::Nightblade, Id::Witchhunter, Id::Scout, Id::LivingLegend };

        // Facets with NO carrier yet (logged once per session by LogMissingOnce). A road must be PROVEN before it is built: these are
        // the ones whose entry point is unverified on NPCs, whose marker / hook does not exist, or that need a later card.
        struct Missing { const char* cls; const char* facet; };
        constexpr Missing kMissing[] = {
            { "Thief", "sneaking movement 30% quieter: ModDetectionMovement (entry point 48) has one unconditioned vanilla entry, so its tabs are unproven and it cannot be rank-gated" },
            { "Conjurer", "low chance of being targeted by foes: an enemy-side target-weight seat does not exist" },
            { "Healer", "low chance of being targeted by foes: an enemy-side target-weight seat does not exist" },
            { "Necromancer", "+1 summon at a time (entry point 68 ModCommandedActorLimit is unproven on NPCs) and thralls +25% health (an ability on commanded actors at spawn)" },
            { "Wizard", "Destruction +20% on a controlled foe: no 'controlled' marker is defined or applied" },
            { "Priest", "undead / Daedra within 6 m take sun damage: the cloak and the MFOC_PriestSunDps runtime magnitude patch (card C11)" },
            { "Spellsword", "Spellstrike +10 element damage on melee hits (the other hand's element): no verified hit-spell road; the cost cut is NOT gated on a weapon in hand" },
            { "Sorcerer", "20% less damage taken while all summons are out: no 'all summons out' condition exists" },
            { "Templar", "weapon always counts as silver: the silver bonus road on NPCs is unverified" },
            { "Nightblade", "+15% melee damage on controlled foes: no 'controlled' marker is defined or applied" },
            { "Witchhunter", "Illusion bonus on CASTER targets (ruling: undead, Daedra AND casters; no vanilla-census-proven 'target is a spellcaster' condition), the Illusion immunity lift of undead / Daedra (spell target conditions), and the on-hit stamina / speed drain (no hit-spell road)" },
            { "Master Thief", "half-sneak-attack flanking rule (a DLL rule, card C13/C9, not a perk effect)" },
            { "Ranger", "the Mark gambit unlock (cards C12 / H-MARK)" },
            { "Blademaster", "all stamina costs -10%: only the power-attack stamina cost is carried (entry point 27)" },
            { "Arch-Mage", "unique: two simultaneous casts of different spells (the cast latch is actor-wide; needs a design pass)" },
            { "Living Legend", "the spark and sound cosmetic on an auto-block (ruling: cosmetic only)" },
            { "Nightingale", "unique Shadowcloak (a vanilla Invisibility cast, once per 30 s in a fight): no trigger logic yet" },
            { "Listener", "NEEDS-RE: Illusion up to the Listener's own level on non-boss foes (no entry function sets magnitude = level)" },
            { "Ebony Warrior", "unique Guardian: allies near it take 25% less, it takes that share (cloak + damage share)" },
            { "Grand Battlemage", "unique: every power attack also releases the off-hand spell (hit hook)" },
            { "Jaqspur", "NEEDS-RE: bow / crossbow draw speed +60% (which actor value or entry point drives NPC draw speed is unverified)" },
            { "Saint", "unique healing aura (+30 HP/s allies, 30/s undead): the cloak and the MCM runtime magnitude patch (card C11)" },
        };

        // ── resolved forms ────────────────────────────────────────────────────────────────────────
        struct Forms {
            bool resolved = false;   // Resolve ran (once per session)
            bool ok = false;         // every REQUIRED form resolved with the right type
            std::array<RE::TESFaction*, kClassCount> fact{};
            std::array<RE::BGSPerk*, kClassCount> perk{};
            RE::SpellItem* mercFull = nullptr;
            RE::SpellItem* mercQuarter = nullptr;
            RE::SpellItem* scoutQuarter = nullptr;
            std::array<RE::SpellItem*, kScoutLevels> scout{};
        };
        Forms g_forms;   // main thread only

        bool IsCarrierClass(Id a_id) {
            if (a_id == Id::Mercenary || a_id == Id::Scout) return true;
            return std::find(kPerkClasses.begin(), kPerkClasses.end(), a_id) != kPerkClasses.end();
        }

        void Resolve() {
            g_forms.resolved = true;
            auto* dh = RE::TESDataHandler::GetSingleton();
            const std::string file{ detail::AddonFileName() };
            if (!dh || file.empty()) {
                spdlog::error("[classes] carriers DISABLED: no data handler or the class add-on's file name is unknown");
                return;
            }
            int bad = 0, perks = 0, spells = 0, carriers = 0;
            auto want = [&](auto* a_form, RE::FormID a_local, const char* a_what) {
                if (!a_form) {
                    spdlog::error("[classes] carriers: {} (local id 0x{:03X}) not found or the wrong form type in {}", a_what, a_local, file);
                    ++bad;
                }
                return a_form;
            };
            for (std::size_t c = 0; c < kClassCount; ++c) {
                const auto id = static_cast<Id>(c);
                if (!IsCarrierClass(id)) continue;
                g_forms.fact[c] = want(dh->LookupForm<RE::TESFaction>(kFactionBase + static_cast<RE::FormID>(c), file),
                                       kFactionBase + static_cast<RE::FormID>(c), "class faction");
                if (id == Id::Mercenary) { ++carriers; continue; }   // ability only, no perk
                g_forms.perk[c] = want(dh->LookupForm<RE::BGSPerk>(kPerkBase + static_cast<RE::FormID>(c), file),
                                       kPerkBase + static_cast<RE::FormID>(c), "class passive perk");
                if (g_forms.perk[c]) ++perks;
                ++carriers;
            }
            g_forms.mercFull = want(dh->LookupForm<RE::SpellItem>(kMercFull, file), kMercFull, "Mercenary full ability");
            g_forms.mercQuarter = want(dh->LookupForm<RE::SpellItem>(kMercQuarter, file), kMercQuarter, "Mercenary quarter ability");
            g_forms.scoutQuarter = want(dh->LookupForm<RE::SpellItem>(kScoutQuarter, file), kScoutQuarter, "Scout quarter ability");
            for (int l = 0; l < kScoutLevels; ++l)
                g_forms.scout[static_cast<std::size_t>(l)] = want(dh->LookupForm<RE::SpellItem>(kScoutBase + static_cast<RE::FormID>(l), file),
                                                                 kScoutBase + static_cast<RE::FormID>(l), "Scout speed ability");
            for (auto* s : { g_forms.mercFull, g_forms.mercQuarter, g_forms.scoutQuarter }) if (s) ++spells;
            for (auto* s : g_forms.scout) if (s) ++spells;
            // The three marker carrier SPELs (B619) are nothing here uses yet; resolve them only to prove they are in the ESL.
            int markers = 0;
            for (RE::FormID i = 0; i < 3; ++i)
                if (want(dh->LookupForm<RE::SpellItem>(kMarkerSpellBase + i, file), kMarkerSpellBase + i, "marker carrier spell")) ++markers;
            g_forms.ok = (bad == 0);
            if (g_forms.ok)
                spdlog::info("[classes] carriers: {} carrier class(es) resolved from {} ({} perk(s), {} ability spell(s), {} marker spell(s))",
                             carriers, file, perks, spells, markers);
            else
                spdlog::error("[classes] carriers DISABLED: {} form(s) missing or mistyped in {} (the DLL and the ESL disagree; regenerate the ESL)",
                              bad, file);
        }

        void LogMissingOnce() {
            static std::atomic<bool> logged{ false };
            if (logged.exchange(true)) return;
            spdlog::warn("[classes] carriers: {} class facet(s) have NO carrier yet (documented degrade, the rest of each class works):", std::size(kMissing));
            for (const auto& m : kMissing) spdlog::warn("[classes] carrier MISSING -- {}: {}", m.cls, m.facet);
        }

        // ── live perk apply (a presence-checked copy of progression/PerkGate.cpp SetLivePerkRank, rank 0 -> 1) ──
        // Not shared: the progression family keeps it internal (ProgAllocator_internal.h) and a class card may not include that header.
        bool HasMiddleHigh(RE::Actor* a_actor) {
            auto* proc = a_actor ? a_actor->GetActorRuntimeData().currentProcess : nullptr;
            return proc && proc->middleHigh;
        }

        struct CountVisitor final : RE::PerkEntryVisitor {
            const RE::BGSPerkEntry* want = nullptr;
            std::uint32_t match = 0;
            RE::BSContainer::ForEachResult Visit(RE::BGSPerkEntry* a_entry) override {
                if (a_entry == want) ++match;
                return RE::BSContainer::ForEachResult::kContinue;
            }
        };

        // Returns the number of entries applied live. The engine applies the base's perks itself when the actor's process is built
        // (and Actor::FinishLoadGame at a load), so a follower with no process needs nothing here.
        int ApplyLive(RE::Actor* a_actor, RE::BGSPerk* a_perk) {
            if (!HasMiddleHigh(a_actor)) return 0;
            int applied = 0;
            for (auto* entry : a_perk->perkEntries) {
                if (!entry || static_cast<unsigned>(entry->header.rank) + 1u != 1u) continue;   // our perks have one rank
                if (entry->GetType() == RE::PERK_ENTRY_TYPE::kEntryPoint) {
                    auto* ep = static_cast<RE::BGSEntryPointPerkEntry*>(entry);
                    const auto point = static_cast<std::uint32_t>(ep->entryData.entryPoint.get());
                    CountVisitor v;
                    v.want = entry;
                    if (point < RE::BGSEntryPoint::ENTRY_POINTS::kTotal)
                        a_actor->ForEachPerkEntry(static_cast<RE::BGSEntryPoint::ENTRY_POINT>(point), v);
                    if (v.match > 0) continue;   // already live (the engine applied the base, or an earlier pass did)
                }
                entry->ApplyPerkEntry(a_actor);
                ++applied;
            }
            return applied;
        }

        // ── pending set + pump (any thread to queue, main thread to run) ────────────────────────────
        constexpr int kRetryFrames = 120;    // ~2 s at 60 fps between attempts on a follower that is not loaded
        constexpr int kMaxTries    = 600;    // ~20 min, then a WARN and the follower waits for its next state change or the next load
        struct Item { int tries = 0; int wait = 0; };
        std::mutex g_pm;
        std::unordered_map<RE::FormID, Item> g_pending;
        std::unordered_map<RE::FormID, Mask> g_granted;   // perks granted this session (Mask over Id), written on the main thread
        std::uint64_t g_gen = 0;
        bool g_pumpLive = false;
        std::set<std::uint64_t> g_loggedMismatch;         // (fid << 8 | class), main thread only

        enum class Result { Done, NotReady };

        // The ability spell a rank asks for (nullptr = none).
        RE::SpellItem* MercSpell(std::uint8_t a_rank) {
            if (a_rank == 0) return nullptr;
            return a_rank == kRankMasteredUnequipped ? g_forms.mercQuarter : g_forms.mercFull;
        }
        RE::SpellItem* ScoutSpell(std::uint8_t a_rank) {
            if (a_rank == 0) return nullptr;
            if (a_rank == kRankMasteredUnequipped) return g_forms.scoutQuarter;
            return g_forms.scout[static_cast<std::size_t>(std::clamp<int>(a_rank, 1, kScoutLevels) - 1)];
        }

        Result Reconcile(RE::FormID a_fid, bool a_teardown) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_fid);
            // No 3D needed: AddPerk is an array edit, AddToFaction / AddSpell work on an unloaded persistent ref, and ApplyLive no-ops
            // without a process (the engine applies the base's perks when the process is built, PerkGate.cpp contract).
            if (!actor) return Result::NotReady;
            auto* base = actor->GetActorBase();
            if (!base) return Result::NotReady;
            if (!g_forms.resolved) Resolve();
            if (!g_forms.ok) return Result::Done;   // already logged loudly; nothing can be applied
            if (!a_teardown) LogMissingOnce();

            std::array<std::uint8_t, kClassCount> ranks{};
            bool classesOn = false;   // equipped != kNoClass
            if (!a_teardown)
                if (auto fc = GetFollower(a_fid)) { ranks = CarrierRanks(*fc); classesOn = fc->equipped < kClassCount; }

            // 1. perks, ONCE per session per follower (the base mutation is runtime-only; a persisted base would not need it)
            int granted = 0, live = 0;
            // Granted only for a follower with classes ON (first class equip): a classes-off follower's base record is not touched.
            if (classesOn) {
                Mask have = 0;
                { std::scoped_lock lk(g_pm); have = g_granted[a_fid]; }
                for (Id id : kPerkClasses) {
                    const auto c = static_cast<std::size_t>(id);
                    if (!g_forms.perk[c] || ((have >> c) & 1u)) continue;
                    if (!base->GetPerkIndex(g_forms.perk[c]).has_value()) {
                        base->AddPerk(g_forms.perk[c], 1);
                        ++granted;
                    }
                    live += ApplyLive(actor, g_forms.perk[c]);
                    have |= Mask{ 1 } << c;
                }
                { std::scoped_lock lk(g_pm); g_granted[a_fid] = have; }
            }

            // 2. faction ranks (diff only; a class that is off and not a member is never added)
            int rankWrites = 0;
            for (std::size_t c = 0; c < kClassCount; ++c) {
                auto* f = g_forms.fact[c];
                if (!f) continue;
                const int want = a_teardown ? 0 : ranks[c];
                const int cur = actor->GetFactionRank(f, false);
                if (want == 0 ? cur <= 0 : cur == want) continue;
                actor->AddToFaction(f, static_cast<std::int8_t>(want));
                ++rankWrites;
                const int back = actor->GetFactionRank(f, false);
                if (back != want && !(want == 0 && back <= 0) && g_loggedMismatch.insert((std::uint64_t{ a_fid } << 8) | c).second)
                    spdlog::error("[classes] carriers {:08X}: faction MFOC_P_{} rank write {} read back as {} -- the class's passive gate is NOT "
                                  "where the DLL thinks it is", a_fid, Get(static_cast<Id>(c)).name, want, back);
            }

            // 3. ability spells (Mercenary, Scout): at most one each, the rest removed
            RE::SpellItem* wantMerc = a_teardown ? nullptr : MercSpell(ranks[static_cast<std::size_t>(Id::Mercenary)]);
            RE::SpellItem* wantScout = a_teardown ? nullptr : ScoutSpell(ranks[static_cast<std::size_t>(Id::Scout)]);
            int spellOps = 0;
            auto sync = [&](RE::SpellItem* a_s, bool a_want) {
                if (!a_s) return;
                const bool has = actor->HasSpell(a_s);
                if (a_want && !has) { actor->AddSpell(a_s); ++spellOps; }
                else if (!a_want && has) { actor->RemoveSpell(a_s); ++spellOps; }
            };
            sync(g_forms.mercFull, g_forms.mercFull == wantMerc);
            sync(g_forms.mercQuarter, g_forms.mercQuarter == wantMerc);
            sync(g_forms.scoutQuarter, g_forms.scoutQuarter == wantScout);
            for (auto* s : g_forms.scout) sync(s, s == wantScout);

            if (granted || rankWrites || spellOps) {
                std::string active;
                for (std::size_t c = 0; c < kClassCount; ++c)
                    if (ranks[c]) active += std::format(" {}={}", Get(static_cast<Id>(c)).slug, static_cast<int>(ranks[c]));
                spdlog::info("[classes] carriers {:08X} ({}){}: {} perk(s) granted ({} entr(ies) live), {} faction write(s), {} ability change(s); ranks:{}",
                             a_fid, actor->GetName() ? actor->GetName() : "?", a_teardown ? " TEARDOWN" : "", granted, live, rankWrites, spellOps,
                             active.empty() ? " none" : active);
            }
            return Result::Done;
        }

        void PumpTick(std::uint64_t a_gen) {
            std::vector<RE::FormID> due;
            {
                std::scoped_lock lk(g_pm);
                if (a_gen != g_gen) return;   // retired by ResetCarriers
                for (auto& [fid, it] : g_pending) {
                    if (it.wait > 0) --it.wait; else due.push_back(fid);
                }
            }
            const bool teardown = !Active();
            for (const RE::FormID fid : due) {
                const Result r = Reconcile(fid, teardown);
                std::scoped_lock lk(g_pm);
                if (a_gen != g_gen) return;
                auto it = g_pending.find(fid);
                if (it == g_pending.end()) continue;
                if (r == Result::Done) { g_pending.erase(it); continue; }
                it->second.wait = kRetryFrames;
                if (++it->second.tries > kMaxTries) {
                    spdlog::warn("[classes] carriers {:08X}: DROPPED after {} tries -- no Actor form (or no base NPC) resolves for this id; "
                                 "its class passives wait for its next class change or the next load", fid, kMaxTries);
                    g_pending.erase(it);
                }
            }
            {
                std::scoped_lock lk(g_pm);
                if (a_gen != g_gen) return;
                if (g_pending.empty()) { g_pumpLive = false; return; }
            }
            MainThread::Post([a_gen] { PumpTick(a_gen); });
        }
    }

    void RefreshCarriers(RE::FormID a_follower) {
        if (a_follower == 0 || !Present()) return;
        if (!MainThread::IsInstalled()) {
            static std::atomic<bool> warned{ false };
            if (!warned.exchange(true))
                spdlog::warn("[classes] carriers: the main-thread pump is not installed (VR) -- class passives are not applied");
            return;
        }
        std::uint64_t gen = 0;
        bool start = false;
        {
            std::scoped_lock lk(g_pm);
            g_pending[a_follower] = Item{};
            if (!g_pumpLive) { g_pumpLive = true; start = true; gen = g_gen; }
        }
        if (start) MainThread::Post([gen] { PumpTick(gen); });
    }

    void RefreshAllCarriers() {
        if (!Present()) return;
        for (const RE::FormID fid : detail::StoredFollowerIds()) RefreshCarriers(fid);
    }

    void ResetCarriers() {
        std::scoped_lock lk(g_pm);
        ++g_gen;
        g_pumpLive = false;
        g_pending.clear();
        g_granted.clear();
    }

#ifdef MFO_DEV_SELFTEST
    DevCarrierView DevReadCarriers(RE::FormID a_follower) {
        DevCarrierView v;
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_follower);
        auto* base  = actor ? actor->GetActorBase() : nullptr;
        if (!base) return v;
        if (!g_forms.resolved) Resolve();
        if (!g_forms.ok) return v;
        v.resolved    = true;
        v.processLive = HasMiddleHigh(actor);
        for (std::size_t c = 0; c < kClassCount; ++c) {
            if (auto* f = g_forms.fact[c]) v.ranks[c] = std::max(0, static_cast<int>(actor->GetFactionRank(f, false)));
            if (auto* p = g_forms.perk[c]) {
                ++v.perksExpected;
                if (base->GetPerkIndex(p).has_value()) ++v.perksOnBase;
                if (!v.processLive || !base->GetPerkIndex(p).has_value()) continue;
                for (auto* entry : p->perkEntries) {
                    if (!entry || static_cast<unsigned>(entry->header.rank) + 1u != 1u || entry->GetType() != RE::PERK_ENTRY_TYPE::kEntryPoint) continue;
                    auto* ep = static_cast<RE::BGSEntryPointPerkEntry*>(entry);
                    const auto point = static_cast<std::uint32_t>(ep->entryData.entryPoint.get());
                    if (point >= RE::BGSEntryPoint::ENTRY_POINTS::kTotal) continue;
                    ++v.entriesExpected;
                    CountVisitor cv;
                    cv.want = entry;
                    actor->ForEachPerkEntry(static_cast<RE::BGSEntryPoint::ENTRY_POINT>(point), cv);
                    if (cv.match > 0) ++v.entriesLive;
                }
            }
        }
        v.mercFull     = g_forms.mercFull && actor->HasSpell(g_forms.mercFull);
        v.mercQuarter  = g_forms.mercQuarter && actor->HasSpell(g_forms.mercQuarter);
        v.scoutQuarter = g_forms.scoutQuarter && actor->HasSpell(g_forms.scoutQuarter);
        for (int l = 0; l < kScoutLevels; ++l)
            if (g_forms.scout[static_cast<std::size_t>(l)] && actor->HasSpell(g_forms.scout[static_cast<std::size_t>(l)])) v.scoutLevel = l + 1;
        return v;
    }
#endif
}
