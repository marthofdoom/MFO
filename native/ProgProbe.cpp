#include "PCH.h"
#include "ProgProbe.h"
#include "Config.h"
#include "Followers.h"
#include "MainThread.h"

// The follower-progression sinker probes (P1/P2/P3). See ProgProbe.h for the
// contract; Docs/FOLLOWER-PROGRESSION-ESL-DESIGN.md §4.1/§4.2/§13 for why.
//
// Every log line carries the [prog] tag so a deck log tells the whole story
// on its own. Log the ZERO cases too — "found none" and "never ran" must not
// look the same (INVARIANTS #46).

namespace MFO::ProgProbe {

    namespace {

        // ── session state — MAIN THREAD ONLY, never serialized ──────────────
        // FormIDs, never pointers/handles: OnPostLoad re-resolves against the
        // loaded session (INVARIANTS #2), and a FormID survives the reload
        // that P3 exists to measure.
        RE::FormID g_followerID   = 0;
        RE::FormID g_perkID       = 0;   // the chosen perk (0 = none chosen)
        RE::FormID g_abilityID    = 0;   // its ability SPEL (0 = entry-point fallback)
        RE::FormID g_effectID     = 0;   // that ability's first MGEF
        bool       g_applied      = false;  // full probe ran this session
        int        g_presses      = 0;

        // P2 — §4.2 reconcile state. A KNOWN allocation on OneHanded so the
        // arithmetic is checkable by eye in the log.
        constexpr RE::ActorValue kProbeAV   = RE::ActorValue::kOneHanded;
        constexpr const char*    kProbeAVName = "OneHanded";
        constexpr float          kAlloc     = 10.0f;
        float g_lastWrittenBase = -1.0f;    // <0 = never written

        // Delayed re-check: ApplyPerksFromBase may not materialize an ability's
        // active effect the same frame, so the immediate read can honestly say
        // "not yet". Re-post through the pump for ~2s and read again.
        bool        g_delayedPending = false;
        int         g_delayedFrames  = 0;
        const char* g_delayedTag     = "";

        constexpr int kDelayFrames = 120;   // ~2s at 60fps

        // ── small helpers ───────────────────────────────────────────────────

        const char* NameOf(RE::TESForm* a_form) {
            if (!a_form) return "<none>";
            const char* n = a_form->GetName();
            return (n && *n) ? n : "<unnamed>";
        }

        RE::Actor* ResolveFollower() {
            if (!g_followerID) return nullptr;
            return RE::TESForm::LookupByID<RE::Actor>(g_followerID);
        }

        RE::BGSPerk* ResolvePerk() {
            if (!g_perkID) return nullptr;
            return RE::TESForm::LookupByID<RE::BGSPerk>(g_perkID);
        }

        RE::EffectSetting* ResolveEffect() {
            if (!g_effectID) return nullptr;
            return RE::TESForm::LookupByID<RE::EffectSetting>(g_effectID);
        }

        // How many times the perk appears in the BASE's perk-rank array. The
        // array is the thing an unguarded double-AddPerk would grow, so P3
        // counts it directly instead of trusting GetPerkIndex's yes/no.
        int CountInBase(RE::TESNPC* a_npc, RE::BGSPerk* a_perk) {
            if (!a_npc || !a_npc->perks || !a_perk) return 0;
            int n = 0;
            for (std::uint32_t i = 0; i < a_npc->perkCount; ++i)
                if (a_npc->perks[i].perk == a_perk) ++n;
            return n;
        }

        // Active-effect instances of the ability's MGEF on the actor. -1 =
        // unreadable (no magic target / no list) — distinct from a real 0.
        int CountEffect(RE::Actor* a_actor, RE::EffectSetting* a_mgef, int& a_outInactive) {
            a_outInactive = 0;
            if (!a_actor || !a_mgef) return -1;
            auto* mt = a_actor->AsMagicTarget();
            if (!mt) return -1;
            auto* list = mt->GetActiveEffectList();
            if (!list) return -1;
            int n = 0;
            for (auto* ae : *list) {
                if (!ae || ae->GetBaseObject() != a_mgef) continue;
                ++n;
                if (ae->flags.any(RE::ActiveEffect::Flag::kInactive)) ++a_outInactive;
            }
            return n;
        }

