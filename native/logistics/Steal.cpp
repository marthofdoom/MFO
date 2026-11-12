// logistics/Steal.cpp -- the witness pre-check shared by act.steal (G13) and the owned-lock path of act.pick_locks (G14).
// See Steal.h for the rules. A new mechanism, so its own file in the logistics subsystem.
#include "Logistics_internal.h"
#include "Steal.h"
#include "APMF_API.h"             // AwareSense / AwareDetail bits (SenseActor)
#include "apmf/APMFBridge.h"      // SenseOf
#include "Sightline.h"            // MeasureNow: the owner's line of sight
#include "MainThread.h"
#include "Lotd.h"                // IsDisplayRef
#include <algorithm>
#include <mutex>

namespace MFO::Logistics::Steal {

    namespace {
        using SClock = std::chrono::steady_clock;

        constexpr auto kFresh      = std::chrono::milliseconds(1500);   // a cached verdict is trusted this long (the take re-checks on main anyway)
        constexpr auto kUnreadMax  = std::chrono::seconds(10);          // an answer nobody has read yet is usable once up to this age
        constexpr auto kRepost     = std::chrono::milliseconds(400);    // and re-asked no faster than this per (follower, ref)
        constexpr auto kLogEvery   = std::chrono::seconds(30);
        constexpr float kMaxSight  = 8192.0f;                           // APMF_API::kAwareMaxSightRange: Harbinger clamps above this
        constexpr int   kMaxWitnessScan = 96;                           // actors examined per evaluation (the high-process list is bounded far below)

        struct Ans {
            SClock::time_point at{}, posted{};
            Verdict            v = Verdict::Pending;
            std::string        why;
            bool               have = false, unread = false;
        };
        std::mutex g_mx;
        std::unordered_map<std::uint64_t, Ans> g_cache;   // (follower << 32 | ref)
        std::mutex g_logMx;
        std::unordered_map<std::string, SClock::time_point> g_logAt;

        std::uint64_t Key(RE::FormID a_f, RE::FormID a_r) { return (static_cast<std::uint64_t>(a_f) << 32) | a_r; }

        const char* NameOf(const RE::TESForm* a_f) {
            const char* n = a_f ? a_f->GetName() : nullptr;
            return n && *n ? n : "?";
        }

        // The crime alarm range, by NAME at run time (a list may change it). Unreadable = 0 = the caller refuses.
        float AlarmRange() {
            auto* gsc = RE::GameSettingCollection::GetSingleton();
            auto* s   = gsc ? gsc->GetSetting("iCrimeAlarmRecDistance") : nullptr;
            if (!s) return 0.0f;
            const float v = static_cast<float>(s->GetSInt());
            return v > 0.0f ? v : 0.0f;
        }

        void Say(RE::Actor* a_f, RE::TESObjectREFR* a_ref, RE::TESForm* a_owner, int a_witnesses, Verdict a_v, const std::string& a_why) {
            if (a_v == Verdict::Pending) return;   // not a verdict
            const std::string verdict = a_v == Verdict::Take ? std::string("take") : "REFUSED:" + a_why;
            const RE::FormID fid = a_f ? a_f->GetFormID() : 0u, rid = a_ref ? a_ref->GetFormID() : 0u;
            const std::string key = std::format("{:08X}|{:08X}|{}", fid, rid, verdict);
            {
                std::scoped_lock lk(g_logMx);
                const auto now = SClock::now();
                if (g_logAt.size() > 512) g_logAt.clear();
                auto& at = g_logAt[key];
                if (at.time_since_epoch().count() != 0 && now - at < kLogEvery) return;
                at = now;
            }
            auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
            spdlog::info("[steal] F={:08X} item={:08X}({}) owner={:08X} witnesses={} verdict={}", fid, base ? base->GetFormID() : 0u,
                         NameOf(base), a_owner ? a_owner->GetFormID() : 0u, a_witnesses, verdict);
        }

        // Does this actor belong to the item's owner? An owner is a base NPC or a faction (ExtraOwnership / the cell owner).
        bool IsOwnerActor(RE::Actor* a_a, RE::TESForm* a_owner) {
            if (!a_a || !a_owner) return false;
            if (auto* npc = a_owner->As<RE::TESNPC>()) {
                RE::TESForm* b = a_a->GetActorBase();
                for (int i = 0; b && i < 8; ++i) {   // the owner's template chain, like TheftGate::OtherOwner
                    if (b == npc) return true;
                    auto* n = b->As<RE::TESNPC>();
                    b = n ? n->baseTemplateForm : nullptr;
                }
                return false;
            }
            if (auto* fac = a_owner->As<RE::TESFaction>()) return a_a->IsInFaction(fac);
            return false;
        }


