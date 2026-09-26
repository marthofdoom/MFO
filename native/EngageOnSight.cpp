// EngageOnSight.cpp -- the hidden out-of-combat gambit "nearest visible enemy"
// (ClickUp 86e3errnu). Contract, threads and the no-loop rule: EngageOnSight.h.
// Harbinger side: Docs/INTEGRATION.md "Starting a fight" (ch.21) and "The recipe:
// enter combat + pin"; MFO's ch.21 claim table is apmf/CombatEntry.cpp.
#include "PCH.h"
#include "EngageOnSight.h"
#include "apmf/APMFBridge.h"      // ch.21 entry table (CombatEntryOffered / RequestCombatEntry / ...)
#include "Config.h"
#include "Confidence.h"           // LeashRadius (the enemy must be inside it, from the player) / OfFacing
#include "MainThread.h"
#include "Sightline.h"            // MeasureNow -- the SEES test, on the main thread
#include "Targeting.h"            // the ch.20 pin (or the latch) for the same target
#include "logistics/Logistics.h"  // ReleaseTravelOnCombat -- the engage ends his loot trip

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MFO::EngageOnSight {

    namespace {

        using Clock = std::chrono::steady_clock;

        // One probe per follower at most this often. Sightline's own per-viewer repost
        // floor (kRepostSeconds) -- the scan and up to kMaxMeasured sightline
        // measurements are bounded to the same ~3 Hz per follower the combat
        // selectors' LoS already runs at, not the 133 ms pump.
        constexpr float kProbeFloorSecs = 0.3f;
        // Nearest-first, the sightline is measured for at most this many non-given-up
        // candidates per probe (the first VISIBLE one wins). Each measurement is one
        // engine HasLineOfSight plus, only on a CLEAR, MFO's 3-sample ray.
        constexpr int kMaxMeasured = 3;
        // Status lines (why the gambit is not acting) are transition-only (#79) with
        // this floor between two lines for one follower, so a gate that flaps (the
        // player crouching, confidence at the floor) cannot flood the log.
        constexpr float kStatusFloorSecs = 5.0f;

        // ── MAIN-THREAD PROBE MIRROR ─────────────────────────────────────────────
        // Written by the main-thread probe, read by the worker. A real lock (#4:
        // cross-thread); a LEAF -- nothing is called while it is held.
        struct ProbeResult {
            std::uint32_t seq = 0;                  // the post sequence this answers (g_seq)
            std::vector<RE::FormID> candidates;     // every enemy in the leash, nearest (to him) first
            RE::FormID chosen   = 0;                // nearest non-given-up candidate measured VISIBLE
            float      dSelf    = 0.0f;             // chosen: distance from him
            float      dPlayer  = 0.0f;             // chosen: distance from the player
            float      leash    = 0.0f;             // the leash the probe used
            int        measured = 0;                // sightline measurements made
            int        occluded = 0;                // of those, OCCLUDED (or unknown)
            // Given-up targets this probe found GONE: unresolvable, dead, disabled,
            // unloaded, no longer hostile to the player or him (the bare engine read), or
            // farther from the player than the FIXED ceiling fLeashMax. Never keyed on the
            // confidence-scaled leash (review of 1c50696, SEV-3: a follower whose leash
            // shrank would forget a refused target still standing there, and loop).
            std::vector<RE::FormID> gone;
            // TOWN / INN FILTER (marth 2026-09-25): the player stands in a civilised place
            // and is not fighting -> no candidates at all this probe. civilSkipped = enemies
            // skipped because THEIR location is civilised (player not fighting).
            bool civilStandDown = false;
            int  civilSkipped   = 0;
        };
        std::mutex                                   g_probeMx;
        std::unordered_map<RE::FormID, ProbeResult>  g_probes;   // guarded by g_probeMx
        // Session generation: a probe posted before a revert/load that lands after it
        // writes nothing (ClearTransientState bumps it inside the StopPump bracket).
        std::atomic<std::uint32_t>                   g_gen{ 1 };
        // ONE monotonic post sequence for every follower, worker-only, NEVER reset (review
        // of 1c50696, SEV-3). A per-follower counter restarted at 0 when Forget erased his
        // note, so a probe still in flight from before landed with a HIGHER seq than his
        // new posts: the mirror's newer-wins guard then rejected every fresh result (he
        // was blind) until his counter caught up and matched the stale one, which then
        // engaged. With one monotonic counter a stale probe is always OLDER than any post
        // made after Forget: it can neither block a fresh result nor equal his latest post.
        std::uint32_t                                g_seq = 0;

        // ── WORKER STATE (worker-only, no lock -- #4) ────────────────────────────
        struct Note {
            std::uint32_t postedSeq   = 0;          // last probe this follower posted (a g_seq value)
            std::uint32_t consumedSeq = 0;          // last probe result acted on
            Clock::time_point lastPost{};
            // Given-up targets -> the postedSeq current when the entry ended. A target
            // leaves this map only when a probe posted AFTER that (seq > value) reports it
            // GONE (ProbeResult::gone).
            std::unordered_map<RE::FormID, std::uint32_t> givenUp;
            std::string       status;               // last status KEY (stable text, no live values)
            Clock::time_point statusAt{};
            // The target this gambit pinned through Targeting on its last engage (0 =
            // none). Released (if Targeting still holds that target) when the entry ends
            // and on the bEngageOnSight kill switch (review of 1c50696).
            RE::FormID        pinned = 0;
        };
        std::unordered_map<RE::FormID, Note> g_notes;

        const char* NameOf(RE::Actor* a_actor) {
            const char* n = a_actor ? a_actor->GetName() : nullptr;
            return (n && *n) ? n : "?";
        }

        float SecsSince(Clock::time_point a_t, Clock::time_point a_now) {
            if (a_t.time_since_epoch().count() == 0) return 1.0e9f;
            return std::chrono::duration<float>(a_now - a_t).count();
        }

        // Transition-only status line, floored per follower. a_key is STABLE text (the
        // transition key: no live value in it, or every changing number would be a
        // "transition"); a_detail carries the live values and is printed, never compared.
        // An EMPTY key resets silently (the toggle off is not news).
        void Status(Note& a_note, RE::Actor* a_f, RE::FormID a_id, const char* a_key, Clock::time_point a_now,
                    const std::string& a_detail = {}) {
            if (a_note.status == a_key) return;
            if (!*a_key) { a_note.status.clear(); return; }
            if (SecsSince(a_note.statusAt, a_now) < kStatusFloorSecs) return;   // keep the old key: re-tried next lap
            a_note.status   = a_key;
            a_note.statusAt = a_now;
            spdlog::info("[engage-on-sight] {} ({:08X}): {}{}", NameOf(a_f), a_id, a_key, a_detail);
        }

        const char* PinOutcomeName(Targeting::CommandOutcome a_o) {
            switch (a_o) {
            case Targeting::CommandOutcome::Changed:     return "pinned";
            case Targeting::CommandOutcome::Unchanged:   return "already pinned";
            case Targeting::CommandOutcome::Suppressed:  return "suppressed (Harbinger ended a pin on this foe)";
            default:                                     return "unavailable";
            }
        }

        // The pin this gambit filed, released if Targeting still holds it on that target
        // (once he fought, his own gambits may have moved it: then it is theirs).
        bool ReleaseOwnPin(Note& a_note, RE::FormID a_id) {
            const RE::FormID t = a_note.pinned;
            a_note.pinned = 0;
            if (!t) return false;
            if (auto cur = Targeting::Current(a_id).get(); cur && cur->GetFormID() == t) {
                Targeting::Clear(a_id);
                return true;
            }
            return false;
        }

        // DESIGN DEFAULT (i), pending marth (review of 1c50696): a follower TOLD TO WAIT
        // does not engage. Vanilla "wait here" sets the WaitingForPlayer actor value (the
        // one MFO's [follower] state line already reads); a follower idling on a Sandbox
        // package is the other "wait" shape. One branch; plain data reads on the worker.
        bool ToldToWait(RE::Actor* a_f) {
            if (auto* avo = a_f->AsActorValueOwner();
                avo && avo->GetActorValue(RE::ActorValue::kWaitingForPlayer) > 0.0f)
                return true;
            const auto* pkg = a_f->GetCurrentPackage();
            return pkg && pkg->packData.packType.get() == RE::PACKAGE_PROCEDURE_TYPE::kSandbox;
        }

        // MAIN THREAD. The ENEMY test (review of 1c50696, SEV-2). Actor::IsHostileToActor
        // alone is the engine's "would attack" predicate (aggression x faction reaction,
        // disassembled AE 0x679F10 by the reviewer): with a bounty, as a stage-4 vampire or
        // a werewolf it names every guard and townsperson, and starting that fight is a
        // hold-wide crime. So, in this order:
        //   (b) a COMMANDED actor whose commander is the player or a teammate (a summon,
        //       a thrall) is never an enemy;
        //   (c) a restrained or bleeding-out actor, or one running an IgnoreCombat
        //       package, is not one to start a fight with. (A GHOST check is NOT here:
        //       Actor::IsGhost is an id call, RELOCATION_ID(36286, 37275), with no row in
        //       VerifiedAddresses.h -- backlogged as MFO-B111.);
        //   then hostile to the player OR to him (design default (ii) below);
        //   (a) an actor with a CRIME FACTION (guards, townsfolk: anyone whose attack is a
        //       crime) counts only when it is ALREADY fighting the party: its
        //       currentCombatTarget resolves to the player or a teammate.
        // DESIGN DEFAULT (ii), pending marth: hostile to HIM alone (not to the player)
        // still counts -- the spec says "hostile to the player/party", and he is party.
        bool IsEnemy(RE::Actor* a, RE::Actor* a_self, RE::Actor* a_pc) {
            if (a->IsCommandedActor()) {
                auto cmd = a->GetCommandingActor();   // HOLD the NiPointer
                if (cmd && (cmd->IsPlayerRef() || cmd->IsPlayerTeammate())) return false;
            }
            if (const auto* st = a->AsActorState()) {
                if (st->GetLifeState() == RE::ACTOR_LIFE_STATE::kRestrained || st->IsBleedingOut()) return false;
            }
            if (const auto* pkg = a->GetCurrentPackage();
                pkg && pkg->packData.packFlags.any(RE::PACKAGE_DATA::GeneralFlag::kIgnoreCombat))
                return false;
            if (!a->IsHostileToActor(a_pc) && !a->IsHostileToActor(a_self)) return false;
            if (a->GetCrimeFaction()) {
                auto  tp = a->GetActorRuntimeData().currentCombatTarget.get();   // HOLD
                auto* t  = tp.get();
                if (!t || !(t->IsPlayerRef() || t->IsPlayerTeammate())) return false;
            }
            return true;
        }

        // MAIN THREAD. TOWN / INN FILTER (marth 2026-09-25: "filter out enemies in Inns or
        // Towns, UNLESS the player is in a battle"). A location is CIVILISED when, walking
        // from the innermost location out through parentLoc, a civilised keyword is met
        // BEFORE a hostile-place one. Hostile-place = LocTypeClearable / LocTypeDungeon: a
        // bandit mine or the Ratway under Riften is a fight site even though its parent is
        // a town, and the innermost tag decides. Civilised = every Skyrim.esm LocType that
        // marks a lived-in place (erring toward civilised, per marth): Inn, City, Town,
        // Settlement, Habitation, HabitationHasInn, Dwelling, House, PlayerHouse, Store,
        // Guild, Temple, Castle, Barracks, Jail, StewardsDwelling, Farm, Mine, LumberMill,
        // OrcStronghold. NOT the LocTypeHold* family: those tag a whole hold, wilderness
        // included. Resolved ONCE by FormID in Skyrim.esm (EditorIDs are not reliably kept;
        // the same road as logistics' InPlayerHome); a keyword that does not resolve is
        // simply absent from the set. FormIDs verified against Skyrim.esm's KYWD records.
        bool IsCivilised(const RE::BGSLocation* a_loc) {
            struct Kw { RE::FormID id; bool civil; };
            static const std::vector<std::pair<const RE::BGSKeyword*, bool>> s_kws = [] {
                constexpr Kw kList[] = {
                    { 0x000F5E80, false },   // LocTypeClearable
                    { 0x000130DB, false },   // LocTypeDungeon
                    { 0x0001CB87, true },    // LocTypeInn
                    { 0x00013168, true },    // LocTypeCity
                    { 0x00013166, true },    // LocTypeTown
                    { 0x00013167, true },    // LocTypeSettlement
                    { 0x00039793, true },    // LocTypeHabitation
                    { 0x000A6E84, true },    // LocTypeHabitationHasInn
                    { 0x000130DC, true },    // LocTypeDwelling
                    { 0x0001CB85, true },    // LocTypeHouse
                    { 0x000FC1A3, true },    // LocTypePlayerHouse
                    { 0x0001CB86, true },    // LocTypeStore
                    { 0x0001CD5A, true },    // LocTypeGuild
                    { 0x0001CD56, true },    // LocTypeTemple
                    { 0x0001CD57, true },    // LocTypeCastle
                    { 0x0001CD55, true },    // LocTypeBarracks
                    { 0x0001CD59, true },    // LocTypeJail
                    { 0x000504F9, true },    // LocTypeStewardsDwelling
                    { 0x00018EF0, true },    // LocTypeFarm
                    { 0x00018EF1, true },    // LocTypeMine
                    { 0x00018EF2, true },    // LocTypeLumberMill
                    { 0x000130E9, true },    // LocTypeOrcStronghold
                };
                std::vector<std::pair<const RE::BGSKeyword*, bool>> v;
                auto* dh = RE::TESDataHandler::GetSingleton();
                for (const auto& k : kList) {
                    auto* kw = dh ? dh->LookupForm<RE::BGSKeyword>(k.id & 0x00FFFFFF, "Skyrim.esm") : nullptr;
                    if (kw) v.emplace_back(kw, k.civil);
                }
                spdlog::info("[engage-on-sight] town/inn filter: {} of {} Skyrim.esm location keywords resolved",
                             v.size(), std::size(kList));
                return v;
            }();
            int guard = 0;
            for (auto* loc = a_loc; loc && guard < 8; loc = loc->parentLoc, ++guard) {
                for (const auto& [kw, civil] : s_kws) {
                    if (!civil && loc->HasKeyword(kw)) return false;   // a fight site, innermost first
                }
                for (const auto& [kw, civil] : s_kws) {
                    if (civil && loc->HasKeyword(kw)) return true;
                }
            }
            return false;
        }

        // MAIN THREAD ONLY (posted). The highActorHandles walk (resized by the main
        // thread, §0.30) and the sightline measurement (a physics query, §0.30) both
        // run here. Everything crosses back as FormIDs.
        void RunProbe(RE::FormID a_id, std::uint32_t a_seq, std::uint32_t a_gen, float a_leash,
                      float a_goneRadius, const std::vector<RE::FormID>& a_givenUp) {
            if (g_gen.load(std::memory_order_relaxed) != a_gen) return;
            auto* self = RE::TESForm::LookupByID<RE::Actor>(a_id);
            auto* pc   = RE::PlayerCharacter::GetSingleton();
            auto* pl   = RE::ProcessLists::GetSingleton();
            if (!self || self->IsDead() || !self->Is3DLoaded() || !pc || !pl) return;   // does not land
            const auto selfPos = self->GetPosition();
            const auto pcPos   = pc->GetPosition();

            // TOWN / INN FILTER, on unless the PLAYER is fighting (read here on main; in the
            // party-OOC branch that calls Service the player is normally out of combat, so
            // this mostly reads false -- the "unless" is honoured for the frame it is not).
            const bool pcFighting = pc->IsInCombat();
            ProbeResult r;
            r.seq   = a_seq;
            r.leash = a_leash;
            r.civilStandDown = !pcFighting && IsCivilised(pc->GetCurrentLocation());

            std::vector<std::pair<float, RE::FormID>> found;
            for (auto& hh : pl->highActorHandles) {
                if (r.civilStandDown) break;
                auto  ptr = hh.get();          // HOLD the NiPointer
                auto* a   = ptr.get();
                if (!a || a == self) continue;
                if (a->IsPlayerRef() || a->IsPlayerTeammate()) continue;
                if (a->IsDead() || a->IsDisabled() || !a->Is3DLoaded()) continue;
                if (a->GetPosition().GetDistance(pcPos) > a_leash) continue;   // the leash is from the PLAYER
                if (!IsEnemy(a, self, pc)) continue;
                if (!pcFighting && IsCivilised(a->GetCurrentLocation())) { ++r.civilSkipped; continue; }
                found.emplace_back(a->GetPosition().GetDistance(selfPos), a->GetFormID());
            }
            std::sort(found.begin(), found.end());

            r.candidates.reserve(found.size());
            for (const auto& [d, fid] : found) r.candidates.push_back(fid);
            for (const auto& [d, fid] : found) {
                if (r.measured >= kMaxMeasured) break;
                if (std::find(a_givenUp.begin(), a_givenUp.end(), fid) != a_givenUp.end()) continue;
                ++r.measured;
                if (Sightline::MeasureNow(a_id, fid) != Sightline::Verdict::Visible) { ++r.occluded; continue; }
                auto* t = RE::TESForm::LookupByID<RE::Actor>(fid);
                r.chosen  = fid;
                r.dSelf   = d;
                r.dPlayer = t ? t->GetPosition().GetDistance(pcPos) : 0.0f;
                break;
            }
            // Given-up targets that are GONE (see ProbeResult::gone). Hostility here is the
            // bare engine read, not IsEnemy: a target stays given up while the engine still
            // calls it hostile, whatever the filters make of it.
            for (const RE::FormID fid : a_givenUp) {
                auto* g = RE::TESForm::LookupByID<RE::Actor>(fid);
                const bool gone = !g || g->IsDead() || g->IsDisabled() || !g->Is3DLoaded() ||
                                  g->GetPosition().GetDistance(pcPos) > a_goneRadius ||
                                  (!g->IsHostileToActor(pc) && !g->IsHostileToActor(self));
                if (gone) r.gone.push_back(fid);
            }

            std::lock_guard lk(g_probeMx);
            if (g_gen.load(std::memory_order_relaxed) != a_gen) return;
            auto& slot = g_probes[a_id];
            if (slot.seq >= a_seq) return;   // an older post landing late (seq starts at 1)
            slot = std::move(r);
        }

        void PostProbe(Note& a_note, RE::FormID a_id, float a_leash, Clock::time_point a_now) {
            if (SecsSince(a_note.lastPost, a_now) < kProbeFloorSecs) return;
            a_note.lastPost = a_now;
            const std::uint32_t seq = ++g_seq;   // global and monotonic (see g_seq)
            a_note.postedSeq = seq;
            const std::uint32_t gen = g_gen.load(std::memory_order_relaxed);
            // The FIXED "gone" radius: the leash ceiling, read now (the MCM may change it).
            const float goneRadius = Config::g_leashMax.load();
            std::vector<RE::FormID> givenUp;
            givenUp.reserve(a_note.givenUp.size());
            for (const auto& [fid, s] : a_note.givenUp) givenUp.push_back(fid);
            // Capture by value: FormIDs and numbers only (the frame re-resolves).
            MainThread::Post([a_id, seq, gen, a_leash, goneRadius, givenUp = std::move(givenUp)]() {
                RunProbe(a_id, seq, gen, a_leash, goneRadius, givenUp);
            });
        }

    }

    bool Service(RE::Actor* a_f, RE::FormID a_id, bool a_retreatCooling, float a_minConfidence) {
        if (!a_f || !a_id) return false;
        const auto now = Clock::now();
        auto& note = g_notes[a_id];

        // 1) HIS ENTRY, if he holds one. Runs whatever the gates say, so an entry the
        //    Harbinger sweep ended is always recorded as given up.
        {
            RE::FormID et = 0;
            const auto st = APMFBridge::CombatEntryStateOf(a_id, &et);
            if (st == APMFBridge::EntryState::Standing) return true;   // pending: his lap, no logistics
            if (st == APMFBridge::EntryState::Ended) {
                note.givenUp[et] = note.postedSeq;
                // The recipe drops both handles. Release the pin only if it is still OURS.
                const bool pinCleared = note.pinned == et && ReleaseOwnPin(note, a_id);
                APMFBridge::ForgetCombatEntry(a_id);
                spdlog::info("[engage-on-sight] {} ({:08X}): entry on {:08X} is over -- target given up until it "
                             "dies, unloads, stops being hostile or is past fLeashMax from you{}",
                             NameOf(a_f), a_id, et, pinCleared ? "; its pin released" : "");
            }
        }

        // 2) GATES. Off is silent (and releases the pin this gambit filed: the kill
        //    switch; the entry claims are released by the bridge's sweep). Every other
        //    stand-down is a transition line with a STABLE key.
        if (!Config::g_engageOnSight.load()) {
            if (note.pinned && ReleaseOwnPin(note, a_id))
                spdlog::info("[engage-on-sight] {} ({:08X}): bEngageOnSight off -- released the pin it filed",
                             NameOf(a_f), a_id);
            Status(note, a_f, a_id, "", now);
            return false;
        }
        if (REL::Module::IsVR()) {
            Status(note, a_f, a_id, "inert: VR (Harbinger serves no ch.21 there, and MFO has no direct road)", now);
            return false;
        }
        if (!Config::g_autoRetreat.load()) {
            Status(note, a_f, a_id, "standing down: bAutoRetreat is OFF (the gambit runs only while a follower "
                                    "who starts a fight can break it off)", now);
            return false;
        }
        if (!APMFBridge::CombatEntryOffered()) {
            Status(note, a_f, a_id, "inert: Harbinger ch.21 combat entry unavailable (Harbinger absent, older "
                                    "than ABI 14, or its ch.21 seat refused) -- MFO has no direct road", now);
            return false;
        }
        if (!Config::g_engageOnSightSneaking.load()) {
            if (auto* pc = RE::PlayerCharacter::GetSingleton(); pc && pc->IsSneaking()) {
                Status(note, a_f, a_id, "standing down: you are sneaking (bEngageOnSightSneaking is OFF)", now);
                return false;
            }
        }
        if (ToldToWait(a_f)) {
            Status(note, a_f, a_id, "standing down: told to wait (WaitingForPlayer set, or on a sandbox package)", now);
            return false;
        }
        if (a_retreatCooling) {
            Status(note, a_f, a_id, "standing down: auto-retreat cooldown", now);
            return false;
        }

        // 3) THE NEWEST PROBE RESULT (the one answering his last post), once.
        ProbeResult r;
        bool have = false;
        {
            std::lock_guard lk(g_probeMx);
            if (auto it = g_probes.find(a_id);
                it != g_probes.end() && it->second.seq == note.postedSeq && it->second.seq > note.consumedSeq) {
                r    = it->second;
                have = true;
            }
        }
        bool engaged = false;
        if (have) {
            note.consumedSeq = r.seq;
            // A given-up target leaves the set once a probe posted AFTER it was given up
            // reports it GONE. Seeing it again after that is a FRESH sighting.
            for (auto it = note.givenUp.begin(); it != note.givenUp.end();) {
                const bool gone = std::find(r.gone.begin(), r.gone.end(), it->first) != r.gone.end();
                if (r.seq > it->second && gone) {
                    spdlog::info("[engage-on-sight] {} ({:08X}): {:08X} is gone (dead, unloaded, no longer hostile "
                                 "or past fLeashMax) -- no longer given up", NameOf(a_f), a_id, it->first);
                    it = note.givenUp.erase(it);
                } else {
                    ++it;
                }
            }
            if (r.civilStandDown) {
                Status(note, a_f, a_id, "standing down: in a town/inn, player not fighting", now);
            } else if (!r.chosen) {
                if (r.civilSkipped > 0 && r.candidates.empty())
                    Status(note, a_f, a_id, "standing down: the enemies in view are in a town/inn, player not fighting",
                           now, fmt::format(" -- {} skipped", r.civilSkipped));
                else
                    Status(note, a_f, a_id, "armed: watching for a visible enemy inside the leash", now);
            } else if (!note.givenUp.contains(r.chosen)) {
                // The confidence gate on the IN-COMBAT estimate (review of 1c50696): the Of()
                // he would read once fighting as many foes as the probe counted -- so he never
                // starts a fight he would at once retreat from.
                const int   foes = static_cast<int>(r.candidates.size());
                const float conf = Confidence::OfFacing(a_f, foes);
                auto* t = RE::TESForm::LookupByID<RE::Actor>(r.chosen);
                if (conf < a_minConfidence) {
                    Status(note, a_f, a_id, "standing down: his in-combat confidence estimate is under the retreat "
                                            "floor (he would start a fight only to flee it)", now,
                           fmt::format(" -- estimate {:.2f} against {} foe(s), floor {:.2f}", conf, foes,
                                       a_minConfidence));
                } else if (t && !t->IsDead() && !t->IsDisabled()) {
                    std::uint32_t h = 0;
                    switch (APMFBridge::RequestCombatEntry(a_id, r.chosen, &h)) {
                    case APMFBridge::EntryResult::Filed: {
                        // The recipe's second handle: pin the same target (ch.20 waits until the
                        // entry has made it a combat-group target). Through Targeting, so there is
                        // ONE pin per follower and his foe gambits re-point it once he fights.
                        const char* pin = "off (bCommandTarget)";
                        if (Targeting::Commandable()) {
                            const auto o = Targeting::CommandEx(a_id, t->GetHandle());
                            pin = PinOutcomeName(o);
                            if (o == Targeting::CommandOutcome::Changed || o == Targeting::CommandOutcome::Unchanged)
                                note.pinned = r.chosen;
                        }
                        // Preempt logistics the way player combat does: his loot trip ends now.
                        Logistics::ReleaseTravelOnCombat(a_f);
                        Status(note, a_f, a_id, "armed: watching for a visible enemy inside the leash", now);
                        spdlog::info("[engage-on-sight] {} ({:08X}) ENGAGE {} ({:08X}): nearest visible enemy, "
                                     "{:.0f}u from him, {:.0f}u from you (leash {:.0f}u), sightline=VISIBLE "
                                     "({} measured, {} not visible), {} candidate(s); ch.21 entry h={}; "
                                     "ch.20 pin: {}; in-combat confidence estimate {:.2f}",
                                     NameOf(a_f), a_id, NameOf(t), r.chosen, r.dSelf, r.dPlayer, r.leash,
                                     r.measured, r.occluded, r.candidates.size(), h, pin, conf);
                        engaged = true;
                        break;
                    }
                    case APMFBridge::EntryResult::Standing:
                        engaged = true;   // cannot happen (checked above); treat as pending
                        break;
                    case APMFBridge::EntryResult::SeatAbsent:   // logged once by the bridge
                    case APMFBridge::EntryResult::Invalid:
                    default:
                        break;
                    }
                }
            }
        }
        if (engaged) return true;

        // 4) THE NEXT PROBE. The leash is read here, on the worker, exactly as the loot
        //    leash is (Confidence::LeashRadius -- out of combat it is vitality only).
        PostProbe(note, a_id, Confidence::LeashRadius(a_f), now);
        return false;
    }

    void Forget(RE::FormID a_id) {
        APMFBridge::ForgetCombatEntry(a_id);
        g_notes.erase(a_id);   // g_seq is NOT reset: a probe still in flight stays older than any new post
        std::lock_guard lk(g_probeMx);
        g_probes.erase(a_id);
    }

    void ClearTransientState() {
        g_gen.fetch_add(1, std::memory_order_relaxed);
        g_notes.clear();
        std::lock_guard lk(g_probeMx);
        g_probes.clear();
    }

}