        // One status line shared by every phase, so before/after/delayed/P3
        // reads line up column-for-column in the log.
        void LogPerkStatus(const char* a_tag, RE::Actor* a_f) {
            auto* perk = ResolvePerk();
            auto* mgef = ResolveEffect();
            auto* base = a_f ? a_f->GetActorBase() : nullptr;
            if (!perk || !a_f || !base) {
                spdlog::info("[prog] {}: unresolvable (follower={}, perk={:08X})",
                             a_tag, a_f ? "ok" : "GONE", g_perkID);
                return;
            }
            const auto idx = base->GetPerkIndex(perk);
            int inactive = 0;
            const int effects = CountEffect(a_f, mgef, inactive);
            spdlog::info("[prog] {}: HasPerk(actor)={} baseIndex={} baseCount={} effectCount={}{}{}",
                         a_tag,
                         a_f->HasPerk(perk) ? "YES" : "no",
                         idx ? std::to_string(static_cast<int>(*idx)) : std::string("none"),
                         CountInBase(base, perk),
                         effects,
                         inactive ? std::format(" ({} inactive)", inactive) : "",
                         g_abilityID ? "" : "  [entry-point fallback: no ability observable]");
        }

        // ── P2: the §4.2 reconcile, logged wholesale ────────────────────────
        //
        //   cur     = GetBaseActorValue(av)
        //   natural = (cur == lastWrittenBase) ? lastWrittenBase - alloc : cur
        //   desired = min(natural + alloc, 100)
        //
        // "held" means the engine did NOT recompute the base since our last
        // write; anything else means autocalc/Requiem/level-up clobbered it
        // and the reconcile adopts the new natural. Either way the log shows
        // all four numbers, which is the entire point of P2.
        void ReconcileAV(const char* a_tag, RE::Actor* a_f) {
            auto* avo = a_f ? a_f->AsActorValueOwner() : nullptr;
            if (!avo) {
                spdlog::info("[prog] {}: no ActorValueOwner — P2 unreadable", a_tag);
                return;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            const int playerLevel = player ? static_cast<int>(player->GetLevel()) : -1;

            const float cur = avo->GetBaseActorValue(kProbeAV);
            if (g_lastWrittenBase < 0.0f) {
                // First write: adopt the current base as natural.
                const float natural = cur;
                const float desired = std::min(natural + kAlloc, 100.0f);
                avo->SetBaseActorValue(kProbeAV, desired);
                const float readback = avo->GetBaseActorValue(kProbeAV);
                g_lastWrittenBase = desired;
                spdlog::info("[prog] {} [{}]: BASELINE natural={:.2f} alloc=+{:.0f} wrote={:.2f} readback={:.2f} "
                             "current(effective)={:.2f} playerLevel={}",
                             a_tag, kProbeAVName, natural, kAlloc, desired, readback,
                             avo->GetActorValue(kProbeAV), playerLevel);
                if (readback != desired)
                    spdlog::warn("[prog] {} [{}]: readback {:.2f} != written {:.2f} — "
                                 "SetBaseActorValue did not stick even same-frame",
                                 a_tag, kProbeAVName, readback, desired);
                return;
            }

            const bool  held    = (cur == g_lastWrittenBase);
            const float natural = held ? g_lastWrittenBase - kAlloc : cur;
            const float desired = std::min(natural + kAlloc, 100.0f);
            spdlog::info("[prog] {} [{}]: cur={:.2f} lastWrittenBase={:.2f} -> {} | natural={:.2f} desired={:.2f} "
                         "current(effective)={:.2f} playerLevel={}",
                         a_tag, kProbeAVName, cur, g_lastWrittenBase,
                         held ? "HELD (engine did not clobber)" : "CLOBBERED (engine recomputed — adopting)",
                         natural, desired, avo->GetActorValue(kProbeAV), playerLevel);
            if (cur != desired) {
                avo->SetBaseActorValue(kProbeAV, desired);
                spdlog::info("[prog] {} [{}]: re-wrote base {:.2f} -> {:.2f} (readback {:.2f})",
                             a_tag, kProbeAVName, cur, desired, avo->GetBaseActorValue(kProbeAV));
            }
            g_lastWrittenBase = desired;
        }

        // ── the delayed re-check pump ───────────────────────────────────────
        void DelayedTick() {
            if (!g_delayedPending) return;   // reset by a load — drop silently
            if (--g_delayedFrames > 0) {
                MainThread::Post(DelayedTick);
                return;
            }
            g_delayedPending = false;
            auto* f = ResolveFollower();
            LogPerkStatus(g_delayedTag, f);
            if (g_abilityID) {
                int inactive = 0;
                const int effects = CountEffect(f, ResolveEffect(), inactive);
                spdlog::info("[prog] {}: VERDICT — ability effect on follower after settle: {}",
                             g_delayedTag,
                             effects > 0 ? "PRESENT (P1(b) entry point TOOK EFFECT on the NPC)"
                                         : "ABSENT (perk present but ability never materialized)");
            }
        }

        void ScheduleDelayedCheck(const char* a_tag) {
            g_delayedPending = true;    // overwrite any stale chain — last tag wins
            g_delayedFrames  = kDelayFrames;
            g_delayedTag     = a_tag;
            MainThread::Post(DelayedTick);
        }

        // ── follower pick ───────────────────────────────────────────────────
        // First unique-base, persistable teammate if one exists (the v1
        // enrollment shape, §4.1 shared-base guard), else the first teammate.
        // Every candidate is logged so the pick is auditable from the deck.
        RE::Actor* PickFollower() {
            RE::Actor* first = nullptr;
            RE::Actor* firstUnique = nullptr;
            int n = 0;
            for (const auto& h : Followers::g_active) {
                auto* a = h.get().get();
                if (!a) continue;
                ++n;
                auto* base = a->GetActorBase();
                const bool unique      = base && base->IsUnique();
                const bool persistable = Followers::IsPersistableID(a->GetFormID());
                spdlog::info("[prog]   teammate: {} ({:08X}, base {:08X}, unique={}, persistable={}, level {})",
                             NameOf(a), a->GetFormID(),
                             base ? base->GetFormID() : 0,
                             unique ? "yes" : "no", persistable ? "yes" : "NO (runtime id)",
                             static_cast<int>(a->GetLevel()));
                if (!first) first = a;
                if (!firstUnique && unique && persistable) firstUnique = a;
            }
            if (n == 0) {
                spdlog::info("[prog] no active teammates — recruit a follower first");
                return nullptr;
            }
            auto* pick = firstUnique ? firstUnique : first;
            spdlog::info("[prog] target: {} ({:08X}){}", NameOf(pick), pick->GetFormID(),
                         firstUnique ? "" : "  [WARNING: no unique-base teammate — using first; "
                                            "shared-base results may not generalize]");
            return pick;
        }

        // ── perk-tree walk (§2.1) ───────────────────────────────────────────
        // The follower's ACTUAL runtime AVIF tree — Requiem's replacements are
        // the live merged forms, so no FormID is ever hardcoded here.

        constexpr struct { RE::ActorValue av; const char* name; } kSkills[] = {
            { RE::ActorValue::kOneHanded,   "OneHanded" },
            { RE::ActorValue::kTwoHanded,   "TwoHanded" },
            { RE::ActorValue::kMarksman,    "Marksman" },
            { RE::ActorValue::kBlock,       "Block" },
            { RE::ActorValue::kHeavyArmor,  "HeavyArmor" },
            { RE::ActorValue::kLightArmor,  "LightArmor" },
            { RE::ActorValue::kDestruction, "Destruction" },
            { RE::ActorValue::kRestoration, "Restoration" },
            { RE::ActorValue::kConjuration, "Conjuration" },
            { RE::ActorValue::kAlteration,  "Alteration" },
            { RE::ActorValue::kIllusion,    "Illusion" },
            { RE::ActorValue::kSneak,       "Sneak" },
        };

        // BFS from the tree root. Visited check is a linear scan over the
        // queue — trees are ~100 nodes, and it keeps <unordered_set> out of a
        // TU that doesn't otherwise need it (the v1.0.8 CI lesson).
        std::vector<RE::BGSPerk*> CollectTree(RE::ActorValueInfo* a_avi) {
            std::vector<RE::BGSPerk*> out;
            if (!a_avi || !a_avi->perkTree) return out;
            std::vector<RE::BGSSkillPerkTreeNode*> queue{ a_avi->perkTree };
            for (std::size_t qi = 0; qi < queue.size() && queue.size() < 512; ++qi) {
                auto* node = queue[qi];
                if (!node) continue;
                if (node->perk) out.push_back(node->perk);
                for (auto* child : node->children) {
                    if (!child) continue;
                    if (std::find(queue.begin(), queue.end(), child) == queue.end())
                        queue.push_back(child);
                }
            }
            return out;
        }

        struct EntryCounts { int ability = 0; int entryPoint = 0; int quest = 0; int other = 0; };

        // Classify a perk's entries; hand back the first ability SPEL seen.
        // The int casts hedge GetType()'s exact return type across CommonLib
        // revisions — enum vs integer compares both ways.
        EntryCounts Classify(RE::BGSPerk* a_perk, RE::SpellItem** a_outAbility) {
            EntryCounts c;
            for (auto* entry : a_perk->perkEntries) {
                if (!entry) continue;
                const int t = static_cast<int>(entry->GetType());
                if (t == static_cast<int>(RE::PERK_ENTRY_TYPE::kAbility)) {
                    ++c.ability;
                    if (a_outAbility && !*a_outAbility)
                        *a_outAbility = static_cast<RE::BGSAbilityPerkEntry*>(entry)->ability;
                } else if (t == static_cast<int>(RE::PERK_ENTRY_TYPE::kEntryPoint)) {
                    ++c.entryPoint;
                } else if (t == static_cast<int>(RE::PERK_ENTRY_TYPE::kQuest)) {
                    ++c.quest;
                } else {
                    ++c.other;
                }
            }
            return c;
        }

        RE::EffectSetting* FirstEffect(RE::SpellItem* a_spell) {
            if (!a_spell) return nullptr;
            for (auto* eff : a_spell->effects)
                if (eff && eff->baseEffect) return eff->baseEffect;
            return nullptr;
        }

        // Walk the combat-skill trees in priority order; per skill log a
        // one-line census (the §3 filter will want these numbers anyway).
        // Choose the first not-yet-owned perk carrying an ABILITY entry whose
        // spell has a readable MGEF — the one entry type observable in a log.
        // If the whole load order offers none, fall back to the first unowned
        // ENTRY-POINT perk and say loudly that P1(b) is then unproven.
        RE::BGSPerk* ChoosePerk(RE::Actor* a_f, RE::TESNPC* a_base,
                                RE::SpellItem*& a_outAbility, RE::EffectSetting*& a_outEffect,
                                const char*& a_outSkill) {
            auto* avl = RE::ActorValueList::GetSingleton();
            if (!avl) {
                spdlog::warn("[prog] no ActorValueList singleton — cannot read AVIF trees");
                return nullptr;
            }

            RE::BGSPerk* abilityPick = nullptr;
            RE::BGSPerk* entryPointPick = nullptr;
            const char*  abilitySkill = nullptr;
            const char*  entryPointSkill = nullptr;

            for (const auto& sk : kSkills) {
                auto* avi = avl->GetActorValue(sk.av);
                auto perks = CollectTree(avi);
                EntryCounts total;
                int withAbility = 0;
                for (auto* p : perks) {
                    RE::SpellItem* ab = nullptr;
                    const auto c = Classify(p, &ab);
                    total.ability += c.ability;  total.entryPoint += c.entryPoint;
                    total.quest   += c.quest;    total.other      += c.other;
                    if (c.ability > 0) ++withAbility;

                    const bool owned = a_f->HasPerk(p) || a_base->GetPerkIndex(p).has_value();
                    if (owned) continue;
                    if (!abilityPick && c.ability > 0 && ab && FirstEffect(ab)) {
                        abilityPick = p;  a_outAbility = ab;  abilitySkill = sk.name;
                    }
                    if (!entryPointPick && c.entryPoint > 0) {
                        entryPointPick = p;  entryPointSkill = sk.name;
                    }
                }
                spdlog::info("[prog] tree {}: {} perk(s) | entries: {} entryPoint, {} ability ({} perk(s) carry one), "
                             "{} quest, {} other{}",
                             sk.name, perks.size(), total.entryPoint, total.ability, withAbility,
                             total.quest, total.other,
                             (avi && avi->perkTree) ? "" : "  [NO TREE on this AVIF]");
            }

            if (abilityPick) {
                a_outEffect = FirstEffect(a_outAbility);
                a_outSkill  = abilitySkill;
                return abilityPick;
            }
            if (entryPointPick) {
                spdlog::warn("[prog] NO unowned ability-entry perk in any combat tree — falling back to an "
                             "entry-point perk; P1(b) (effect takes hold) will be UNPROVEN by this probe");
                a_outAbility = nullptr;
                a_outEffect  = nullptr;
                a_outSkill   = entryPointSkill;
                return entryPointPick;
            }
            spdlog::warn("[prog] no candidate perk at all (every tree perk owned or empty?) — probe cannot run P1");
            return nullptr;
        }

        // ── the full probe (first press) ────────────────────────────────────
        void FullProbe() {
            auto* f = PickFollower();
            if (!f) return;
            auto* base = f->GetActorBase();
            if (!base) {
                spdlog::warn("[prog] follower has no TESNPC base — cannot probe");
                return;
            }

            RE::SpellItem*     ability = nullptr;
            RE::EffectSetting* mgef    = nullptr;
            const char*        skill   = "?";
            auto* perk = ChoosePerk(f, base, ability, mgef, skill);
            if (!perk) return;

            g_followerID = f->GetFormID();
            g_perkID     = perk->GetFormID();
            g_abilityID  = ability ? ability->GetFormID() : 0;
            g_effectID   = mgef ? mgef->GetFormID() : 0;

            spdlog::info("[prog] chosen perk: {} ({:08X}) from tree {} (numRanks={})",
                         NameOf(perk), g_perkID, skill, static_cast<int>(perk->data.numRanks));
            if (ability)
                spdlog::info("[prog] chosen ability: {} ({:08X}) -> observable effect {} ({:08X})",
                             NameOf(ability), g_abilityID, NameOf(mgef), g_effectID);

            // Would the player-identical gate (§5) pass right now? Pure data
            // for the enforcement design — the probe adds the perk either way.
            const bool gate = perk->perkConditions.IsTrue(f, f);
            spdlog::info("[prog] P1 gate: perkConditions.IsTrue(follower, follower) = {}", gate ? "yes" : "no");

            LogPerkStatus("P1 before", f);

            // THE SPID PATTERN (§4.1): base AddPerk, then apply to the actor.
            // Actor::AddPerk is a verified NPC no-op and is never called here.
            base->AddPerk(perk, 1);
            f->ApplyPerksFromBase();
            spdlog::info("[prog] P1 apply: TESNPC::AddPerk(rank 1) + Actor::ApplyPerksFromBase() issued");

            LogPerkStatus("P1 after (same frame)", f);
            ScheduleDelayedCheck("P1 after (~2s settle)");

            // P2 baseline write on the same follower.
            ReconcileAV("P2 baseline", f);

            g_applied = true;
            spdlog::info("[prog] NEXT STEPS: (1) level up, press the probe key again -> P2 verdict; "
                         "(2) save + reload -> P3 reapply verdict lands at kPostLoadGame.");
        }

        // ── status read (every later press) ─────────────────────────────────
        void StatusRead() {
            auto* f = ResolveFollower();
            if (!f) {
                spdlog::info("[prog] status: follower {:08X} did not resolve (dismissed/unloaded?)", g_followerID);
                return;
            }
            LogPerkStatus(std::format("status (press #{})", g_presses).c_str(), f);
            ReconcileAV("P2 reconcile", f);
        }

    }

