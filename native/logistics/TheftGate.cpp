// logistics/TheftGate.cpp -- the universal theft gate. See TheftGate.h for the rules and the RE note.
#include "Logistics_internal.h"
#include "TheftGate.h"
#include "Sightline.h"        // the guard's line of sight (MeasureNow / PointSeenNow, main thread)
#include "MainThread.h"
#include <array>
#include <mutex>

namespace MFO::Logistics::TheftGate {

    namespace {
        using SClock = std::chrono::steady_clock;

        // The guard radius: a guard farther than this from BOTH the follower and the source is not asked. 3500 u is about 50 m,
        // past the distance at which a guard in a street or a hall notices a sneaking player in the base game's detection
        // formula, and well inside the high-process list the scan walks. Inside it the line of sight decides.
        constexpr float kGuardRadius   = 3500.0f;
        constexpr auto  kGuardFresh    = std::chrono::milliseconds(2000);   // a cached guard verdict is trusted this long
        constexpr auto  kGuardUnreadMax = std::chrono::seconds(10);        // an answer nobody has read yet is usable once up to this age
        constexpr auto  kGuardRepost   = std::chrono::milliseconds(500);    // and re-asked no faster than this per (follower, src)
        constexpr auto  kLogEvery      = std::chrono::seconds(30);          // [theft-gate] per (follower, src, verdict)
        constexpr RE::FormID kPlayerBaseNpc = 0x00000007;
        constexpr RE::FormID kPlayerFaction = 0x00000DB1;

        const char* NameOf(const RE::TESForm* a_f) {
            const char* n = a_f ? a_f->GetName() : nullptr;
            return n && *n ? n : "?";
        }

        // The owner of a copy is the follower's side ONLY when it is the player's base NPC, PlayerFaction, or the follower's own base NPC
        // (or its template chain); no faction he merely belongs to (a guild's stock is not his). ExtraOwnership.owner is a BASE form
        // (TESNPC / TESFaction). A data-only read, so no seat row. Anything else is someone else's: stolen goods, or an owned container's.
        bool OtherOwner(RE::TESForm* a_owner, RE::Actor* a_follower) {
            if (!a_owner) return false;
            const RE::FormID id = a_owner->GetFormID();
            if (id == kPlayerBaseNpc || id == kPlayerFaction) return false;
            RE::TESForm* b = a_follower->GetActorBase();
            for (int i = 0; b && i < 8; ++i) {
                if (b == a_owner) return false;
                auto* npc = b->As<RE::TESNPC>();
                b = npc ? npc->baseTemplateForm : nullptr;
            }
            return true;   // any other NPC or faction, the follower's own factions included (marth 2026-11-11: "bar all stolen")
        }

        // ── the rate-limited proof line ──
        std::mutex g_logMx;
        std::unordered_map<std::string, SClock::time_point> g_logAt;

        void Say(RE::Actor* a_f, Road a_road, RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj, std::int32_t a_n,
                 RE::TESForm* a_owner, const char* a_via, const char* a_barred, bool a_pending = false) {
            const RE::FormID fid = a_f ? a_f->GetFormID() : 0u;
            const RE::FormID sid = a_src ? a_src->GetFormID() : 0u;
            const std::string verdict = a_pending ? std::string("pending") : a_barred ? std::string("BARRED:") + a_barred : std::string("take");
            const std::string key = std::format("{:08X}|{:08X}|{}", fid, sid, verdict);
            {
                std::scoped_lock lk(g_logMx);
                const auto now = SClock::now();
                if (g_logAt.size() > 512) g_logAt.clear();
                auto& at = g_logAt[key];
                if (at.time_since_epoch().count() != 0 && now - at < kLogEvery) return;
                at = now;
            }
            auto* sb = a_src ? a_src->GetBaseObject() : nullptr;
            spdlog::info("[theft-gate] F={:08X} road={} src={:08X}({:08X}) item={:08X}x{} owner={:08X} via={} offLimits(pc)={} locked={} verdict={}",
                         fid, RoadName(a_road), sid, sb ? sb->GetFormID() : 0u, a_obj ? a_obj->GetFormID() : 0u, a_n,
                         a_owner ? a_owner->GetFormID() : 0u, a_via ? a_via : "-", a_src && a_src->IsOffLimits() ? 1 : 0, a_src && a_src->IsLocked() ? 1 : 0, verdict);
        }

