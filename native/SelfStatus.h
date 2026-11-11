#pragma once
#include "PCH.h"
#include <vector>

// SELF STATUS READS that the gambit evaluator cannot make on its own thread (ClickUp 86e3g81x0, marth 2026-09-29).
//
// cond.self_poisoned needs the follower's ACTIVE EFFECT LIST, which mutates under the magic caster on the MAIN
// thread (cast/Summon.cpp's rule; logistics/ErrandRest.cpp PostMagicProbe walks it the same way). The Evaluator
// runs on the AddTask job worker, so it never walks the list: it asks Poisoned(), which answers from a small
// mirror and, when the mirror is older than kRefreshSec, posts ONE main-thread refresh (MainThread::Post). The
// answer is the last measured one. A follower with no fresh measurement (never read, or older than kTrustSec)
// reads NOT poisoned, the evaluator's fail-closed rule, so the first lap after a load or after a lost refresh is
// quiet, not wrong. On VR, where the pump is a no-op (MainThread::IsInstalled), there is NO read at all: the
// condition is never true there (a documented degrade, not a worker-side list walk). No co-save, no
// ResetAllState hook: the trust window ages a pre-load entry out by itself.
//
// POISONED = a live (not dispelled, not inactive) ActiveEffect whose source spell answers MagicItem::IsPoison()
// (vfunc 0x61: for an AlchemyItem the poison flag, flags bit 17; an alchemy poison, a poisoned weapon's hit).
// NOT GetSpellType(): AlchemyItem::GetSpellType (vfunc 0x53) always returns kAlchemy, never kPoison.
// Works for ANY actor (the key is the FormID): the follower himself (cond.self_poisoned) and an ally or the
// player (cond.ally_poisoned).
namespace MFO::SelfStatus {

    // True when the mirror says a_actor carries a live poison. Worker-safe (a mutex-guarded map read).
    [[nodiscard]] bool Poisoned(RE::Actor* a_actor);

    // CARD G5 (cond.self_has_summon / cond.self_no_summon) and G6 (cond.foe_attacking_my_summon): the LIVE actors
    // a_actor commands (conjured creatures and Reanimate thralls), measured on the MAIN thread by
    // Actuation::LiveCommandedActors in the SAME refresh as the poison read (one Post, one stamp, the same 1 s refresh
    // and 5 s trust). Worker-safe. With no fresh measurement both conditions read FALSE (fail closed: "no summon"
    // is never claimed on a stale or missing read either), and CommandedLive returns empty.
    [[nodiscard]] bool HasSummon(RE::Actor* a_actor);
    [[nodiscard]] bool NoSummon(RE::Actor* a_actor);
    // The FormIDs of those live commanded actors (empty = none, or no fresh measurement).
    [[nodiscard]] std::vector<RE::FormID> CommandedLive(RE::Actor* a_actor);

    // Drop the whole mirror. ResetAllState (load / revert), after StopPump, beside MainThread::Clear: a pre-load
    // answer must not outlive the load (handles and FormIDs are re-resolved against the next session).
    void Clear();

}
