#pragma once
#include "PCH.h"

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
// POISONED = a live (not dispelled, not inactive) ActiveEffect whose source spell is a POISON (SpellType kPoison:
// an alchemy poison, a poisoned weapon's hit). That is the set the engine's own cure-poison effect removes: the fork's
// CureEffect carries the spellType it cures (RE/C/CureEffect.h:22).
namespace MFO::SelfStatus {

    // True when the mirror says a_actor carries a live poison. Worker-safe (a mutex-guarded map read).
    [[nodiscard]] bool Poisoned(RE::Actor* a_actor);

}