        // ── the action ring ──
        struct Act { Road road = Road::Loose; RE::FormID ref = 0; SClock::time_point at{}; const char* result = ""; };
        struct Ring { std::array<Act, 3> a{}; std::uint32_t n = 0; };
        std::mutex g_ringMx;
        std::unordered_map<RE::FormID, Ring> g_rings;

        // ── the guard cache ──
        struct GuardAns { SClock::time_point at{}, posted{}; bool seen = true; RE::FormID guard = 0; const char* how = "pending"; bool have = false; bool unread = false; SClock::time_point readAt{}; };   // unread: posted by a reader that has not seen it yet; readAt: when a gate first USED this answer (one verdict per visit)
        std::mutex g_gMx;
        std::unordered_map<std::uint64_t, GuardAns> g_guard;   // (follower, src) -> the last scan

        // The calling thread's own count of non-Clear GuardCheck results (thread_local: no sharing, no lock). A caller that wants to know
        // whether ANY gate inside a call chain refused (StripCorpse: its LootHere / Take gates) snapshots n before and after.
        thread_local GuardRefusal t_lastRefusal{};

        std::uint64_t GKey(RE::FormID a_f, RE::FormID a_src) { return (static_cast<std::uint64_t>(a_f) << 32) | a_src; }

        // MAIN THREAD (or the VR no-pump caller). Does any live guard in the high-process list see the follower or the source?
        // Seen = Visible, or Unknown (the sight could not be measured: fail closed). Teammates and the player are never guards here.
        void ScanGuards(RE::FormID a_fid, RE::FormID a_srcId) {
            GuardAns ans; ans.at = SClock::now(); ans.posted = ans.at; ans.have = true; ans.unread = true; ans.seen = false; ans.how = "none";
            auto* f   = RE::TESForm::LookupByID<RE::Actor>(a_fid);
            auto* src = a_srcId ? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_srcId) : nullptr;
            auto* pl  = RE::ProcessLists::GetSingleton();
            if (!f || !pl) { ans.seen = true; ans.how = "unreadable"; }
            else {
                const auto fpos = f->GetPosition();
                const auto spos = src ? src->GetPosition() : fpos;
                for (auto& hh : pl->highActorHandles) {
                    auto  ptr = hh.get();
                    auto* g   = ptr.get();
                    if (!g || g == f || !g->IsGuard()) continue;
                    if (g->IsPlayerRef() || g->IsPlayerTeammate()) continue;
                    if (g->IsDead() || g->IsDisabled() || !g->Is3DLoaded()) continue;
                    const auto  gpos = g->GetPosition();
                    const float df = gpos.GetDistance(fpos), ds = src ? gpos.GetDistance(spos) : 1.0e9f;
                    if (df > kGuardRadius && ds > kGuardRadius) continue;
                    if (df <= kGuardRadius) {
                        const auto v = Sightline::MeasureNow(g->GetFormID(), a_fid, Sightline::Basis::Engine);
                        if (v != Sightline::Verdict::Occluded) {
                            ans.seen = true; ans.guard = g->GetFormID();
                            ans.how = v == Sightline::Verdict::Visible ? "guard sees follower" : "guard in range, sight unmeasurable";
                            break;
                        }
                    }
                    if (src && ds <= kGuardRadius) {
                        const auto ps = Sightline::PointSeenNow(g, spos, 40.0f);
                        if (ps != Sightline::PointSight::Hidden) {
                            ans.seen = true; ans.guard = g->GetFormID();
                            ans.how = ps == Sightline::PointSight::Seen ? "guard sees the item" : "guard in range, item sight unmeasurable";
                            break;
                        }
                    }
                }
            }
            std::scoped_lock lk(g_gMx);
            if (g_guard.size() > 256) g_guard.clear();
            g_guard[GKey(a_fid, a_srcId)] = ans;
        }
    }

    const char* RoadName(Road a_road) {
        switch (a_road) {
            case Road::Loose: return "loose"; case Road::Flora: return "flora"; case Road::Container: return "container";
            case Road::Corpse: return "corpse"; case Road::Swap: return "swap"; case Road::Gear: return "gear";
            case Road::Lotd: return "lotd"; case Road::Door: return "door"; case Road::Guard: return "guard";
            case Road::Steal: return "steal"; case Road::Pick: return "pick";
        }
        return "?";
    }

    void LogOwnedCopy(RE::Actor* a_follower, RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj, std::int32_t a_count, const char* a_how) {
        auto* own = a_src ? a_src->GetOwner() : nullptr;
        Say(a_follower, Road::Steal, a_src, a_obj, a_count, own, a_how ? a_how : "-", nullptr);   // verdict=take; road=steal says WHICH road moved it
        spdlog::info("[theft-gate] F={:08X} owned copy of {:08X}x{} moved by the STEAL road (witness pre-check Take, re-proved on main), not by Take",
                     a_follower ? a_follower->GetFormID() : 0u, a_obj ? a_obj->GetFormID() : 0u, a_count);
    }

    void NoteAction(RE::FormID a_follower, Road a_road, RE::FormID a_ref, const char* a_result) {
        std::scoped_lock lk(g_ringMx);
        if (g_rings.size() > 64) g_rings.clear();
        auto& r = g_rings[a_follower];
        if (r.n > 0) {   // the same action again only refreshes the newest slot: a loop must not push the real action out of the 3
            auto& last = r.a[(r.n - 1) % 3];
            if (last.road == a_road && last.ref == a_ref && std::string_view(last.result) == std::string_view(a_result ? a_result : "")) { last.at = SClock::now(); return; }
        }
        r.a[r.n % 3] = Act{ a_road, a_ref, SClock::now(), a_result ? a_result : "" };
        ++r.n;
    }

    void LogRings() {
        std::scoped_lock lk(g_ringMx);
        const auto now = SClock::now();
        for (auto& [fid, r] : g_rings) {
            const std::uint32_t cnt = std::min<std::uint32_t>(r.n, 3);
            for (std::uint32_t i = 0; i < cnt; ++i) {   // newest first
                const auto& a = r.a[(r.n - 1 - i) % 3];
                spdlog::info("[crime-probe]   last MFO action F={:08X} #{}: road={} ref={:08X} {:.1f}s ago result={}", fid, i + 1,
                             RoadName(a.road), a.ref, std::chrono::duration<float>(now - a.at).count(), a.result);
            }
        }
    }

    GuardState GuardCheck(RE::Actor* a_follower, RE::TESObjectREFR* a_src, Road a_road, bool a_onMain, bool a_peek) {
        if (!a_follower) return GuardState::Seen;
        const RE::FormID fid = a_follower->GetFormID(), sid = a_src ? a_src->GetFormID() : 0u;
        if (a_onMain) ScanGuards(fid, sid);   // the caller is ALREADY on the main thread (a posted lambda): answer now, never "pending"
        const auto now = SClock::now();
        bool post = false, have = false, seen = true;
        const char* how = "pending";
        RE::FormID guard = 0;
        {
            std::scoped_lock lk(g_gMx);
            auto& e = g_guard[GKey(fid, sid)];
            // Fresh = younger than the TTL, OR never read yet: the service round-robin (N x 133 ms per follower, over 2 s from 16 actives) can
            // revisit a follower later than the TTL, and an answer that was posted for that very reader must still be usable once.
            // ONE VERDICT PER (follower, source) FOR kGuardFresh FROM ITS FIRST USE: a visit reads this key several times (StripCorpse,
            // LootHere, Take). The first gate that USES an answer stamps readAt; the later reads of the same key accept it for kGuardFresh from
            // that stamp even when the answer itself is older than the TTL by then (it is not tied to a visit boundary: a revisit within
            // that window reuses it too). It only re-uses a verdict a gate already acted on: it never turns "no answer" into a take.
            // PEEK (the approach prefetch) reads the cache but stamps nothing, so a take is never authorised by an answer taken at the
            // approach position: the first arrival gate starts the visit.
            const bool freshAns = e.have && (now - e.at < kGuardFresh || (e.unread && now - e.at < kGuardUnreadMax));
            const bool sameVisit = e.have && e.readAt.time_since_epoch().count() != 0 && now - e.readAt < kGuardFresh;
            if (freshAns || sameVisit) {
                have = true; seen = e.seen; how = e.how; guard = e.guard;
                if (!a_peek) {
                    e.unread = false;
                    if (!sameVisit) e.readAt = now;
                }
            }
            if (!have && (e.posted.time_since_epoch().count() == 0 || now - e.posted >= kGuardRepost)) { e.posted = now; post = true; }
        }
        if (post) {
            if (MainThread::IsInstalled()) MainThread::Post([fid, sid]() { ScanGuards(fid, sid); });
            else                           ScanGuards(fid, sid);   // VR: no pump, the other main-thread reads fall back the same way
        }
        if (a_peek) return GuardState::Pending;   // prefetch: the scan may be posted, nothing else (no stamp, no log, no refusal record)
        if (!have) {   // no fresh answer yet: the caller must retry next tick, never treat this as a verdict (no blocklist, no travel-fail)
            ++t_lastRefusal.n; t_lastRefusal.state = GuardState::Pending; t_lastRefusal.guard = 0;
            Say(a_follower, Road::Guard, a_src, nullptr, 0, nullptr, how, "guard", /*a_pending*/ true);
            return GuardState::Pending;
        }
        if (!seen) return GuardState::Clear;
        ++t_lastRefusal.n; t_lastRefusal.state = GuardState::Seen; t_lastRefusal.guard = guard;
        auto* g = guard ? RE::TESForm::LookupByID<RE::Actor>(guard) : nullptr;
        Say(a_follower, a_road == Road::Loose || a_road == Road::Flora ? a_road : Road::Guard, a_src, nullptr, 0,
            g ? static_cast<RE::TESForm*>(g->GetActorBase()) : nullptr, how, "guard");
        return GuardState::Seen;   // a guard bar is a verdict, not a world action: it never enters the ring
    }

    GuardRefusal GuardRefusalInfo() { return t_lastRefusal; }

    bool GuardInterim(RE::Actor* a_follower, RE::TESObjectREFR* a_src, Road a_road) {
        return GuardCheck(a_follower, a_src, a_road, false) != GuardState::Clear;
    }

    const char* TakeBarred(RE::Actor* a_follower, RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj,
                           RE::ExtraDataList* a_xList, Road a_road) {
        (void)a_obj; (void)a_road;
        if (!a_follower || !a_src) return "no follower or source";
        if (a_xList) {
            if (auto* own = a_xList->GetByType<RE::ExtraOwnership>(); own && own->owner)
                return OtherOwner(own->owner, a_follower) ? "this copy is owned by someone else (stolen goods)" : nullptr;
        }
        if (auto* co = a_src->GetOwner(); co && OtherOwner(co, a_follower)) return "the container's owner is someone else";
        return nullptr;
    }

    namespace {
        struct Cp { RE::ExtraDataList* xl; std::int32_t n; const char* bar; RE::TESForm* owner; };
        struct Plan {
            std::vector<Cp> copies;
            std::int32_t clean = 0;            // copies with no extra list
            const char*  cleanBar = nullptr;   // why the plain copies are barred
            bool anyBarred = false;            // any copy (extra-listed or plain) barred
            RE::TESForm* contOwner = nullptr;
            bool found = false;
        };
        Plan Analyse(RE::Actor* a_f, RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj, Road a_road) {
            Plan p;
            auto inv = a_src->GetInventory([&](RE::TESBoundObject& x) { return &x == a_obj; });
            auto it = inv.find(a_obj);
            if (it == inv.end() || it->second.first <= 0) return p;
            p.found = true;
            p.contOwner = a_src->GetOwner();
            std::int32_t xlTotal = 0;
            auto* e = it->second.second.get();
            if (e && e->extraLists)
                for (auto* xl : *e->extraLists) {
                    if (!xl) continue;
                    auto* cnt = xl->GetByType<RE::ExtraCount>();
                    const std::int32_t n = cnt && cnt->count > 0 ? static_cast<std::int32_t>(cnt->count) : 1;
                    auto* own = xl->GetByType<RE::ExtraOwnership>();
                    const char* bar = TakeBarred(a_f, a_src, a_obj, xl, a_road);
                    p.copies.push_back({ xl, n, bar, own ? own->owner : p.contOwner });
                    xlTotal += n;
                    if (bar) p.anyBarred = true;
                }
            p.clean = std::max<std::int32_t>(0, it->second.first - xlTotal);
            if (p.clean > 0) {
                p.cleanBar = TakeBarred(a_f, a_src, a_obj, nullptr, a_road);
                if (p.cleanBar) p.anyBarred = true;
            }
            return p;
        }
    }

    bool Takeable(RE::Actor* a_follower, RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj) {
        if (!a_follower || !a_src || !a_obj) return false;
        const Plan p = Analyse(a_follower, a_src, a_obj, Road::Container);
        if (!p.found) return false;
        if (!p.anyBarred) return true;
        for (const auto& c : p.copies) if (!c.bar) return true;   // an extra-listed copy we can name
        return false;   // only plain copies are left and either barred, or (mixed) not selectable
    }

    std::int32_t Take(RE::Actor* a_follower, RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj, std::int32_t a_count, Road a_road) {
        if (!a_follower || !a_src || !a_obj || a_count <= 0) return 0;
        if (a_road == Road::Container && a_src->As<RE::Actor>()) a_road = Road::Corpse;
        if (GuardInterim(a_follower, a_src, a_road)) return 0;
        constexpr auto kReason = RE::ITEM_REMOVE_REASON::kStoreInContainer;
        std::int32_t moved = 0;
        for (int iter = 0; iter < 16 && moved < a_count; ++iter) {   // re-read the inventory each time: a RemoveItem invalidates its extra lists
            const Plan p = Analyse(a_follower, a_src, a_obj, a_road);
            if (!p.found) break;
            if (!p.anyBarred && iter == 0) {   // the unchanged road: one plain RemoveItem, exactly as every caller always made it
                a_src->RemoveItem(a_obj, a_count, kReason, nullptr, a_follower);
                Say(a_follower, a_road, a_src, a_obj, a_count, p.contOwner, p.contOwner ? "container" : "none", nullptr);
                NoteAction(a_follower->GetFormID(), a_road, a_src->GetFormID(), "take");
                return a_count;
            }
            const Cp* pick = nullptr;
            for (const auto& c : p.copies) if (!c.bar) { pick = &c; break; }
            if (!pick) {
                const char* why = p.copies.empty() ? (p.cleanBar ? p.cleanBar : "no copy")
                                  : "only plain copies are left and a barred copy shares the stack (RemoveItem cannot choose the copy)";
                if (moved == 0) {
                    Say(a_follower, a_road, a_src, a_obj, a_count, p.contOwner, p.cleanBar ? "container" : "copy", why);
                    NoteAction(a_follower->GetFormID(), a_road, a_src->GetFormID(), "barred: stolen");
                }
                break;
            }
            const std::int32_t n = std::min(pick->n, a_count - moved);
            a_src->RemoveItem(a_obj, n, kReason, pick->xl, a_follower);
            moved += n;
            Say(a_follower, a_road, a_src, a_obj, n, pick->owner, "xOwn", nullptr);
            NoteAction(a_follower->GetFormID(), a_road, a_src->GetFormID(), "take (clean copy)");
        }
        return moved;
    }

    const char* LooseBarred(RE::Actor* a_follower, RE::TESObjectREFR* a_ref, Road a_road, bool a_arrival, bool* a_pending) {
        if (!a_follower || !a_ref) return "no ref";
        auto* base = a_ref->GetBaseObject();
        if (const char* why = LooseRefBarred(a_ref)) {   // any owner, off-limits, quest, civilised-owned cell, ... (stricter than the engine's pickup test)
            if (a_arrival) NoteAction(a_follower->GetFormID(), a_road, a_ref->GetFormID(), "barred: ownership");   // the scan pass is not a world action
            // the caller logs its own SKIPPED line once per ref; this is the proof line, rate-limited
            Say(a_follower, a_road, a_ref, base, 1, a_ref->GetOwner(), a_ref->GetOwner() ? "xOwn" : "cell", why);
            return why;
        }
        if (!a_arrival) return nullptr;   // the scan: the guard gate ran once for the follower at the top of LootNearby, and a take line per candidate would flood
        switch (GuardCheck(a_follower, a_ref, a_road, false)) {
            case GuardState::Pending: if (a_pending) *a_pending = true; return "the guard check is not back yet (retry next tick)";
            case GuardState::Seen:    return "a guard can see the follower or the item (interim ruling)";
            case GuardState::Clear:   break;
        }
        Say(a_follower, a_road, a_ref, base, 1, nullptr, "none", nullptr);
        NoteAction(a_follower->GetFormID(), a_road, a_ref->GetFormID(), "take");
        return nullptr;
    }
}
