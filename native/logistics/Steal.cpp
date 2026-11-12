// logistics/Steal.cpp -- the witness pre-check shared by act.steal (G13) and the owned-lock path of act.pick_locks (G14).
// See Steal.h for the rules. A new mechanism, so its own file in the logistics subsystem.
#include "Logistics_internal.h"
#include "Steal.h"
#include "APMF_API.h"             // AwareSense / AwareDetail bits (SenseActor)
#include "apmf/APMFBridge.h"      // SenseOf
#include "Sightline.h"            // MeasureNow: the owner's line of sight
#include "MainThread.h"
#include "Lotd.h"                // IsDisplayRef
#include "Runtime.h"             // SeatVerified(): the mit-3.7 F1 self-check gate (Steal.ExtraDataList.Destroy row)
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

        void CollectChestOwners(RE::TESObjectREFR* a_ref, std::vector<RE::TESForm*>& a_out);   // below (owned chests)

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
            // F4: a CONTAINER's owners: the chest's, the cell owner of an off-limits chest with none, and the owner of any copy that differs
            std::vector<RE::TESForm*> owners;
            if (a_ref && !a_ref->As<RE::Actor>() && a_ref->GetBaseObject() && a_ref->GetBaseObject()->Is(RE::FormType::Container)) {
                CollectChestOwners(a_ref, owners);
                if (!owner && !owners.empty()) owner = owners.front();
            } else if (owner) owners.push_back(owner);
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
                        bool        isOwner = false;
                        for (auto* o : owners) if (IsOwnerActor(a, o)) { isOwner = true; break; }
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

    // ═══ OWNED CHESTS (act.pick_locks, Q-L-6): the source bars and the steal take ═══════════════════════════════════════════════

    namespace {
        struct OwnedJob {
            RE::FormID  chest = 0;
            bool        posted = true;               // true = the main-thread take has not answered yet
            OwnedTake   res = OwnedTake::Posted;
            std::string why;
        };
        std::mutex g_otMx;                            // its own lock (never g_snapMx); held only around this map
        std::unordered_map<RE::FormID, OwnedJob> g_ot;   // follower -> the one take in flight / its answer

        // The loot kinds act.steal takes from the floor (Q-L-1), judged on an INVENTORY item with the predicates the container loot
        // categories run (LootGold / Lockpicks / Valuables / SoulGems / Ingredients / Jewelry / Potions). Equipment and ammo are not here.
        bool StealKind(RE::TESBoundObject* a_obj, RE::InventoryEntryData* a_entry) {
            constexpr RE::FormID kGold001 = 0x0000000F, kLockpick = 0x0000000A;
            if (!a_obj) return false;
            if (a_entry && IsQuestObjectInstance(a_entry)) return false;   // a quest item never moves, whatever the toggle
            const RE::FormID id = a_obj->GetFormID();
            if (id == kGold001 || id == kLockpick || IsCoinLoot(a_obj)) return true;
            const bool excluded = !Config::g_lootSpecialItems.load() && Catalog::IsExcluded(id);
            if (IsValuableMisc(a_obj) || IsSoulGemItem(a_obj) || IsIngredientItem(a_obj)) return !excluded;
            if (auto* armo = a_obj->As<RE::TESObjectARMO>(); armo && IsJewelryPiece(armo)) return !excluded;
            if (auto* alc = a_obj->As<RE::AlchemyItem>(); alc && IsDrinkablePotion(alc)) return true;
            return false;
        }

        void Finish(RE::FormID a_f, RE::FormID a_chest, OwnedTake a_res, std::string a_why) {
            std::scoped_lock lk(g_otMx);
            auto it = g_ot.find(a_f);
            if (it == g_ot.end() || it->second.chest != a_chest) return;   // abandoned / replaced (a revert cleared it): drop
            it->second.posted = false; it->second.res = a_res; it->second.why = std::move(a_why);
        }

        // One owned-and-selectable copy of an item, read fresh from the live inventory (extra-list pointers are never reused across a RemoveItem).
        struct Copy { RE::ExtraDataList* xl = nullptr; std::int32_t n = 0; RE::TESForm* owner = nullptr; bool found = false; };

        RE::TESForm* CellOwner(RE::TESObjectREFR* a_chest) {
            auto* cell = a_chest ? a_chest->GetParentCell() : nullptr;
            return cell ? cell->GetOwner() : nullptr;
        }

        // The next copy of a_obj in a_chest that moves on the steal road: an extra-listed copy owned by someone else first, then the
        // plain stack (unless an UNOWNED extra-listed copy shares the entry: RemoveItem cannot choose the copy of a plain stack).
        // An off-limits chest with no explicit owner: every copy without its own ownership is the cell owner's.
        Copy NextOwned(RE::Actor* a_f, RE::TESObjectREFR* a_chest, RE::TESBoundObject* a_obj, bool a_cellOwnedOnly) {
            Copy c;
            auto inv = a_chest->GetInventory([&](RE::TESBoundObject& x) { return &x == a_obj; });
            auto it = inv.find(a_obj);
            if (it == inv.end() || it->second.first <= 0) return c;
            RE::TESForm* const chestOwner = a_cellOwnedOnly ? CellOwner(a_chest) : a_chest->GetOwner();
            std::int32_t xlTotal = 0;
            bool unownedXl = false;
            auto* e = it->second.second.get();
            if (e && e->extraLists)
                for (auto* xl : *e->extraLists) {
                    if (!xl) continue;
                    auto* cnt = xl->GetByType<RE::ExtraCount>();
                    const std::int32_t n = cnt && cnt->count > 0 ? static_cast<std::int32_t>(cnt->count) : 1;
                    auto* own = xl->GetByType<RE::ExtraOwnership>();
                    const bool owned = TheftGate::TakeBarred(a_f, a_chest, a_obj, xl, TheftGate::Road::Steal) != nullptr || (a_cellOwnedOnly && !own);
                    xlTotal += n;
                    if (!owned) { unownedXl = true; continue; }
                    if (!c.found) { c.found = true; c.xl = xl; c.n = n; c.owner = own && own->owner ? own->owner : chestOwner; }
                }
            if (c.found) return c;
            const std::int32_t plain = std::max<std::int32_t>(0, it->second.first - xlTotal);
            if (plain > 0 && !unownedXl &&
                (TheftGate::TakeBarred(a_f, a_chest, a_obj, nullptr, TheftGate::Road::Steal) != nullptr || a_cellOwnedOnly)) {
                c.found = true; c.xl = nullptr; c.n = plain; c.owner = chestOwner;
            }
            return c;
        }

        // The extra owners a witness check must also treat as "the owner": every distinct ExtraOwnership on a copy in the chest (a copy can
        // differ from the chest's owner), and the CELL owner of an off-limits chest with no explicit owner.
        void CollectChestOwners(RE::TESObjectREFR* a_ref, std::vector<RE::TESForm*>& a_out) {
            auto add = [&](RE::TESForm* o) { if (o && a_out.size() < 8 && std::find(a_out.begin(), a_out.end(), o) == a_out.end()) a_out.push_back(o); };
            if (a_ref->GetOwner()) add(a_ref->GetOwner());
            else if (a_ref->IsOffLimits()) add(CellOwner(a_ref));
            for (auto& [obj, data] : a_ref->GetInventory()) {
                auto* e = data.second.get();
                if (!obj || !e || !e->extraLists) continue;
                for (auto* xl : *e->extraLists)
                    if (xl) if (auto* own = xl->GetByType<RE::ExtraOwnership>()) add(own->owner);
            }
        }

        // ── the stamp road's engine seat: the ExtraDataList destructor, bound by id (the fork declares ~ExtraDataList, never defines it) ──
        // void(ExtraDataList*), frees nothing of the object itself. 1.6.1170 id 11572 @0x1513D0 (~ExtraDataList, id 11584 @0x151EE0, is a
        // bare jmp to it): restores the vtable, clears this thread's extra-lookup cache when it holds this list (TLS +0x18), deletes every
        // BSExtraData of the chain (+0x08) through its vtable slot 0 with edx=1, Deallocates the presence bitfield (+0x10).
        // 1.5.97 id 11438 @0x1076D0 = `mov dl,1; call 11427` (the same three steps on the 1.5.97 layout: chain +0x00, presence +0x08).
        // The engine frees a list as this destructor, then its sized free (109588 -> 36098 / 102158 -> 11233), which is
        // MemoryManager::Deallocate(ptr, false) = RE::free: 41 such pairs on 1.6.1170, 34 on 1.5.97, one inside
        // Character::AddObjectToContainer itself (37525 @0x678FEE). Row "Steal.ExtraDataList.Destroy" in native/VerifiedAddresses.h.
        using ListDtorFn = void(RE::ExtraDataList*);
        REL::Relocation<ListDtorFn>& ListDtorRel() {
            static REL::Relocation<ListDtorFn> func{ REL::RelocationID(11438, 11572) };
            return func;
        }
        // May a plain stack be stamped (a NEW ExtraDataList handed to AddObjectToContainer)? Only where a list this code made can also be
        // destroyed by it. Exactly 1.6.1170 / 1.5.97: the fork's `new ExtraDataList()` is a named FATAL error on any other build
        // (GetRuntimeSize), and id 11572 is not in the 1.7.104 id table (an id miss is fatal), so every other build is refused BEFORE
        // anything is resolved or allocated. Refused = a plain stack moves UNSTAMPED, warned per item (never a leak, never a silent launder).
        bool StampSeatOk() {
            static const bool ok = [] {
                if (!Runtime::IsVerified1_6_1170() && !Runtime::IsVerified1_5_97()) {
                    spdlog::error("[steal] runtime {} ({}): the ExtraDataList constructor / destructor pair is verified on 1.6.1170 and 1.5.97 only "
                                  "-- the stamp road is REFUSED: plain stacks stolen from owned chests move UNSTAMPED (no ownership mark)",
                                  REL::Module::get().version().string("."), Runtime::BuildLabel());
                    return false;
                }
                if (!Runtime::SeatVerified(ListDtorRel().address(), "Steal.ExtraDataList.Destroy")) {
                    spdlog::error("[steal] the Steal.ExtraDataList.Destroy self-check row did not verify -- the stamp road is REFUSED: plain stacks "
                                  "stolen from owned chests move UNSTAMPED (no ownership mark)");
                    return false;
                }
                return true;
            }();
            return ok;
        }

        // Where did the list handed to AddObjectToContainer land? InventoryChanges' add (16053 @0x226A20) LINKS the caller's list when
        // the follower has no entry for the object, or no equivalent list in it (push-front 0x226EA4 / 0x22724B). When an equivalent list
        // exists (compare 11594: same owner, ExtraCount ignored) it adds the count to THAT list (0x227071-0x227080) and neither links nor
        // frees the caller's (0x227189 `cmp rsi,[rsp+0xF0]` skips the free for a caller-made list): Merged, the caller still owns it.
        // Merged is claimed only on positive evidence (a list he held BEFORE the add now counts exactly `moved` more), never on "not
        // linked" alone: a container-changed sink of another plugin runs inside the add and may move the item on, list included.
        struct HeldList { const RE::ExtraDataList* xl; std::int32_t n; };
        enum class Landed : std::uint8_t { Adopted, Merged, Unknown };

        // Every extra list the follower holds for a_obj, with its count. Empty when he has none.
        void HeldLists(RE::Actor* a_f, const RE::TESBoundObject* a_obj, std::vector<HeldList>& a_out) {
            auto* changes = a_f->GetInventoryChanges(true);
            if (!changes || !changes->entryList) return;
            for (auto* e : *changes->entryList) {
                if (!e || e->object != a_obj || !e->extraLists) continue;
                for (auto* xl : *e->extraLists)
                    if (xl) a_out.push_back({ xl, xl->GetCount() });
            }
        }

        Landed WhereListLanded(RE::Actor* a_f, const RE::TESBoundObject* a_obj, const RE::ExtraDataList* a_xl, const std::vector<HeldList>& a_before,
                               std::int32_t a_moved) {
            auto* changes = a_f->GetInventoryChanges(true);
            if (!changes || !changes->entryList) return Landed::Unknown;
            bool merged = false;
            for (auto* e : *changes->entryList) {   // EVERY entry, whatever its object: "linked anywhere" must never read as "not adopted"
                if (!e || !e->extraLists) continue;
                for (auto* xl : *e->extraLists) {
                    if (!xl) continue;
                    if (xl == a_xl) return Landed::Adopted;
                    if (e->object != a_obj) continue;
                    for (const auto& h : a_before)
                        if (h.xl == xl && xl->GetCount() == h.n + a_moved) merged = true;
                }
            }
            return merged ? Landed::Merged : Landed::Unknown;
        }

        // Destroys a list THIS FILE allocated (new RE::ExtraDataList = RE::malloc + the engine ctor) that the engine did not adopt
        // (Landed::Merged): the engine destructor, then RE::free, exactly the engine's own pair. Call only after StampSeatOk(). MAIN
        // THREAD (the destructor clears the calling thread's extra cache; the list was only ever touched on this thread).
        void DestroyUnadoptedList(RE::ExtraDataList* a_xl) {
            ListDtorRel()(a_xl);
            RE::free(a_xl);
        }

        // How many of a_obj the chest holds right now (the before / after of every RemoveItem).
        std::int32_t CountIn(RE::TESObjectREFR* a_chest, RE::TESBoundObject* a_obj) {
            auto inv = a_chest->GetInventory([&](RE::TESBoundObject& x) { return &x == a_obj; });
            auto it = inv.find(a_obj);
            return it == inv.end() ? 0 : std::max<std::int32_t>(0, it->second.first);
        }

        // MAIN THREAD. The whole take. Re-plans from the live inventory each call, so a Retry resumes where the last call stopped.
        void OwnedTakeOnMain(RE::FormID a_fid, RE::FormID a_cid) {
            auto* f = RE::TESForm::LookupByID<RE::Actor>(a_fid);
            auto* chest = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_cid);
            if (!f || !chest || f->IsDead() || chest->IsDisabled() || chest->IsMarkedForDeletion() || !chest->Is3DLoaded()) {
                Finish(a_fid, a_cid, OwnedTake::Cut, "the follower or the chest is gone, dead or not loaded"); return;
            }
            if (chest->IsLocked()) { Finish(a_fid, a_cid, OwnedTake::Cut, "the chest reads locked again"); return; }
            if (const char* bar = OwnedChestBarred(f, chest)) { Finish(a_fid, a_cid, OwnedTake::Cut, bar); return; }
            const bool cellOwnedOnly = chest->GetOwner() == nullptr && chest->IsOffLimits();
            std::vector<RE::TESBoundObject*> items;
            for (auto& [obj, data] : chest->GetInventory())
                if (obj && data.first > 0 && StealKind(obj, data.second.get()) && NextOwned(f, chest, obj, cellOwnedOnly).found) items.push_back(obj);
            if (items.empty()) { Finish(a_fid, a_cid, OwnedTake::Nothing, {}); return; }
            // ONE proof for the whole take, on this thread, immediately before the first RemoveItem (the RemoveItems below run in this one
            // main-thread call, so nothing can change between the proof and the act)
            std::string why;
            const auto v = CheckNow(f, chest, TheftGate::Road::Steal, &why);
            if (v == Verdict::Pending) {
                spdlog::info("[steal] F={:08X} chest={:08X}: owned take PAUSED -- the pre-check is not conclusive this frame", a_fid, a_cid);
                Finish(a_fid, a_cid, OwnedTake::Retry, "pre-check not conclusive this frame"); return;
            }
            if (v == Verdict::Refused) {
                spdlog::info("[steal] F={:08X} chest={:08X}: owned take CUT before the first item -- witness pre-check: {} (nothing moved)", a_fid, a_cid, why);
                Finish(a_fid, a_cid, OwnedTake::Cut, why); return;
            }
            std::int32_t movedTotal = 0;
            for (auto* obj : items) {
                for (int iter = 0; iter < 16; ++iter) {   // the inventory is re-read before EVERY RemoveItem: it invalidates the extra lists
                    const Copy c = NextOwned(f, chest, obj, cellOwnedOnly);
                    if (!c.found) break;
                    if (obj->GetFormID() != 0x0000000F && !IsCoinLoot(obj) && !FitsCarryWeight(f, obj->GetWeight() * c.n)) break;
                    TheftGate::NoteAction(a_fid, TheftGate::Road::Steal, a_cid, "take (owned chest item)");
                    const char* how = "xOwn";
                    const std::int32_t before = CountIn(chest, obj);
                    std::int32_t moved = 0;
                    if (c.xl) {
                        // keep the copy's own owner; a copy with none gets the chest's (or cell's) owner stamped before it moves
                        auto* own = c.xl->GetByType<RE::ExtraOwnership>();
                        if ((!own || !own->owner) && c.owner) { c.xl->SetOwner(c.owner); own = c.xl->GetByType<RE::ExtraOwnership>(); }   // SetOwner refuses a dynamic form
                        if (!own || !own->owner) {
                            spdlog::warn("[steal] F={:08X} chest={:08X}: item {:08X}x{} has no stampable owner ({:08X}): moved UNSTAMPED", a_fid, a_cid, obj->GetFormID(), c.n,
                                         c.owner ? c.owner->GetFormID() : 0u);
                            how = "unstamped";
                        }
                        chest->RemoveItem(obj, c.n, RE::ITEM_REMOVE_REASON::kStoreInContainer, c.xl, f);
                        moved = before - CountIn(chest, obj);
                    } else if (c.owner && !c.owner->IsDynamicForm() && StampSeatOk()) {
                        // a plain stack arrives with NO owner (RemoveItem moves base + count only), which would launder the theft: remove it
                        // and add what left the chest to the follower WITH an ExtraOwnership of its owner. The engine either ADOPTS the
                        // new list (links it into his entry) or MERGES its count into an equivalent list he already holds and drops ours
                        // unlinked and unfreed: then it is destroyed here (the engine's destructor + free), not leaked.
                        chest->RemoveItem(obj, c.n, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                        moved = before - CountIn(chest, obj);
                        if (moved > 0) {
                            std::vector<HeldList> held;
                            HeldLists(f, obj, held);
                            auto* xl2 = new RE::ExtraDataList();
                            xl2->SetOwner(c.owner);
                            f->AddObjectToContainer(obj, xl2, moved, chest);
                            switch (WhereListLanded(f, obj, xl2, held, moved)) {
                                case Landed::Adopted: break;
                                case Landed::Merged:
                                    DestroyUnadoptedList(xl2);
                                    spdlog::info("[steal] F={:08X} chest={:08X}: item {:08X}x{} merged into a stack he already held with that owner -- "
                                                 "the new ownership list was not adopted and is destroyed (engine destructor + free)", a_fid, a_cid, obj->GetFormID(), moved);
                                    break;
                                case Landed::Unknown:   // never guessed: destroying a list something else holds is worse than one list not reclaimed
                                    spdlog::error("[steal] F={:08X} chest={:08X}: item {:08X}x{}: the new ownership list is neither linked into his inventory nor "
                                                  "merged into a stack he held (something moved the item during the add?) -- left alone, ONE ExtraDataList is not "
                                                  "reclaimed", a_fid, a_cid, obj->GetFormID(), moved);
                                    break;
                            }
                            how = "stamped";
                        }
                    } else {
                        const bool stampable = c.owner && !c.owner->IsDynamicForm();
                        spdlog::warn("[steal] F={:08X} chest={:08X}: item {:08X}x{} {} ({:08X}): moved UNSTAMPED", a_fid, a_cid, obj->GetFormID(), c.n,
                                     stampable ? "cannot be stamped, the stamp road is refused on this runtime" : "has no stampable owner",
                                     c.owner ? c.owner->GetFormID() : 0u);
                        chest->RemoveItem(obj, c.n, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, f);
                        moved = before - CountIn(chest, obj);
                        how = "unstamped";
                    }
                    if (moved <= 0) {   // nothing left the chest: no STOLE line, no count, and no second try at the same copy
                        spdlog::warn("[steal] F={:08X} chest={:08X}: item {:08X}x{} did NOT leave the chest (RemoveItem moved nothing; {} before and after) -- "
                                     "this item is left", a_fid, a_cid, obj->GetFormID(), c.n, before);
                        break;
                    }
                    movedTotal += moved;
                    TheftGate::LogOwnedCopy(f, chest, obj, moved, how);
                    spdlog::info("[steal] F={:08X} chest={:08X}: STOLE item={:08X}({}) x{} road=owned-chest ownership={} owner={:08X} (pick + witness pre-check Take, proved on main)",
                                 a_fid, a_cid, obj->GetFormID(), NameOf(obj), moved, how, c.owner ? c.owner->GetFormID() : 0u);
                }
                // copies that were not owned by someone else follow the normal theft-gate road (a plain RemoveItem if none is barred)
                if (TheftGate::Takeable(f, chest, obj)) {
                    auto inv = chest->GetInventory([&](RE::TESBoundObject& x) { return &x == obj; });
                    if (auto it = inv.find(obj); it != inv.end() && it->second.first > 0)
                        movedTotal += TheftGate::Take(f, chest, obj, it->second.first, TheftGate::Road::Container);
                }
            }
            Finish(a_fid, a_cid, movedTotal > 0 ? OwnedTake::Took : OwnedTake::Nothing, {});
        }
    }

    bool IsOwnedChest(RE::TESObjectREFR* a_chest) {
        return a_chest && (a_chest->GetOwner() != nullptr || a_chest->IsOffLimits());
    }

    const char* OwnedChestBarred(RE::Actor* a_follower, RE::TESObjectREFR* a_chest) {
        if (!a_follower || !a_chest) return "no chest";
        if (Lotd::IsDisplayRef(a_chest->GetFormID())) return "a LOTD museum display";
        if (IsLOTDDropOff(a_chest)) return "a LOTD drop-off / sell box";
        if (RefInPlayerStorage(a_chest)) return "player storage / player home (museum halls included)";
        if (IsQuestObjectRef(a_chest)) return "a quest container";
        if (IsFactionServiceContainer(a_chest)) return "a faction's vendor / stolen-goods / evidence container";
        // The EFFECTIVE owner: the chest's own, else the CELL owner of an off-limits chest (the owner the take stamps and the witness
        // check reads). Both bars below apply to it (steal road only; TheftGate unchanged).
        RE::TESForm* owner = a_chest->GetOwner();
        if (!owner && a_chest->IsOffLimits()) owner = CellOwner(a_chest);
        if (!owner) return nullptr;
        auto* pc = RE::PlayerCharacter::GetSingleton();
        // F2: a chest owned by a FACTION the player or the follower is in is not stolen from either
        if (auto* fac = owner->As<RE::TESFaction>()) {
            if ((pc && pc->IsInFaction(fac)) || a_follower->IsInFaction(fac)) return "owned by a faction the player or the follower is in";
        }
        // owned by the player's side or the follower's own (his base NPC or its template chain): not someone else's, so not this road
        // (the same owners TheftGate::TakeBarred lets through)
        constexpr RE::FormID kPlayerBaseNpc = 0x00000007, kPlayerFaction = 0x00000DB1;
        if (owner->GetFormID() == kPlayerBaseNpc || owner->GetFormID() == kPlayerFaction || IsOwnerActor(pc, owner) || IsOwnerActor(a_follower, owner))
            return "owned by the player's own side or the follower's";
        return nullptr;
    }

    OwnedTake TakeOwnedContents(RE::Actor* a_follower, RE::TESObjectREFR* a_chest, std::string* a_why) {
        if (!a_follower || !a_chest) { if (a_why) *a_why = "unresolved actor or chest"; return OwnedTake::Cut; }
        const RE::FormID fid = a_follower->GetFormID(), cid = a_chest->GetFormID();
        {
            std::scoped_lock lk(g_otMx);
            if (auto it = g_ot.find(fid); it != g_ot.end() && it->second.chest == cid) {
                if (it->second.posted) return OwnedTake::Posted;
                const OwnedTake r = it->second.res;
                if (a_why) *a_why = std::move(it->second.why);
                g_ot.erase(it);   // read once; a Retry re-posts on the next call
                return r;
            }
            g_ot[fid] = OwnedJob{ cid, true, OwnedTake::Posted, {} };
        }
        if (!MainThread::IsInstalled()) {   // VR: no main-thread reads, so no proof, so no steal (fail closed)
            std::scoped_lock lk(g_otMx);
            g_ot.erase(fid);
            if (a_why) *a_why = "no main-thread pump (the witness pre-check cannot run)";
            return OwnedTake::Cut;
        }
        MainThread::Post([fid, cid]() { OwnedTakeOnMain(fid, cid); });
        return OwnedTake::Posted;
    }

    void ClearOwnedTakes() {
        std::scoped_lock lk(g_otMx);
        g_ot.clear();
    }
}