        // One sense read, fail-closed. Out: a_seen = the viewer senses the target (or the read could not prove otherwise);
        // a_defer = a sight ray was deferred / unavailable this frame (retry). a_sightOnly: only the sight bit counts.
        void ReadSense(RE::Actor* a_viewer, RE::Actor* a_target, float a_range, bool a_sightOnly, bool& a_seen, bool& a_defer, bool& a_unreadable) {
            APMFBridge::SenseReading r{};
            // the same radius for sight AND hearing: a noise the engine's witness loop could hear at the alarm distance is heard here too
            if (!APMFBridge::SenseOf(a_viewer->GetFormID(), a_target->GetFormID(), 0, a_range, r, a_range)) { a_unreadable = true; return; }
            if (r.detail & (APMF_API::kAwareDetail_SightDeferred | APMF_API::kAwareDetail_SightUnavailable)) {
                // out-of-range sight is a definitive NO (no ray); deferred / unavailable is UNKNOWN, never "not seen"
                if (!(r.detail & APMF_API::kAwareDetail_SightOutOfRange) && !(r.senses & APMF_API::kSense_Sight)) a_defer = true;
            }
            const std::uint32_t mask = a_sightOnly ? APMF_API::kSense_Sight : r.senses;
            if (r.senses & mask) a_seen = true;
        }

        // MAIN THREAD. The whole evaluation.
        Ans Evaluate(RE::Actor* a_f, RE::TESObjectREFR* a_ref, TheftGate::Road /*a_road*/) {
            Ans ans; ans.at = SClock::now(); ans.posted = ans.at; ans.have = true; ans.unread = true;
            int witnesses = 0;
            auto* owner = a_ref ? a_ref->GetOwner() : nullptr;
            auto refuse = [&](std::string a_why) { ans.v = Verdict::Refused; ans.why = std::move(a_why); };
            auto* pc = RE::PlayerCharacter::GetSingleton();
            auto* pl = RE::ProcessLists::GetSingleton();
            if (!a_f || !a_ref || !pc || !pl) {
                refuse("unresolved actor or reference");
            } else if (a_f->IsDead() || !a_f->Is3DLoaded()) {
                refuse("the follower is not loaded");
            } else if (!a_f->IsSneaking()) {
                refuse("the follower is not sneaking");
            } else if (float range = AlarmRange(); range <= 0.0f) {
                refuse("GMST iCrimeAlarmRecDistance unreadable");
            } else {
                // F2: the witness radius is the larger of the crime alarm distance and Harbinger's default hearing radius (a list may set the
                // GMST below what an NPC hears). Past Harbinger's maximum the pre-check cannot ask, so it FAILS CLOSED (never clamps silently).
                range = std::max(range, APMF_API::kAwareDefaultHearRadius);
                if (range > kMaxSight) {
                    static std::atomic<bool> s_said{ false };
                    if (!s_said.exchange(true))
                        spdlog::warn("[steal] witness radius {:.0f} exceeds Harbinger's SenseActor maximum {:.0f}: act.steal / owned locks stay REFUSED", range, kMaxSight);
                    refuse("the witness radius exceeds what the sense service can ask (fail closed)");
                } else {
                const RE::FormID fid = a_f->GetFormID();
                // 2. the guard gate (TheftGate): the follower, and the item/lock position.
                const auto guard = TheftGate::GuardCheck(a_f, a_ref, TheftGate::Road::Guard, /*a_onMain*/ true);
                if (guard == TheftGate::GuardState::Seen) { ++witnesses; refuse("a guard sees the follower or the item"); }
                else if (guard == TheftGate::GuardState::Pending) ans.v = Verdict::Pending;
                else {
                    const auto fpos = a_f->GetPosition(), ppos = pc->GetPosition(), rpos = a_ref->GetPosition();
                    bool defer = false, unreadable = false;
                    int scanned = 0;
                    for (auto& hh : pl->highActorHandles) {
                        auto  ptr = hh.get();
                        auto* a   = ptr.get();
                        if (!a || a == a_f || a->IsPlayerRef() || a->IsPlayerTeammate()) continue;
                        if (a->IsDead() || a->IsDisabled() || !a->Is3DLoaded()) continue;
                        if (++scanned > kMaxWitnessScan) { refuse("too many actors in range to prove (fail closed)"); break; }
                        const auto  apos = a->GetPosition();
                        const bool  isOwner = IsOwnerActor(a, owner);
                        const float near = std::min({ apos.GetDistance(fpos), apos.GetDistance(ppos), apos.GetDistance(rpos) });
                        const bool  inRange = near <= range;
                        if (!inRange && !isOwner) continue;   // too far to witness; the owner is judged at any distance
                        const char* who = NameOf(a->GetActorBase());
                        bool seen = false;
                        if (inRange) {
                            // ALL senses toward the FOLLOWER, then ALL senses toward the PLAYER too (the redirected crime is judged for him, sound included)
                            ReadSense(a, a_f, range, /*sightOnly*/ false, seen, defer, unreadable);
                            if (seen) { ++witnesses; refuse(std::format("{} {:08X} senses the follower", who, a->GetFormID())); break; }
                            ReadSense(a, pc, range, /*sightOnly*/ false, seen, defer, unreadable);
                            if (seen) { ++witnesses; refuse(std::format("{} {:08X} senses the player", who, a->GetFormID())); break; }
                        }
                        if (isOwner) {   // the owner, in line of sight of either, at any distance (Sightline: the engine basis, main thread)
                            for (RE::FormID tgt : { fid, pc->GetFormID() }) {
                                const auto v = Sightline::MeasureNow(a->GetFormID(), tgt, Sightline::Basis::Engine);
                                if (v != Sightline::Verdict::Occluded) {
                                    ++witnesses;
                                    refuse(std::format("the owner {} {:08X} {} the {}", who, a->GetFormID(),
                                                       v == Sightline::Verdict::Visible ? "sees" : "may see (sight unmeasurable)",
                                                       tgt == fid ? "follower" : "player"));
                                    break;
                                }
                            }
                            if (ans.v == Verdict::Refused) break;
                        }
                    }
                    if (ans.v != Verdict::Refused) {
                        if (unreadable) refuse("the sense service is unavailable (Harbinger ABI 18 needed): not proven clear");
                        else if (defer) ans.v = Verdict::Pending;   // a sight ray was deferred this frame: ask again, never act
                        else ans.v = Verdict::Take;
                    }
                }
                }
            }
            if (ans.v == Verdict::Pending && ans.why.empty()) ans.why = "pending";
            Say(a_f, a_ref, owner, witnesses, ans.v, ans.why);
            return ans;
        }

