#include "PCH.h"
#include "Followers.h"
#include "Loadout.h"
#include "Targeting.h"
#include "Packages.h"
#include "Forms.h"
#include "Config.h"
#include "Vocabulary.h"

namespace MFO::Followers {

    namespace {

        // EVERY follower's base gambits, applied ONCE when the record is first
        // created. NOT seeding-behind-a-test-flag (marth: nothing should be
        // seeded that can't be manually set) -- every op here is in the board's
        // pickers, so the player can edit, reorder or delete any of it. Sized to
        // the Rank I slots (2 combat, 4 logistics) so a co-save round-trip never
        // truncates it (#11). The co-save LOAD path assigns g_followers[] a
        // fully-populated record directly, so it never routes through here --
        // loaded edits are never overwritten.
        void ApplyDefaultKit(FollowerState& st) {
            auto add = [](std::vector<Gambit>& tab, const char* cond, float p, const char* act) {
                Gambit g{}; g.conditionOpcode = cond; g.conditionParam = p; g.actionOpcode = act;
                tab.push_back(g);
            };
            add(st.combat(), Vocab::kCondFoeLowestHp, 0.0f,  Vocab::kActAttack);  // fight the weakest foe
            add(st.combat(), Vocab::kCondAlways,      0.0f,  Vocab::kActWait);     // otherwise hold
            add(st.logistics(), Vocab::kCondSelfHpBelow,          0.50f, Vocab::kActDrinkHealthPotion);
            add(st.logistics(), Vocab::kCondSelfOutOfArrows,      10.0f, Vocab::kActLootArrows);
            add(st.logistics(), Vocab::kCondSelfLowHealthPotion,   2.0f, Vocab::kActLootPotions);
            add(st.logistics(), Vocab::kCondAlways,               0.0f,  Vocab::kActLootEquipment);
        }

        // ── quirk table (data/follower_quirks.json, hand-compiled) ──────────
        // Kept as code rather than parsed at runtime: no file paths to break
        // in-game, no parser dependency, and the table cannot drift from the
        // code consuming it (ARCHITECTURE.md §8.2).

        struct FactionQuirk {
            const char* name;
            const char* plugin;
            RE::FormID  localID;
            std::int32_t rank;          // -1 = "membership alone means dismissed"
            RE::TESFaction* resolved = nullptr;
        };

        // Inigo: sets PlayerTeammate but does NOT reliably clear it on
        // dismissal (DESIGN.md §3.1, corrected 2026-07-21). THE case the first
        // design draft got wrong -- the draft claimed he never sets it at all,
        // which is why the quirk AV below, not the teammate flag, is decisive.
        constexpr float kInigoDismissedAV = -1.0f;

        int g_quirksActive = 0, g_quirksInactive = 0;

        // Last time each follower was SEEN fighting. Sampled on the sweep, not
        // read at death time -- see SecondsSinceCombat's note (#51).
        std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_lastCombat;

        // Consecutive sweeps a follower may be missing before we believe it.
        // FIELD-FOUND 2026-07-21: the log showed `- id` then `+ id` 117ms apart,
        // i.e. a transient miss -- and because the death sink Refreshes before
        // awarding, a kill landing in that window credited NOBODY. One sweep is
        // not evidence of absence.
        constexpr int kMissesBeforeDrop = 3;
        std::unordered_map<RE::FormID, int> g_missStreak;

        FactionQuirk g_dismissedFactions[] = {
            { "Vilja",  "EMCompViljaSkyrim.esp", 0x0D6867, -1, nullptr },
            { "Tindra", "EMCompViljaSkyrim.esp", 0x1FB779,  0, nullptr },
        };

        // Reads THE SUBJECT ACTOR's actor values.
        bool HasDismissedActorValue(RE::Actor* a_actor) {
            auto* avo = a_actor->AsActorValueOwner();
            if (!avo) return false;
            const float v = avo->GetActorValue(RE::ActorValue::kWaitingForPlayer);
            // Compare with tolerance -- it is a float AV, not an int.
            return v < (kInigoDismissedAV + 0.5f);
        }

    }

