#pragma once
// iaus/MovePass.h -- THE MOVEMENT PASS: one shared decision of who stands where, once per window (S1, iaus-sim-v4.md 8.7). PURE, no caller in the DLL yet.
//
//   window start   Freeze        positions, threats, marks. The picture's inputs do not change until the window ends, so the result cannot depend on slicing.
//   across ticks   Phase1        spots, paths, the ray asks. Resumable: false = call again. Asks appear once, when it returns true.
//   as they come   Answer        the engine's sight answers, of this window or of an earlier one (up to spotFactMaxAgeSec late: PartyPicture::late). An answer
//                                later than its window should carry RayAnswer::t and the viewer's place; without them it counts from its first filing
//   window end     Phase3        score and assign. Adopt writes the commits. Orders turns them into what the mover is asked for (MoveOrder.h).
//   A window belongs to ONE view. Phase1 called with another stamp drops the window's work (counts.viewChanged): the uncommitted stay.
//
// CADENCE: the holder starts a window every MoveDecisionSec(tuning), never under 0.5 s. An event sets Member::marks, it never starts a window.
// DETERMINISM: everything is ordered by FormID, spot key and triangle, scores and distances are compared as integers (Quant), no libm beyond sqrt here.
#include "iaus/Spots.h"

#include <span>

namespace MFO::Iaus::MovePass {

    using namespace MFO::Iaus::Move;
    using ClassProfiles = const Profile& (*)(std::uint8_t a_classId);   // ClassCurves::ProfileFor in the game

    void Freeze(PartyPicture& a_pic, const Env::View& a_v, const Tuning& a_t);
    // a_sliceMs <= 0: exactly one unit of work a call. +inf: all of it.
    bool Phase1(PartyPicture& a_pic, const Env::View& a_v, Env::Scratch& a_s, const ClassProfiles& a_profiles, const Tuning& a_t, std::vector<RayAsk>& a_asks, float a_sliceMs);
    void Answer(PartyPicture& a_pic, std::span<const RayAnswer> a_answers, const Tuning& a_t);
    void File(PartyPicture& a_pic);   // the asks of the window that ended become carried and late. Freeze calls it (a dump writer too, on its copy)
    void Phase3(PartyPicture& a_pic, const ClassProfiles& a_profiles, const Tuning& a_t, std::vector<Assignment>& a_out);   // one row per deciding member, by FormID. Writes only counts
    void Adopt(PartyPicture& a_pic, std::span<const Assignment> a_as);   // the commits of the next window. Clears the marks THIS window saw, never the inbox

    void Orders(PartyPicture& a_pic, std::span<const Assignment> a_as, const Tuning& a_t);   // Member::order of everybody, after Adopt. Free for all without RoadCaps::strict

    [[nodiscard]] std::int64_t Quant(float a_score);   // the integer a score is compared as
}
