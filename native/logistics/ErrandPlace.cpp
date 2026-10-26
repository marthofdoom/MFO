// logistics/ErrandPlace.cpp -- the UNSTICK of a town errand (TOWN RUN 5, marth 2026-10-20 R1 / R4; ClickUp 86e3t1mx7).
// R1: "ALWAYS teleport a stuck follower to his destination, even in the player's view." R4: "Any real verified intentional cell being
// unreachable is an error": a stuck walk is PLACED, never silently dropped, and a placement that fails is an ERROR, never a mask.
// The engine action is Harbinger's kIntent_Place (ABI v28; apmf/Place.cpp is the thin wrapper): ONE TESObjectREFR::MoveTo_Impl on the game
// thread, no claim, no facet. MFO only DECLARES the spot. The trip's ch.19 claim is MFO's own and Place leaves it standing (Harbinger's
// arbitration refuses a placement only over ANOTHER client's travel / hold claim), so on Placed the walk simply carries on from the new spot.
//
// Destination of the CURRENT stop (generic, no per-town or per-door knowledge):
//   * a ref stop (a vendor actor / a PLVD ref): beside the ref (kFrontU in front of it, in its own cell);
//   * a cell stop (an inn / a shop interior): the interior side of its load door. The EXTERIOR door's DoorTeleportData (position + rotation)
//     is where the engine lands an actor that takes the door, in the interior cell; read from the exterior door whose linked door sits in the
//     stop's cell (a MAIN-thread read of the loaded grid, Logistics::ForEachLoadedRef). Sanity: that landing point must be within
//     kLandingSanityU of the linked (interior) door, else the placement is beside the linked door instead. No door among the loaded
//     references -> beside a loaded member of the stop's vendor faction (the innkeeper / the vendor); nothing at all -> Failed (an ERROR).
// Threading: Begin / Poll on the AddTask worker; the cell stop's door read on the main thread (MainThread::Post). Own mutex, a leaf.
// Nothing is saved.
#include "PCH.h"
#include "Errand_internal.h"
#include "Logistics.h"
#include "MainThread.h"
#include "apmf/APMFBridge.h"
#include "APMF_API.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace MFO::Logistics::Errand::Unstick {

    namespace {
        using Clock = std::chrono::steady_clock;
        constexpr float kFrontU          = 90.0f;    // beside a ref: this far in front of it
        constexpr float kLandingSanityU  = 800.0f;   // a door's landing point farther than this from its linked door is not believed
        constexpr auto  kResolveTimeout  = std::chrono::seconds(15);   // the main-thread door read must have landed by now
        constexpr auto  kAnswerTimeout   = std::chrono::seconds(10);   // Harbinger's game thread must have answered by now

        struct Job {
            bool         requested = false;   // RequestPlace returned a handle
            std::uint32_t handle = 0;
            Clock::time_point began{}, at{};
            std::string  desc;                // where he was sent (the log text)
            bool         finished = false;    // a synchronous / resolve-time failure is parked here until Poll
            Result       done;
        };
        std::mutex                                 g_mx;
        std::unordered_map<RE::FormID, Job>        g_jobs;
        std::uint64_t                              g_gen = 0;   // ClearAll bumps it: a queued main-thread read for an older generation is dropped

        RE::NiPoint3 Front(RE::TESObjectREFR* a_ref, float a_u) {
            const float a = a_ref ? a_ref->GetAngleZ() : 0.0f;   // Skyrim heading: clockwise from +Y, so forward is (sin, cos)
            return RE::NiPoint3(std::sin(a) * a_u, std::cos(a) * a_u, 0.0f);
        }
        const char* WhyText(std::uint32_t a_why) {
            switch (a_why) {
                case APMF_API::kPlaceWhy_ActorGone:     return "the actor is not an actor in memory";
                case APMF_API::kPlaceWhy_DestGone:      return "the destination form resolves to nothing";
                case APMF_API::kPlaceWhy_DestInvalid:   return "the destination is not a usable reference or cell";
                case APMF_API::kPlaceWhy_DestExterior:  return "the destination is an exterior cell";
                case APMF_API::kPlaceWhy_ActorLoaded:   return "the actor is loaded (kPlace_OnlyIfUnloaded)";
                case APMF_API::kPlaceWhy_PlayerPresent: return "the player is present (kPlace_RefuseIfPlayerPresent)";
                case APMF_API::kPlaceWhy_TravelOwned:   return "another client's ch.19 travel claim owns the actor";
                case APMF_API::kPlaceWhy_HoldOwned:     return "another client's ch.1 hold claim owns the actor";
                case APMF_API::kPlaceWhy_NotMoved:      return "the engine move ran but the position did not read back";
                case APMF_API::kPlaceWhy_Error:         return "an exception inside Harbinger's game-thread step";
                default:                                return "no reason given";
            }
        }
        void Park(RE::FormID a_fid, State a_state, std::string a_text) {
            std::scoped_lock lk(g_mx);
            auto& j = g_jobs[a_fid];
            j.finished = true;
            j.done.state = a_state;
            j.done.text = std::move(a_text);
        }
        // Declare the placement (any thread). a_gen: the generation the job was begun in.
        void Request(RE::FormID a_fid, RE::FormID a_dest, const RE::NiPoint3& a_offset, float a_yaw, std::uint32_t a_flags, std::string a_desc,
                     std::uint64_t a_gen) {
            const auto h = APMFBridge::RequestPlace(a_fid, a_dest, a_offset, a_yaw, a_flags);
            std::scoped_lock lk(g_mx);
            if (a_gen != g_gen) return;   // a revert / load dropped the job meanwhile
            auto& j = g_jobs[a_fid];
            if (h == APMF_API::kInvalidHandle) {
                j.finished = true;
                j.done.state = State::Failed;
                j.done.text = std::format("Harbinger refused the placement at the call (not installed, ABI < 28, or an invalid request; APMF.log says): {}", a_desc);
                return;
            }
            j.requested = true;
            j.handle = h;
            j.at = Clock::now();
            j.desc = std::move(a_desc);
        }

        // MAIN thread: find where a CELL stop's interior side is, then declare the placement.
        void ResolveCellOnMain(RE::FormID a_fid, RE::FormID a_cell, RE::FormID a_faction, std::uint64_t a_gen) {
            auto* f = RE::TESForm::LookupByID<RE::Actor>(a_fid);
            auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(a_cell);
            if (!f || !cell || !cell->IsInteriorCell()) {
                std::scoped_lock lk(g_mx);
                if (a_gen == g_gen) {
                    auto& j = g_jobs[a_fid];
                    j.finished = true;
                    j.done.state = State::Failed;
                    j.done.text = std::format("the stop's cell {:08X} does not resolve to an interior cell (or the follower is gone)", a_cell);
                }
                return;
            }
            auto* fac = a_faction ? RE::TESForm::LookupByID<RE::TESFaction>(a_faction) : nullptr;
            const auto here = f->GetPosition();
            float bestDoor = 1.0e9f, bestMan = 1.0e9f;
            RE::FormID doorId = 0, linkedId = 0, manId = 0;
            RE::NiPoint3 landing{}, landingRot{}, linkedPos{};
            bool haveLanding = false;
            int scanned = 0;
            Logistics::ForEachLoadedRef([&](RE::TESObjectREFR* a_ref) {
                if (!a_ref) return RE::BSContainer::ForEachResult::kContinue;
                ++scanned;
                if (auto* tele = a_ref->extraList.GetByType<RE::ExtraTeleport>(); tele && tele->teleportData) {
                    auto lp = tele->teleportData->linkedDoor.get();
                    auto* linked = lp.get();
                    auto* lc = linked ? linked->GetParentCell() : nullptr;
                    if (lc && lc->GetFormID() == a_cell) {
                        const float d = a_ref->GetPosition().GetDistance(here);
                        if (d < bestDoor) {
                            bestDoor = d;
                            doorId = a_ref->GetFormID(); linkedId = linked->GetFormID();
                            landing = tele->teleportData->position; landingRot = tele->teleportData->rotation;
                            linkedPos = linked->GetPosition();
                            haveLanding = true;
                        }
                    }
                } else if (fac) {
                    auto* a = a_ref->As<RE::Actor>();
                    if (a && a != f && !a->IsDead() && !a->IsPlayerRef() && a->GetParentCell() == cell && a->IsInFaction(fac)) {
                        const float d = a->GetPosition().GetDistance(here);
                        if (d < bestMan) { bestMan = d; manId = a->GetFormID(); }
                    }
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
            const std::string cellName = cell->GetName() ? cell->GetName() : "?";
            if (haveLanding) {
                const bool sane = landing.GetDistance(linkedPos) <= kLandingSanityU;
                if (sane) {
                    Request(a_fid, a_cell, landing, landingRot.z, APMF_API::kPlace_SetYaw,
                            std::format("the interior side of load door {:08X} of '{}' (cell {:08X}): landing point {:.0f},{:.0f},{:.0f}, {} loaded refs read",
                                        doorId, cellName, a_cell, landing.x, landing.y, landing.z, scanned), a_gen);
                } else if (auto* lref = RE::TESForm::LookupByID<RE::TESObjectREFR>(linkedId)) {
                    Request(a_fid, linkedId, Front(lref, kFrontU), 0.0f, 0u,
                            std::format("beside the interior door {:08X} of '{}' (cell {:08X}): the exterior door {:08X}'s landing point is {:.0f} u from it, "
                                        "not believed", linkedId, cellName, a_cell, doorId, landing.GetDistance(linkedPos)), a_gen);
                } else {
                    std::scoped_lock lk(g_mx);
                    if (a_gen == g_gen) {
                        auto& j = g_jobs[a_fid];
                        j.finished = true; j.done.state = State::Failed;
                        j.done.text = std::format("the interior door {:08X} of '{}' does not resolve", linkedId, cellName);
                    }
                }
                return;
            }
            if (manId) {
                if (auto* mref = RE::TESForm::LookupByID<RE::TESObjectREFR>(manId)) {
                    Request(a_fid, manId, Front(mref, kFrontU), 0.0f, 0u,
                            std::format("beside {:08X} (a loaded member of faction {:08X}) in '{}' (cell {:08X}): no load door to it among {} loaded refs",
                                        manId, a_faction, cellName, a_cell, scanned), a_gen);
                    return;
                }
            }
            std::scoped_lock lk(g_mx);
            if (a_gen == g_gen) {
                auto& j = g_jobs[a_fid];
                j.finished = true; j.done.state = State::Failed;
                j.done.text = std::format("no destination for '{}' (cell {:08X}): none of the {} loaded refs is a load door into it or a member of faction {:08X}",
                                          cellName, a_cell, scanned, a_faction);
            }
        }
    }

    bool Begin(RE::FormID a_follower, const Stop& a_stop, std::string& a_err) {
        if (!APMFBridge::PlaceSupported()) {
            a_err = "Harbinger is absent or older than ABI 28: there is no placement facet";
            return false;
        }
        std::uint64_t gen = 0;
        {
            std::scoped_lock lk(g_mx);
            gen = g_gen;
            Job j;
            j.began = Clock::now();
            g_jobs[a_follower] = std::move(j);
        }
        if (a_stop.destKind == DestKind::Cell) {
            if (!a_stop.destForm) { Clear(a_follower); a_err = "the cell stop has no destination cell"; return false; }
            const RE::FormID cell = a_stop.destForm, fac = a_stop.faction;
            auto fn = [a_follower, cell, fac, gen]() { ResolveCellOnMain(a_follower, cell, fac, gen); };
            if (MainThread::IsInstalled()) MainThread::Post(fn);
            else                           fn();
            return true;
        }
        auto* ref = a_stop.destForm ? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_stop.destForm) : nullptr;
        if (!ref) { Clear(a_follower); a_err = std::format("the stop's destination reference {:08X} does not resolve", a_stop.destForm); return false; }
        // v2.7.1: inside the leg's own arrival radius (run 6: placed 90 u from a ref whose leg radius was 50 stood 120 s until the stuck timeout).
        const float frontU = a_stop.destRad > 0.0f ? std::clamp(0.5f * a_stop.destRad, 20.0f, kFrontU) : kFrontU;
        Request(a_follower, a_stop.destForm, Front(ref, frontU), 0.0f, 0u,
                std::format("beside the {} {:08X} ('{}')", a_stop.destKind == DestKind::VendorActor ? "vendor" : "shop ref", a_stop.destForm, a_stop.vendorName), gen);
        return true;   // a refusal at the call is parked by Request and read by the next Poll
    }

    Result Poll(RE::FormID a_follower) {
        Job j;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_jobs.find(a_follower);
            if (it == g_jobs.end()) return {};
            if (it->second.finished) {
                Result r = it->second.done;
                g_jobs.erase(it);
                return r;
            }
            j = it->second;
        }
        const auto now = Clock::now();
        if (!j.requested) {   // the main-thread read of a cell stop has not landed
            if (now - j.began > kResolveTimeout) {
                Clear(a_follower);
                return { State::Failed, "the main-thread read of the destination did not land within 15 s" };
            }
            return { State::Pending, {} };
        }
        const auto pr = APMFBridge::ReadPlace(j.handle);
        switch (pr.state) {
            case APMF_API::kPlaceState_Queued:
                if (now - j.at > kAnswerTimeout) {
                    Clear(a_follower);
                    return { State::Failed, std::format("Harbinger's game thread did not answer the placement (handle {}) within 10 s: {}", j.handle, j.desc) };
                }
                return { State::Pending, {} };
            case APMF_API::kPlaceState_Placed: {
                Clear(a_follower);
                return { State::Placed, std::format("{} -> read back in cell {:08X} at {:.0f},{:.0f},{:.0f}", j.desc, pr.cell, pr.pos.x, pr.pos.y, pr.pos.z) };
            }
            case APMF_API::kPlaceState_Refused:
            case APMF_API::kPlaceState_Failed: {
                Clear(a_follower);
                return { State::Failed, std::format("{} by Harbinger (why {}: {}; blocker handle {}): {}", pr.state == APMF_API::kPlaceState_Refused ? "REFUSED" : "FAILED",
                                                    pr.why, WhyText(pr.why), pr.blocker, j.desc) };
            }
            default:
                Clear(a_follower);
                return { State::Failed, std::format("Harbinger has no record of the placement (handle {}; a load or a new game dropped it): {}", j.handle, j.desc) };
        }
    }

    void Clear(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        g_jobs.erase(a_follower);
    }
    void ClearAll() {
        std::scoped_lock lk(g_mx);
        g_jobs.clear();
        ++g_gen;
    }
}