    void ResolveQuirks() {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return;

        int found = 0, absent = 0;
        for (auto& q : g_dismissedFactions) {
            q.resolved = dh->LookupForm<RE::TESFaction>(q.localID, q.plugin);
            if (q.resolved) {
                ++found;
                spdlog::info("[follower] quirk active: {} ({} 0x{:06X})", q.name, q.plugin, q.localID);
            } else {
                ++absent;
                // An absent plugin is NORMAL, not an error. DYNAMIC_OR_DROP:
                // nothing is baked from this machine's load order.
                spdlog::debug("[follower] quirk inactive: {} -- {} not in load order", q.name, q.plugin);
            }
        }
        g_quirksActive   = found;
        g_quirksInactive = absent;
        spdlog::info("[follower] quirk table: {} active, {} inactive (of {})",
                     found, absent, std::size(g_dismissedFactions));
    }

    void ClearTransientState() {
        g_missStreak.clear();
        g_lastCombat.clear();
        g_activeIds.clear();
    }

    float SecondsSinceCombat(RE::FormID a_actorID) {
        const auto it = g_lastCombat.find(a_actorID);
        if (it == g_lastCombat.end()) return 1.0e9f;
        return std::chrono::duration<float>(std::chrono::steady_clock::now() - it->second).count();
    }

    int QuirksActive()   { return g_quirksActive; }
    int QuirksInactive() { return g_quirksInactive; }

    bool IsDismissedCustomFollower(RE::Actor* a_actor) {
        if (!a_actor) return false;   // nullptr is an ERROR, never a wildcard

        if (HasDismissedActorValue(a_actor)) return true;

        for (const auto& q : g_dismissedFactions) {
            if (!q.resolved) continue;
            if (q.rank < 0) {
                if (a_actor->IsInFaction(q.resolved)) return true;
            } else {
                if (a_actor->GetFactionRank(q.resolved, false) == q.rank &&
                    a_actor->IsInFaction(q.resolved)) {
                    return true;
                }
            }
        }
        return false;
    }

    bool IsEligibleFollower(RE::Actor* a_actor) {
        if (!a_actor) return false;

        // Cheap disqualifiers first (Swiftly Order Squad's shipped order).
        if (a_actor->IsDead())     return false;
        if (a_actor->IsDisabled()) return false;
        if (a_actor->IsDeleted())  return false;
        if (a_actor->IsPlayerRef()) return false;

        // Summons: eligible only when explicitly allowed, and NEVER persisted
        // (a runtime 0xFF FormID must not reach the co-save, INVARIANTS #9).
        if (a_actor->IsCommandedActor() && !Config::g_allowSummons) return false;

        if (!a_actor->IsPlayerTeammate()) return false;

        // ...then the quirks, which can revoke eligibility despite teammate
        // status. This ordering matters: teammate is the cheap gate, dismissal
        // is the expensive-but-authoritative one.
        if (IsDismissedCustomFollower(a_actor)) return false;

        return true;
    }

    bool IsTracked(RE::FormID a_actorID) {
        for (const auto& h : g_active) {
            if (auto* a = h.get().get(); a && a->GetFormID() == a_actorID) return true;
        }
        return false;
    }

    bool IsPersistableID(RE::FormID a_actorID) {
        // 0xFF is the runtime/dynamic form range. NEVER persist one
        // (INVARIANTS #9): next session the id dangles or has been REUSED by a
        // different created form, so the record silently attaches to whatever
        // now owns it.
        //
        // IsCommandedActor() alone does NOT cover this. A PlaceAtMe-spawned or
        // script-cloned teammate -- routine in a Lorerim-class order -- is not
        // "commanded" but still carries a 0xFF id. And the load side cannot
        // save us: SKSE's ResolveFormID passes 0xFF ids through as resolved.
        // So the guard has to be here, on the way in.
        return (a_actorID >> 24) != 0xFF;
    }

    FollowerState* TryEnsureRecord(RE::FormID a_actorID) {
        if (!IsPersistableID(a_actorID)) {
            spdlog::debug("[follower] {:08X} is a runtime (0xFF) form -- session-only, no record",
                          a_actorID);
            return nullptr;
        }
        auto [it, created] = g_followers.try_emplace(a_actorID);
        if (created) {
            ApplyDefaultKit(it->second);   // editable base gambits, every follower
            spdlog::info("[follower] {:08X} new record -- seeded {} combat / {} logistics defaults",
                         a_actorID, it->second.combat().size(), it->second.logistics().size());
        }
        return &it->second;
    }