        void Store(RE::FormID a_f, RE::FormID a_r, Ans a_ans) {
            std::scoped_lock lk(g_mx);
            if (g_cache.size() > 256) g_cache.clear();
            g_cache[Key(a_f, a_r)] = std::move(a_ans);
        }
    }

    Verdict CheckNow(RE::Actor* a_follower, RE::TESObjectREFR* a_ref, TheftGate::Road a_road, std::string* a_why) {
        Ans ans = Evaluate(a_follower, a_ref, a_road);
        if (a_follower && a_ref) Store(a_follower->GetFormID(), a_ref->GetFormID(), ans);
        if (a_why) *a_why = ans.why;
        return ans.v;
    }

    Verdict Check(RE::Actor* a_follower, RE::TESObjectREFR* a_ref, TheftGate::Road a_road, std::string* a_why) {
        if (!a_follower || !a_ref) { if (a_why) *a_why = "unresolved actor or reference"; return Verdict::Refused; }
        const RE::FormID fid = a_follower->GetFormID(), rid = a_ref->GetFormID();
        const auto now = SClock::now();
        bool post = false, have = false;
        Verdict v = Verdict::Pending;
        std::string why;
        {
            std::scoped_lock lk(g_mx);
            auto& e = g_cache[Key(fid, rid)];
            if (e.have && e.v != Verdict::Pending && (now - e.at < kFresh || (e.unread && now - e.at < kUnreadMax))) {
                have = true; v = e.v; why = e.why; e.unread = false;
            }
            if (!have && (e.posted.time_since_epoch().count() == 0 || now - e.posted >= kRepost)) { e.posted = now; post = true; }
        }
        if (post && MainThread::IsInstalled()) {
            MainThread::Post([fid, rid, a_road]() {
                auto* f = RE::TESForm::LookupByID<RE::Actor>(fid);
                auto* r = RE::TESForm::LookupByID<RE::TESObjectREFR>(rid);
                (void)CheckNow(f, r, a_road);
            });
        }
        if (post && !MainThread::IsInstalled()) {   // VR: no pump, no main-thread reads: not provable, so no steal (fail closed, said once)
            static std::atomic<bool> s_said{ false };
            if (!s_said.exchange(true))
                spdlog::warn("[steal] no main-thread pump on this runtime: the witness pre-check cannot run, act.steal / owned locks stay REFUSED");
            if (a_why) *a_why = "no main-thread pump (the witness pre-check cannot run)";
            return Verdict::Refused;
        }
        if (!have) { if (a_why) *a_why = "pending"; return Verdict::Pending; }
        if (a_why) *a_why = std::move(why);
        return v;
    }

    const char* LooseBarred(RE::Actor* a_follower, RE::TESObjectREFR* a_ref, bool a_arrival, bool* a_pending) {
        if (!a_follower || !a_ref) return "no ref";
        if (Lotd::IsDisplayRef(a_ref->GetFormID())) return "a LOTD museum display";
        if (RefInPlayerStorage(a_ref)) return "player storage / player home (museum halls included)";
        if (IsQuestObjectRef(a_ref)) return "a quest item";
        if (IsFactionServiceContainer(a_ref)) return "a faction's vendor / stolen-goods / evidence container";
        if (auto* own = a_ref->GetOwner()) {
            constexpr RE::FormID kPlayerBaseNpc = 0x00000007, kPlayerFaction = 0x00000DB1;
            if (own->GetFormID() == kPlayerBaseNpc || own->GetFormID() == kPlayerFaction) return "owned by the player's own side";
        }
        if (!a_arrival) return nullptr;
        thread_local std::string s_why;
        switch (Check(a_follower, a_ref, TheftGate::Road::Steal, &s_why)) {
            case Verdict::Take:    return nullptr;
            case Verdict::Pending: if (a_pending) *a_pending = true; return "the witness pre-check is not back yet (retry next tick)";
            case Verdict::Refused: break;
        }
        s_why = "witness pre-check: " + s_why;
        return s_why.c_str();
    }
}
