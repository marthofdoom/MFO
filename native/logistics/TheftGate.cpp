// logistics/TheftGate.cpp -- the universal theft gate. See TheftGate.h for the rules and the RE note.
#include "Logistics_internal.h"
#include "TheftGate.h"
#include "Sightline.h"        // the guard's line of sight (MeasureNow / PointSeenNow, main thread)
#include "MainThread.h"
#include "LocationTypes.h"   // CorpseGuardWaived: TownAnchor, the same town test the errands and the loot town bars use
#include <algorithm>
#include <array>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

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
                 RE::TESForm* a_owner, const char* a_via, const char* a_barred, bool a_pending = false, RE::Actor* a_guard = nullptr) {
            const RE::FormID fid = a_f ? a_f->GetFormID() : 0u;
            const RE::FormID sid = a_src ? a_src->GetFormID() : 0u;
            const std::string verdict = a_pending ? std::string("pending") : a_barred ? std::string("BARRED:") + a_barred : std::string("take");
            // via is in the key: the worker's "queued to main" and main's "landed on main" lines for one take are both kept (review F5)
            const std::string key = std::format("{:08X}|{:08X}|{}|{}", fid, sid, verdict, a_via ? a_via : "-");
            {
                std::scoped_lock lk(g_logMx);
                const auto now = SClock::now();
                if (g_logAt.size() > 512) g_logAt.clear();
                auto& at = g_logAt[key];
                if (at.time_since_epoch().count() != 0 && now - at < kLogEvery) return;
                at = now;
            }
            auto* sb = a_src ? a_src->GetBaseObject() : nullptr;
            spdlog::info("[theft-gate] F={:08X} road={} src={:08X}({:08X}) item={:08X}x{} owner={:08X}{} via={} offLimits(pc)={} locked={} verdict={}",
                         fid, RoadName(a_road), sid, sb ? sb->GetFormID() : 0u, a_obj ? a_obj->GetFormID() : 0u, a_n,
                         a_owner ? a_owner->GetFormID() : 0u,
                         a_guard ? std::format(" guard={:08X}({:08X})", a_guard->GetFormID(), a_guard->GetActorBase() ? a_guard->GetActorBase()->GetFormID() : 0u) : std::string(),
                         a_via ? a_via : "-", a_src && a_src->IsOffLimits() ? 1 : 0, a_src && a_src->IsLocked() ? 1 : 0, verdict);
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
            // PEEK (the approach prefetch) stamps nothing and reposts on the TTL alone: the first arrival gate starts the visit, and the
            // answer it uses may come from an approach scan, bounded by kGuardFresh (plus the repost interval).
            const bool freshAns = e.have && (now - e.at < kGuardFresh || (e.unread && now - e.at < kGuardUnreadMax));
            const bool sameVisit = e.have && e.readAt.time_since_epoch().count() != 0 && now - e.readAt < kGuardFresh;
            if (freshAns || sameVisit) {
                have = true; seen = e.seen; how = e.how; guard = e.guard;
                if (!a_peek) {
                    e.unread = false;
                    if (!sameVisit) e.readAt = now;
                }
            }
            // A PEEK reposts on the TTL alone (never on unread / sameVisit): the scans it posts are never read by it, so their unread flag must
            // not keep an approach-position answer alive past kGuardFresh (review 2 SEV-2). The newest scan stays under ~kGuardFresh + repost.
            const bool haveForPost = a_peek ? (e.have && now - e.at < kGuardFresh) : have;
            if (!haveForPost && (e.posted.time_since_epoch().count() == 0 || now - e.posted >= kGuardRepost)) { e.posted = now; post = true; }
        }
        if (post) {
            if (MainThread::IsInstalled()) MainThread::Post([fid, sid]() { ScanGuards(fid, sid); });
            else                           ScanGuards(fid, sid);   // VR: no pump, the other main-thread reads fall back the same way
        }
        if (a_peek) return GuardState::Pending;   // prefetch: the scan may be posted, nothing else (no stamp, no log, no refusal record)
        if (!have) {   // no fresh answer yet: the caller must retry next tick, never treat this as a verdict (no blocklist, no travel-fail)
            ++t_lastRefusal.n; t_lastRefusal.state = GuardState::Pending; t_lastRefusal.guard = 0; t_lastRefusal.landing = false; t_lastRefusal.notYet = false;
            Say(a_follower, Road::Guard, a_src, nullptr, 0, nullptr, how, "guard", /*a_pending*/ true);
            return GuardState::Pending;
        }
        if (!seen) return GuardState::Clear;
        ++t_lastRefusal.n; t_lastRefusal.state = GuardState::Seen; t_lastRefusal.guard = guard; t_lastRefusal.landing = false; t_lastRefusal.notYet = false;
        auto* g = guard ? RE::TESForm::LookupByID<RE::Actor>(guard) : nullptr;
        Say(a_follower, a_road == Road::Loose || a_road == Road::Flora ? a_road : Road::Guard, a_src, nullptr, 0,
            nullptr, how, "guard", /*a_pending*/ false, g);   // owner= stays the SOURCE's owner (none here); the guard prints as guard=<ref>(<base>)
        return GuardState::Seen;   // a guard bar is a verdict, not a world action: it never enters the ring
    }

    GuardRefusal GuardRefusalInfo() { return t_lastRefusal; }

    bool GuardInterim(RE::Actor* a_follower, RE::TESObjectREFR* a_src, Road a_road) {
        return GuardCheck(a_follower, a_src, a_road, false) != GuardState::Clear;
    }

    bool PlayerInTown() {
        auto* pc = RE::PlayerCharacter::GetSingleton();
        return !pc || LocationTypes::TownAnchor(pc->GetCurrentLocation());   // no player: fail closed (the bar stays)
    }

    bool CorpseGuardWaived(RE::Actor* a_follower, RE::TESObjectREFR* a_corpse) {
        if (!a_follower || !a_corpse || !a_corpse->As<RE::Actor>()) return false;
        auto* pc = RE::PlayerCharacter::GetSingleton();
        // In a town (the player's location OR the corpse's): the corpse guard check stays. A null location (wilderness) is no town: it waives.
        if (!pc || LocationTypes::TownAnchor(pc->GetCurrentLocation()) || LocationTypes::TownAnchor(a_corpse->GetCurrentLocation())) return false;
        if (a_corpse->GetOwner()) return false;
        const auto inv = InvSnap::Get(a_corpse);   // INVARIANTS #84: the main-built snapshot, never the live list
        if (!inv) return false;                    // not yet: NOT waived (every gate stays), asked again next tick
        for (const auto& [obj, data] : inv->items)
            for (const auto& c : data.second->copies)
                if (c.owner) return false;   // one owned copy keeps every gate
        const std::string key = std::format("waive|{:08X}|{:08X}", a_follower->GetFormID(), a_corpse->GetFormID());
        {
            std::scoped_lock lk(g_logMx);
            const auto now = SClock::now();
            if (g_logAt.size() > 512) g_logAt.clear();
            auto& at = g_logAt[key];
            if (at.time_since_epoch().count() == 0 || now - at >= kLogEvery) {
                at = now;
                spdlog::info("[theft-gate] F={:08X} road=corpse src={:08X} guard check WAIVED: outside a town and the corpse carries no ownership",
                             a_follower->GetFormID(), a_corpse->GetFormID());
            }
        }
        return true;
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

    // One copy's verdict from its VALUES (the snapshot road): the same two tests TakeBarred makes on a live list.
    const char* TakeBarredOwner(RE::Actor* a_follower, RE::TESObjectREFR* a_src, RE::TESForm* a_copyOwner) {
        if (!a_follower || !a_src) return "no follower or source";
        if (a_copyOwner)
            return OtherOwner(a_copyOwner, a_follower) ? "this copy is owned by someone else (stolen goods)" : nullptr;
        if (auto* co = a_src->GetOwner(); co && OtherOwner(co, a_follower)) return "the container's owner is someone else";
        return nullptr;
    }

    namespace {

        // xl is set by AnalyseLive ONLY (a live ExtraDataList, valid for the write that follows on the same thread). The snapshot
        // road leaves it null: an extra-list pointer does not survive a tick. uid = that copy's ExtraUniqueID (0 = none).
        struct Cp { RE::ExtraDataList* xl; std::int32_t n; const char* bar; RE::TESForm* owner; bool worn; std::uint16_t uid; };
        struct Plan {
            std::vector<Cp> copies;
            std::int32_t total = 0;            // every copy of the object the source holds
            std::int32_t clean = 0;            // copies with no extra list
            const char*  cleanBar = nullptr;   // why the plain copies are barred
            bool anyBarred = false;            // any copy (extra-listed or plain) barred
            bool anyWorn = false;              // a copy the source WEARS (ExtraWorn / ExtraWornLeft): taking it unequips it (3D side effects)
            RE::TESForm* contOwner = nullptr;
            bool found = false;
            bool notYet = false;               // Analyse only: the source's snapshot is not readable yet (found stays false)
        };
        // THE DECISION READ (INVARIANTS #84): the plan from the source's main-built snapshot. Every worker decision (Takeable, Take's
        // plan for a queued corpse transfer) reads this one. On the main thread InvSnap::Get reads live, so the answer is the same
        // as AnalyseLive's there. Cp::xl is null.
        Plan Analyse(RE::Actor* a_f, RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj, Road a_road) {
            (void)a_road;
            Plan p;
            const auto inv = InvSnap::Get(a_src);
            if (!inv) { p.notYet = true; return p; }
            const auto it = inv->items.find(a_obj);
            if (it == inv->items.end() || it->second.first <= 0) return p;
            p.found = true;
            p.total = it->second.first;
            p.contOwner = a_src->GetOwner();
            std::int32_t xlTotal = 0;
            for (const auto& c : it->second.second->copies) {
                const char* bar = TakeBarredOwner(a_f, a_src, c.owner);
                const bool worn = c.worn || c.wornLeft;
                p.copies.push_back({ nullptr, c.n, bar, c.hasOwnership ? c.owner : p.contOwner, worn, c.hasUid ? c.uid : std::uint16_t{ 0 } });
                xlTotal += c.n;
                if (bar) p.anyBarred = true;
                if (worn) p.anyWorn = true;
            }
            p.clean = std::max<std::int32_t>(0, it->second.first - xlTotal);
            if (p.clean > 0) {
                p.cleanBar = TakeBarredOwner(a_f, a_src, nullptr);
                if (p.cleanBar) p.anyBarred = true;
            }
            return p;
        }

        // THE WRITE'S OWN READ: the live list, with the live ExtraDataList of each copy, for the RemoveItem that follows on the SAME
        // thread. Callers: MoveNow and RunTransfer only. RunTransfer is the main thread. MoveNow on the worker is the CONTAINER road's
        // transfer, the one worker-side inventory write #83 left on the worker (a non-actor source): its read stays with it and moves
        // to the main thread with it (stability audit M2). Nothing else may call this.
        Plan AnalyseLive(RE::Actor* a_f, RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj, Road a_road) {
            Plan p;
            MainThread::NoteWriteRead(__FUNCTION__);   // INVARIANTS #84: the live read of a write made right here (self-counted when off the main thread)
            auto inv = a_src->GetInventory([&](RE::TESBoundObject& x) { return &x == a_obj; });
            auto it = inv.find(a_obj);
            if (it == inv.end() || it->second.first <= 0) return p;
            p.found = true;
            p.total = it->second.first;
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
                    const bool worn = xl->HasType<RE::ExtraWorn>() || xl->HasType<RE::ExtraWornLeft>();
                    auto* u = xl->GetByType<RE::ExtraUniqueID>();
                    p.copies.push_back({ xl, n, bar, own ? own->owner : p.contOwner, worn, u ? u->uniqueID : std::uint16_t{ 0 } });
                    xlTotal += n;
                    if (bar) p.anyBarred = true;
                    if (worn) p.anyWorn = true;
                }
            p.clean = std::max<std::int32_t>(0, it->second.first - xlTotal);
            if (p.clean > 0) {
                p.cleanBar = TakeBarred(a_f, a_src, a_obj, nullptr, a_road);
                if (p.cleanBar) p.anyBarred = true;
            }
            return p;
        }

        // How many copies the plan lets him take: the whole stack when nothing is barred (the one plain RemoveItem), else the
        // extra-listed copies that are not barred (a plain copy cannot be chosen next to a barred one).
        std::int32_t Offer(const Plan& a_p) {
            if (!a_p.anyBarred) return a_p.total;
            std::int32_t n = 0;
            for (const auto& c : a_p.copies) if (!c.bar) n += c.n;
            return n;
        }
        const char* BarWhy(const Plan& a_p) {
            return a_p.copies.empty() ? (a_p.cleanBar ? a_p.cleanBar : "no copy")
                   : "only plain copies are left and a barred copy shares the stack (RemoveItem cannot choose the copy)";
        }

        // THE TRANSFER, on the calling thread. The loop every take always ran. a_landing = this is the main-thread run of a take the
        // worker already announced (its `queued` proof line and ring entry stand): the success ring entry is not written twice.
        std::int32_t MoveNow(RE::Actor* a_follower, RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj, std::int32_t a_count, Road a_road,
                             bool a_landing) {
            constexpr auto kReason = RE::ITEM_REMOVE_REASON::kStoreInContainer;
            std::int32_t moved = 0;
            for (int iter = 0; iter < 16 && moved < a_count; ++iter) {   // re-read the inventory each time: a RemoveItem invalidates its extra lists
                const Plan p = AnalyseLive(a_follower, a_src, a_obj, a_road);
                if (!p.found) break;
                if (!p.anyBarred && iter == 0) {   // the unchanged road: one plain RemoveItem, exactly as every caller always made it
                    a_src->RemoveItem(a_obj, a_count, kReason, nullptr, a_follower);
                    if (!a_landing) { InvSnap::Touch(a_src); InvSnap::Touch(a_follower); }   // a worker-side write: both snapshots are behind it
                    Say(a_follower, a_road, a_src, a_obj, a_count, p.contOwner, a_landing ? "landed on main" : p.contOwner ? "container" : "none", nullptr);
                    if (!a_landing) NoteAction(a_follower->GetFormID(), a_road, a_src->GetFormID(), "take");
                    return a_count;
                }
                const Cp* pick = nullptr;
                for (const auto& c : p.copies) if (!c.bar) { pick = &c; break; }
                if (!pick) {
                    if (moved == 0) {
                        Say(a_follower, a_road, a_src, a_obj, a_count, p.contOwner, p.cleanBar ? "container" : "copy", BarWhy(p));
                        NoteAction(a_follower->GetFormID(), a_road, a_src->GetFormID(), "barred: stolen");
                    }
                    break;
                }
                const std::int32_t n = std::min(pick->n, a_count - moved);
                a_src->RemoveItem(a_obj, n, kReason, pick->xl, a_follower);
                if (!a_landing) { InvSnap::Touch(a_src); InvSnap::Touch(a_follower); }
                moved += n;
                Say(a_follower, a_road, a_src, a_obj, n, pick->owner, a_landing ? "xOwn, landed on main" : "xOwn", nullptr);
                if (!a_landing) NoteAction(a_follower->GetFormID(), a_road, a_src->GetFormID(), "take (clean copy)");
            }
            return moved;
        }

        // ── CORPSE TRANSFERS RUN ON THE MAIN THREAD (INVARIANTS #83, ENGINE_NOTES 0.30 amendment 2026-11-15) ──
        // RemoveItem on an ACTOR is not an inventory-only call. InventoryChanges::RemoveItem (1.6.1170 0x228180) calls
        // ActorEquipManager::UnequipObject (0x6CA010, at 0x22A967) for a copy the actor WEARS, on the calling thread: an AMMO or a
        // torch is detached from the biped right there (0x2E9AA0 / 0x2E9670), an ARMO sets the actor's 3D update flag and calls
        // Actor::WornArmorChanged, and the equip event fires to every sink. And a corpse's inventory is walked by main-thread code
        // (its own 3D rebuild, the container menu, other plugins) with no lock a worker write would respect. So every transfer whose
        // SOURCE is an actor is queued here by the take road (the worker) and run by ONE drain per frame on the main thread:
        //   * FIFO, FormIDs only; both refs and the copies are re-resolved on the frame that runs (extra-list pointers do not survive
        //     the hop), by the same Analyse, so the per-copy ownership bar is re-read there. The guard verdict was the worker's.
        //   * no per-frame cap: the whole queue runs in one drain, worn pieces included (vanilla Take All does the same in one frame).
        //     Only an open inventory menu (InventoryMenuOpen) stops the drain, which reposts itself (next frame), queue order intact.
        //   * a follow-up that needs the item in the pack (the equip) is attached to its take (AfterLanding) and runs right after it.
        //   * the worker never waits: until a take lands, QueuedOut hides its copies from every worker decision about that source,
        //     InFlightWeight carries its weight, and LandingHold makes Takeable hold same-type decisions for that follower. That is
        //     LOGIC honesty only: the worker's own reads of the corpse / follower inventories (Analyse / GetInventory here, DeclareAtExit,
        //     IAUS ReadLootInv) still run while main writes them (the old race, the other direction). Docs/REVIEW-BACKLOG.md (F2).
        struct Job {
            std::uint64_t seq = 0;
            RE::FormID    fid = 0, sid = 0, obj = 0;
            std::int32_t  n = 0;
            Road          road = Road::Corpse;
            std::uint16_t uid = 0;                          // != 0: the ONE copy carrying this ExtraUniqueID (QueueCopy)
            RE::FormType  type = RE::FormType::None;        // the item's form type (LandingHold)
            float         weight = 0.0f;                    // n x the item's weight (InFlightWeight)
            SClock::time_point at{};
        };
        struct Xfer { Job j; bool lateSaid = false; std::vector<std::function<void(std::int32_t)>> after; };
        constexpr auto kLandLate = std::chrono::seconds(5);   // a take not landed by then is SAID (the pump drains every frame; a pause stalls it)
        std::mutex       g_xMx;
        std::deque<Xfer> g_xq;                 // g_xMx. The front entry stays queued while the drain runs it
        bool             g_xPosted = false;    // g_xMx. One drain is posted (or reposted) and has not found the queue empty yet
        std::uint64_t    g_xSeq = 0;           // g_xMx
        std::atomic<std::thread::id> g_mainTid{};   // the thread the drain last ran on (the main thread); default id until the first drain

        bool OnMainThread() { return g_mainTid.load(std::memory_order_relaxed) == std::this_thread::get_id(); }

        // Copies of (source, item) already queued out and not landed. The entry the drain is running still counts.
        std::int32_t QueuedOut(RE::FormID a_sid, RE::FormID a_obj) {
            std::scoped_lock lk(g_xMx);
            std::int32_t n = 0;
            for (const auto& x : g_xq) if (x.j.sid == a_sid && x.j.obj == a_obj) n += x.j.n;
            return n;
        }

        // TRUE = an item of a_type is on its way to a_fid. A take older than kLandLate is said once (principle 7: never a silent stall).
        bool LandingHold(RE::FormID a_fid, RE::FormType a_type) {
            const auto now = SClock::now();
            std::scoped_lock lk(g_xMx);
            bool held = false;
            for (auto& x : g_xq) {
                if (x.j.fid != a_fid) continue;
                if (!x.lateSaid && now - x.j.at > kLandLate) {
                    x.lateSaid = true;
                    spdlog::warn("[take] F={:08X} src={:08X} item={:08X}x{}: queued to the main thread {:.1f}s ago and NOT landed yet (the main-thread "
                                 "pump is not draining: a pause, or a stall). His same-type takes stay held until it lands.",
                                 x.j.fid, x.j.sid, x.j.obj, x.j.n, std::chrono::duration<float>(now - x.j.at).count());
                }
                if (x.j.type == a_type) held = true;
            }
            return held;
        }

        void DrainTransfers();

        void Enqueue(RE::Actor* a_follower, RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj, std::int32_t a_n, Road a_road,
                     std::uint16_t a_uid, std::function<void(std::int32_t)> a_landed) {
            Xfer x;
            x.j.fid = a_follower->GetFormID(); x.j.sid = a_src->GetFormID(); x.j.obj = a_obj->GetFormID();
            x.j.n = a_n; x.j.road = a_road; x.j.uid = a_uid; x.j.type = a_obj->GetFormType();
            x.j.weight = a_obj->GetWeight() * static_cast<float>(a_n);
            x.j.at = SClock::now();
            if (a_landed) x.after.push_back(std::move(a_landed));
            std::scoped_lock lk(g_xMx);
            x.j.seq = ++g_xSeq;
            g_xq.push_back(std::move(x));
            if (!g_xPosted) { g_xPosted = true; MainThread::Post([] { DrainTransfers(); }); }
        }

        // MAIN THREAD. One queued take. FALSE = it must wait for the next frame (the player has an inventory menu open: the vanilla menus
        // build their lists from the containers this would mutate); nothing was touched, the entry stays queued. TRUE = it ran (or was
        // skipped, said loudly); a_moved = the copies that left the source. No per-frame cap on WORN pieces: on main each ARMO removal only
        // sets the deferred 3D-update flag (ENGINE_NOTES 0.30 amendment), and vanilla Take All removes a whole worn set in one frame.
        // a_rebuild collects the refs whose worker view the drain replaces once the whole batch has run (each distinct ref ONCE, F1).
        void AddRebuild(std::vector<RE::TESObjectREFR*>& a_rebuild, RE::TESObjectREFR* a_ref) {
            if (a_ref && std::find(a_rebuild.begin(), a_rebuild.end(), a_ref) == a_rebuild.end()) a_rebuild.push_back(a_ref);
        }

        bool RunTransfer(const Job& a_j, std::int32_t& a_moved, std::vector<RE::TESObjectREFR*>& a_rebuild) {
            a_moved = 0;
            if (InventoryMenuOpen()) return false;
            auto* fol = RE::TESForm::LookupByID<RE::Actor>(a_j.fid);
            auto* src = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_j.sid);
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(a_j.obj);
            const char* skip = nullptr;
            if (!fol || !obj)                                                  skip = "the follower or the item no longer resolves";
            else if (fol->IsDead() || fol->IsDisabled() || fol->IsMarkedForDeletion()) skip = "the follower is dead or disabled";
            else if (!src)                                                     skip = "the source no longer resolves";
            else if (src->IsDisabled() || src->IsMarkedForDeletion())          skip = "the source is disabled or deleted";
            else if (auto* cell = src->GetParentCell(); !cell || !cell->IsAttached()) skip = "the source is unloaded (its cell is not attached)";
            if (skip) {
                spdlog::warn("[take] F={:08X} src={:08X} item={:08X}x{}: queued take NOT run on the main thread: {} (nothing moved)",
                             a_j.fid, a_j.sid, a_j.obj, a_j.n, skip);
                return true;
            }
            const Plan p = AnalyseLive(fol, src, obj, a_j.road);
            const Cp* named = nullptr;
            if (p.found && a_j.uid != 0)
                for (const auto& c : p.copies)
                    if (c.uid == a_j.uid) { named = &c; break; }
            if (!p.found || (a_j.uid != 0 && (!named || named->bar))) {
                spdlog::warn("[take] F={:08X} src={:08X} item={:08X}x{}: queued take NOT run on the main thread: {} (nothing moved)",
                             a_j.fid, a_j.sid, a_j.obj, a_j.n,
                             !p.found ? "the source no longer holds the item" : !named ? "the named copy is no longer in the source" : named->bar);
                return true;
            }
            // A named copy is judged by itself; a counted take by the object (the plain RemoveItem lets the engine choose the copies).
            const bool worn = named ? named->worn : p.anyWorn;
            if (named) src->RemoveItem(obj, 1, RE::ITEM_REMOVE_REASON::kStoreInContainer, named->xl, fol);
            else       MoveNow(fol, src, obj, a_j.n, a_j.road, /*a_landing*/ true);
            const Plan q = AnalyseLive(fol, src, obj, a_j.road);   // q.total is 0 when nothing is left
            // INVARIANTS #84: the worker's view of both inventories is replaced in place, BEFORE the drain drops this entry from the
            // queue (QueuedOut / LandingHold / InFlightWeight), so no worker tick sees "nothing queued" next to the pre-take inventory.
            // The drain does it once per distinct ref after the batch (a corpse of N item kinds = one rebuild of each pack, not N).
            AddRebuild(a_rebuild, src);
            AddRebuild(a_rebuild, fol);
            a_moved = p.total - q.total;
            if (a_moved != a_j.n)
                spdlog::warn("[take] F={:08X} src={:08X} item={:08X}: planned x{} on the worker, x{} left the source on the main thread ({} before, {} "
                             "after){}", a_j.fid, a_j.sid, a_j.obj, a_j.n, a_moved, p.total, q.total, worn ? "; a WORN piece was in the stack" : "");
            else if (worn)
                spdlog::info("[take] F={:08X} src={:08X} item={:08X}x{}: taken on the main thread from a stack the corpse WORE (the unequip ran "
                             "on main)", a_j.fid, a_j.sid, a_j.obj, a_moved);
            return true;
        }

        // MAIN THREAD, at most once per frame (g_xPosted: one drain is posted at a time, and its own repost lands next frame).
        // A batch: every queued job runs (each entry stays queued while it runs: QueuedOut / LandingHold keep counting it), then each
        // distinct ref the batch wrote is rebuilt ONCE, then the entries that ran are popped and their follow-ups fire. Rebuild before
        // pop (INVARIANTS #84) holds for the whole batch. Jobs read live (AnalyseLive), so a job never needs the rebuild of the one before.
        void DrainTransfers() {
            g_mainTid.store(std::this_thread::get_id(), std::memory_order_relaxed);
            struct Ran { std::uint64_t seq; std::int32_t moved; RE::FormID fid, obj; };
            for (;;) {
                std::vector<Ran> ran;
                std::vector<RE::TESObjectREFR*> rebuild;
                bool blocked = false;
                for (std::size_t i = 0;; ++i) {
                    Job j;
                    {
                        std::scoped_lock lk(g_xMx);
                        if (i >= g_xq.size()) break;
                        j = g_xq[i].j;
                    }
                    std::int32_t moved = 0;
                    if (!RunTransfer(j, moved, rebuild)) { blocked = true; break; }
                    ran.push_back({ j.seq, moved, j.fid, j.obj });
                }
                for (auto* r : rebuild) InvSnap::RebuildOnMain(r);
                std::vector<std::pair<const Ran*, std::vector<std::function<void(std::int32_t)>>>> after;
                {
                    std::scoped_lock lk(g_xMx);
                    for (const auto& r : ran) {   // found by seq (a revert may have cleared the queue meanwhile)
                        const auto it = std::find_if(g_xq.begin(), g_xq.end(), [&](const Xfer& x) { return x.j.seq == r.seq; });
                        if (it == g_xq.end()) continue;
                        after.emplace_back(&r, std::move(it->after));
                        g_xq.erase(it);
                    }
                }
                for (auto& [r, fns] : after) {
                    if (r->moved > 0) { for (auto& fn : fns) if (fn) fn(r->moved); }
                    else if (!fns.empty())
                        spdlog::warn("[take] F={:08X} item={:08X}: {} follow-up(s) (the equip of the looted piece) DROPPED: the take moved nothing",
                                     r->fid, r->obj, fns.size());
                }
                if (blocked) {
                    MainThread::Post([] { DrainTransfers(); });   // g_xPosted stays true: this repost IS the posted drain
                    return;
                }
                std::scoped_lock lk(g_xMx);
                if (g_xq.empty()) { g_xPosted = false; return; }
            }
        }
    }

    void NoteInventoryNotYet(RE::Actor* a_follower, RE::TESObjectREFR* a_src) {
        ++t_lastRefusal.n; t_lastRefusal.state = GuardState::Pending; t_lastRefusal.guard = 0; t_lastRefusal.landing = true; t_lastRefusal.notYet = true;
        // The first ask about any source is "not yet" by design (the next tick has it), so one not-yet says nothing. A source that
        // STAYS not-yet for kNotYetSay is said (principle 7: never a silent stall), then once per kLogEvery while it lasts.
        constexpr auto kNotYetSay   = std::chrono::seconds(2);
        constexpr auto kNotYetReset = std::chrono::seconds(5);   // no not-yet for this long = that episode ended
        struct Run { SClock::time_point first{}, last{}, said{}; };
        static std::unordered_map<std::uint64_t, Run> s_runs;   // g_logMx
        const std::uint64_t key = (static_cast<std::uint64_t>(a_follower ? a_follower->GetFormID() : 0u) << 32) | (a_src ? a_src->GetFormID() : 0u);
        const auto now = SClock::now();
        std::scoped_lock lk(g_logMx);
        if (s_runs.size() > 1024) s_runs.clear();
        auto& r = s_runs[key];
        if (r.first.time_since_epoch().count() == 0 || now - r.last > kNotYetReset) { r.first = now; r.said = {}; }
        r.last = now;
        if (now - r.first >= kNotYetSay && (r.said.time_since_epoch().count() == 0 || now - r.said >= kLogEvery)) {
            r.said = now;
            spdlog::warn("[theft-gate] F={:08X} src={:08X} held for {:.1f}s: the source's inventory snapshot is still not readable (it is built "
                         "on the main thread, INVARIANTS #84). The main-thread pump is not draining, or the source no longer resolves. "
                         "Judged again next tick; nothing is taken and the source is not marked done.",
                         a_follower ? a_follower->GetFormID() : 0u, a_src ? a_src->GetFormID() : 0u,
                         std::chrono::duration<float>(now - r.first).count());
        }
    }

    bool Takeable(RE::Actor* a_follower, RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj) {
        if (!a_follower || !a_src || !a_obj) return false;
        if (LandingHold(a_follower->GetFormID(), a_obj->GetFormType())) {
            ++t_lastRefusal.n; t_lastRefusal.state = GuardState::Pending; t_lastRefusal.guard = 0; t_lastRefusal.landing = true; t_lastRefusal.notYet = false;
            const std::string key = std::format("land|{:08X}|{:02X}", a_follower->GetFormID(), static_cast<std::uint32_t>(a_obj->GetFormType()));
            std::scoped_lock lk(g_logMx);
            const auto now = SClock::now();
            if (g_logAt.size() > 512) g_logAt.clear();
            auto& at = g_logAt[key];
            if (at.time_since_epoch().count() == 0 || now - at >= kLogEvery) {
                at = now;
                spdlog::info("[theft-gate] F={:08X} src={:08X} item={:08X} held: an item of this form type is still on its way to him (a corpse take "
                             "queued to the main thread); judged again next tick", a_follower->GetFormID(), a_src->GetFormID(), a_obj->GetFormID());
            }
            return false;
        }
        const Plan p = Analyse(a_follower, a_src, a_obj, Road::Container);
        if (p.notYet) { NoteInventoryNotYet(a_follower, a_src); return false; }
        if (!p.found) return false;
        // Copies already queued out of an actor source are not offered twice (to him or to another follower).
        const std::int32_t queued = a_src->As<RE::Actor>() ? QueuedOut(a_src->GetFormID(), a_obj->GetFormID()) : 0;
        return Offer(p) - queued > 0;   // nothing barred: the stack; else an extra-listed copy we can name (plain copies next to a barred one are not selectable)
    }

    std::int32_t Take(RE::Actor* a_follower, RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj, std::int32_t a_count, Road a_road) {
        if (!a_follower || !a_src || !a_obj || a_count <= 0) return 0;
        const bool actorSrc = a_src->As<RE::Actor>() != nullptr;
        if (a_road == Road::Container && actorSrc) a_road = Road::Corpse;
        if (!(a_road == Road::Corpse && CorpseGuardWaived(a_follower, a_src)) && GuardInterim(a_follower, a_src, a_road)) return 0;
        if (!actorSrc || OnMainThread()) return MoveNow(a_follower, a_src, a_obj, a_count, a_road, /*a_landing*/ false);
        if (!MainThread::IsInstalled()) {   // no live pump (unsupported runtime): the documented direct road, as the other main-thread hops fall back
            static std::atomic<bool> s_said{ false };
            if (!s_said.exchange(true))
                spdlog::warn("[take] the main-thread pump is not installed on this runtime: corpse transfers run on the calling thread (the "
                             "pre-2026-11-15 road). Said once.");
            return MoveNow(a_follower, a_src, a_obj, a_count, a_road, /*a_landing*/ false);
        }
        // AN ACTOR SOURCE, ON THE WORKER: plan here, transfer on main.
        const Plan p = Analyse(a_follower, a_src, a_obj, a_road);
        if (p.notYet) { NoteInventoryNotYet(a_follower, a_src); return 0; }
        if (!p.found) return 0;
        const std::int32_t offer   = Offer(p);
        const std::int32_t planned = std::min(a_count, offer - QueuedOut(a_src->GetFormID(), a_obj->GetFormID()));
        if (planned <= 0) {
            if (offer <= 0) {   // every copy is barred (the verdict the loop always gave); else the rest is queued already: nothing to say
                Say(a_follower, a_road, a_src, a_obj, a_count, p.contOwner, p.cleanBar ? "container" : "copy", BarWhy(p));
                NoteAction(a_follower->GetFormID(), a_road, a_src->GetFormID(), "barred: stolen");
            }
            return 0;
        }
        Enqueue(a_follower, a_src, a_obj, planned, a_road, 0, {});
        Say(a_follower, a_road, a_src, a_obj, planned, p.contOwner, "queued to main", nullptr);
        NoteAction(a_follower->GetFormID(), a_road, a_src->GetFormID(), "take (queued to the main thread)");
        return planned;
    }

    std::int32_t QueueCopy(RE::Actor* a_follower, RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj, std::uint16_t a_uid, Road a_road,
                           std::function<void(std::int32_t)> a_landed) {
        if (!a_follower || !a_src || !a_obj || a_uid == 0) return 0;
        if (!a_src->As<RE::Actor>() || !MainThread::IsInstalled() || OnMainThread()) return 0;
        Enqueue(a_follower, a_src, a_obj, 1, a_road, a_uid, std::move(a_landed));
        Say(a_follower, a_road, a_src, a_obj, 1, a_src->GetOwner(), "queued to main (named copy)", nullptr);
        NoteAction(a_follower->GetFormID(), a_road, a_src->GetFormID(), "take (queued to the main thread)");
        return 1;
    }

    bool AfterLanding(RE::FormID a_follower, RE::FormID a_obj, std::function<void()> a_onMain) {
        if (!a_onMain) return false;
        std::scoped_lock lk(g_xMx);
        for (auto it = g_xq.rbegin(); it != g_xq.rend(); ++it) {   // the newest take of that item for him
            if (it->j.fid != a_follower || it->j.obj != a_obj) continue;
            it->after.push_back([fn = std::move(a_onMain)](std::int32_t) { fn(); });
            return true;
        }
        return false;
    }

    float InFlightWeight(RE::FormID a_follower) {
        std::scoped_lock lk(g_xMx);
        float w = 0.0f;
        for (const auto& x : g_xq) if (x.j.fid == a_follower) w += x.j.weight;
        return w;
    }

    void ClearTransfers() {
        std::scoped_lock lk(g_xMx);
        g_xq.clear();
        g_xPosted = false;   // MainThread::Clear dropped the posted drain with the rest of the queue
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