    FollowerState& EnsureRecord(RE::FormID a_actorID) {
        // Callers that already know the id is persistable. Kept for the sites
        // where a record must exist; the guarded form is TryEnsureRecord.
        return g_followers[a_actorID];
    }

    void Refresh() {
        auto* pl = RE::ProcessLists::GetSingleton();
        if (!pl) {
            spdlog::warn("[follower] ProcessLists unavailable -- refresh skipped");
            return;
        }

        // Previous set, taken from the ID list rather than by re-resolving --
        // a handle that momentarily fails to resolve must still count as a
        // MISS (and get the hold), not silently disappear (F6).
        const std::vector<RE::FormID> before = g_activeIds;

        std::vector<RE::ActorHandle> next;
        std::vector<RE::FormID>      nextIds;
        for (auto& handle : pl->highActorHandles) {
            auto* actor = handle.get().get();
            if (!actor) continue;                  // unloaded between frames
            if (!IsEligibleFollower(actor)) continue;
            next.push_back(handle);
            nextIds.push_back(actor->GetFormID());
        }

        // Deltas, by name, so the log is testable per TEST_GUIDE 2A.
        for (const auto& h : next) {
            auto* a = h.get().get();
            if (!a) continue;
            const auto id = a->GetFormID();
            g_missStreak.erase(id);   // seen -- reset any streak
            if (a->IsInCombat()) g_lastCombat[id] = std::chrono::steady_clock::now();
            if (std::find(before.begin(), before.end(), id) == before.end()) {
                const bool isSummon    = a->IsCommandedActor();
                const bool persistable = IsPersistableID(id);
                spdlog::info("[follower] + {:08X} {} ({})", id, a->GetName(),
                             isSummon      ? "summon, session-only"
                             : !persistable ? "runtime form, session-only"
                                            : "teammate");
                // Two independent reasons to withhold a record. Checking only
                // IsCommandedActor was the gap: a cloned/spawned teammate is
                // not commanded but still has a 0xFF id.
                if (!isSummon) {
                    TryEnsureRecord(id);
                }
            }
        }
        for (const auto id : before) {
            bool still = false;
            for (const auto& h : next) {
                if (auto* a = h.get().get(); a && a->GetFormID() == id) { still = true; break; }
            }
            if (!still) {
                // Not seen this sweep -- but hold them until it repeats.
                const int misses = ++g_missStreak[id];
                if (misses < kMissesBeforeDrop) {
                    for (size_t i = 0; i < g_active.size() && i < g_activeIds.size(); ++i) {
                        if (g_activeIds[i] == id) { next.push_back(g_active[i]); nextIds.push_back(id); break; }
                    }
                    spdlog::debug("[follower] {:08X} missed sweep {}/{} -- holding",
                                  id, misses, kMissesBeforeDrop);
                    continue;
                }
                g_missStreak.erase(id);
                // Give back anything MFO put in their hands BEFORE we stop
                // tracking them -- the hit sink is gated on IsTracked, so after
                // this point nothing would ever restore it (#55).
                Loadout::Restore(id);
                // And the commanded-target latch -- same obligation, same
                // moment (#55). The hook does not check IsTracked, so a latch
                // left behind keeps redirecting an ex-follower AND keeps every
                // Character in combat worldwide off the fast path.
                Targeting::Clear(id);
                // And the package alias -- the SAME obligation as the two
                // above, but with a longer tail. #55 says restore before you
                // stop tracking; here the thing to restore is an alias fill
                // that the ENGINE SERIALIZES INTO THE .ess. A latch left on a
                // dismissed follower does not merely linger for the session,
                // it comes back on every future load of every save descended
                // from this one, and nothing in MFO will ever look at them
                // again to notice.
                Packages::Release(id);
                // Record is RETAINED -- dismissal must never destroy Rapport
                // (DESIGN.md §3.1, the emotional core of the progression).
                spdlog::info("[follower] - {:08X} (record and Rapport retained)", id);
            }
        }

        g_active   = std::move(next);
        g_activeIds = std::move(nextIds);

        // Log the zero case too, or "found none" and "never ran" look the same.
        spdlog::debug("[follower] refresh: {} active, {} record(s) stored",
                      g_active.size(), g_followers.size());
    }

}
