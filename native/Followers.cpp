#include "PCH.h"
#include "Followers.h"
#include "Forms.h"
#include "Config.h"

namespace MFO::Followers {

    namespace {

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

        // Inigo: dismissed state is WaitingForPlayer == -1. He never sets
        // PlayerTeammate. THE case the first design draft got wrong.
        constexpr float kInigoDismissedAV = -1.0f;

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
        spdlog::info("[follower] quirk table: {} active, {} inactive (of {})",
                     found, absent, std::size(g_dismissedFactions));
    }

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
        return &g_followers[a_actorID];
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

        // Snapshot the previous set so we can log deltas rather than state.
        std::vector<RE::FormID> before;
        before.reserve(g_active.size());
        for (const auto& h : g_active) {
            if (auto* a = h.get().get()) before.push_back(a->GetFormID());
        }

        std::vector<RE::ActorHandle> next;
        for (auto& handle : pl->highActorHandles) {
            auto* actor = handle.get().get();
            if (!actor) continue;                  // unloaded between frames
            if (!IsEligibleFollower(actor)) continue;
            next.push_back(handle);
        }

        // Deltas, by name, so the log is testable per TEST_GUIDE 2A.
        for (const auto& h : next) {
            auto* a = h.get().get();
            if (!a) continue;
            const auto id = a->GetFormID();
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
                // Record is RETAINED -- dismissal must never destroy Rapport
                // (DESIGN.md §3.1, the emotional core of the progression).
                spdlog::info("[follower] - {:08X} (record and Rapport retained)", id);
            }
        }

        g_active = std::move(next);

        // Log the zero case too, or "found none" and "never ran" look the same.
        spdlog::debug("[follower] refresh: {} active, {} record(s) stored",
                      g_active.size(), g_followers.size());
    }

}