    void OnHotkey() {
        if (!Config::g_progProbe.load()) return;
        ++g_presses;
        spdlog::info("[prog] ===== PROG PROBE — press #{} =====", g_presses);
        if (!g_applied) FullProbe();
        else            StatusRead();
    }

    void OnPostLoad() {
        // A pending delayed check from before the load would re-resolve
        // against the new session — drop it (its FormIDs are ours, but its
        // moment has passed; MainThread::Clear on revert drops the closure).
        g_delayedPending = false;

        if (!Config::g_progProbe.load() || !g_applied) return;

        spdlog::info("[prog] ===== P3 reapply (post-load) =====");
        auto* f    = ResolveFollower();
        auto* perk = ResolvePerk();
        auto* base = f ? f->GetActorBase() : nullptr;
        if (!f || !perk || !base) {
            spdlog::info("[prog] P3: unresolvable after load (follower={}, perk={}) — different save?",
                         f ? "ok" : "GONE", perk ? "ok" : "GONE");
            return;
        }

        LogPerkStatus("P3 before", f);

        // THE §4.1 PERSISTENCE SHAPE, exactly as the real feature will run it:
        // AddPerk only when the base lacks the perk; ApplyPerksFromBase only
        // on change. P3's question is whether this stays clean — the before/
        // after baseCount + effectCount answer it.
        const bool present = base->GetPerkIndex(perk).has_value();
        if (!present) {
            base->AddPerk(perk, 1);
            f->ApplyPerksFromBase();
            spdlog::info("[prog] P3 guard: perk was MISSING from base after load — re-added + re-applied "
                         "(base form mutations did not survive; reapply-on-load is mandatory)");
        } else {
            spdlog::info("[prog] P3 guard: perk already on base — AddPerk SKIPPED, ApplyPerksFromBase NOT called");
        }

        LogPerkStatus("P3 after", f);
        {
            const int baseCount = CountInBase(base, perk);
            int inactive = 0;
            const int effects = CountEffect(f, ResolveEffect(), inactive);
            spdlog::info("[prog] P3 VERDICT: baseCount={} effectCount={} -> {}",
                         baseCount, effects,
                         (baseCount <= 1 && effects <= 1)
                             ? "NO DOUBLING (guarded reapply is idempotent)"
                             : "DOUBLED — the guard is NOT sufficient, see counts");
        }
        ScheduleDelayedCheck("P3 after (~2s settle)");

        // Did the save round-trip preserve the actor's base AV, or restore the
        // natural? Either answer is P2 data — log the reconcile.
        ReconcileAV("P2 post-load", f);
    }

}
