#include "PCH.h"
#include "CombatStyle.h"
#include "Config.h"
#include "Forms.h"

namespace MFO::CombatStyle {

    namespace {

        // combatStyle sits at 0x38, BELOW the 0x68 AE layout divergence (the
        // pinned NG header guards its AE-only BSSpinLock on a macro NG never
        // defines, so everything past 0x68 is +8 at runtime -- §0.29). 0x38 is
        // identical on SE and AE. These asserts break the build the day that
        // stops being true; the write below is a single aligned pointer store,
        // atomic on x64, through the live controller the engine just handed us.
        static_assert(offsetof(RE::CombatController, combatStyle) == 0x38,
                      "CombatController::combatStyle moved -- re-verify against the "
                      "pinned header (ENGINE_NOTES §0.29) before shipping");
        static_assert(offsetof(RE::CombatController, combatStyle) < 0x68,
                      "combatStyle is past the AE layout divergence point (0x68) "
                      "-- its compiled offset is WRONG on AE runtimes");

        struct Owned {
            Stance                stance   = Stance::None;   // what the gambit wants
            Stance                applied  = Stance::None;   // what we last wrote (handoff detect)
            RE::CombatController*  cc       = nullptr;        // identity ONLY -- never dereferenced
            RE::TESCombatStyle*    saved    = nullptr;        // engine-derived style this fight
            std::uint32_t          rederives = 0;             // engine re-derived under us, times
        };

        // Written by Want/Clear (worker) AND ApplyTick (combat thread), so a
        // real lock -- a plain mutex, taken only when AnyActive() (the atomic
        // mirror) already says a stance is live, so a non-combat / no-stance
        // tick never touches it.
        std::mutex g_mx;
        std::unordered_map<RE::FormID, Owned> g_owned;
        std::atomic<std::size_t> g_count{ 0 };

        RE::TESCombatStyle* StyleFor(Stance a_stance) {
            switch (a_stance) {
                case Stance::Melee:  return Forms::g_meleeStyle;
                case Stance::Ranged: return Forms::g_rangedStyle;
                default:             return nullptr;
            }
        }

        const char* Name(Stance a_stance) {
            switch (a_stance) {
                case Stance::Melee:  return "melee";
                case Stance::Ranged: return "ranged";
                default:             return "none";
            }
        }

    }

    bool        AnyActive()  { return g_count.load(std::memory_order_relaxed) != 0; }
    std::size_t OwnedCount() { return g_count.load(std::memory_order_relaxed); }

    void Want(RE::FormID a_follower, Stance a_stance) {
        if (a_stance == Stance::None) return;
        if (!Config::g_weaponStyleControl.load(std::memory_order_relaxed)) return;
        std::lock_guard<std::mutex> lk(g_mx);
        // Only the desire is set here; cc/saved are filled in on the combat
        // thread by ApplyTick against the live controller. A stance CHANGE on an
        // existing entry (melee<->ranged) keeps cc/saved so the next ApplyTick
        // is a handoff, not a re-baseline.
        auto& o = g_owned[a_follower];
        o.stance = a_stance;
        g_count.store(g_owned.size(), std::memory_order_relaxed);
    }

    void Clear(RE::FormID a_follower) {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_owned.erase(a_follower) != 0)
            g_count.store(g_owned.size(), std::memory_order_relaxed);
    }

    void ClearAll() {
        std::lock_guard<std::mutex> lk(g_mx);
        g_owned.clear();
        g_count.store(0, std::memory_order_relaxed);
    }

    void ApplyTick(RE::Actor* a_actor, RE::CombatController* a_cc) {
        if (!a_actor || !a_cc) return;

        // The P1 cast-style probe (CasterConsent, bProbeCastStyle -- dev-only,
        // default OFF) also writes cc->combatStyle, from the caster thunk. While
        // it is armed, defer entirely: two features re-asserting DIFFERENT CSTYs
        // onto the same field every tick would ping-pong and spam both logs. The
        // dev running that experiment wants the probe's style, not ours. In the
        // shipping default (probe off) this branch is never taken.
        if (Config::g_probeCastStyle.load(std::memory_order_relaxed)) return;

        const auto fid = a_actor->GetFormID();

        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_owned.find(fid);
        if (it == g_owned.end()) return;
        auto& o = it->second;

        RE::TESCombatStyle* target = StyleFor(o.stance);
        if (!target) return;   // form missing -> stance disabled, silent per the "one feature" rule

        // A DIFFERENT controller than we last swapped means the fight that swap
        // lived in is over -- its controller (and our swap) died with it, and
        // the engine re-derived this fresh one from the base record. Re-baseline
        // against the LIVE controller: capture its engine-derived style as the
        // restore point, then own it. Identity compare only; the stored pointer
        // is NEVER dereferenced.
        if (o.cc != a_cc) {
            o.cc        = a_cc;
            o.saved     = a_cc->combatStyle;
            o.rederives = 0;
            o.applied   = o.stance;
            a_cc->combatStyle = target;
            spdlog::info("[wstyle] {:08X} {}: combat style OWNED {:08X} -> {:08X} ({} stance)",
                         fid, a_actor->GetName() ? a_actor->GetName() : "?",
                         o.saved ? o.saved->GetFormID() : 0,
                         target->GetFormID(), Name(o.stance));
            return;
        }

        // Same fight. Two reasons the live style can differ from our target:
        //  * a melee<->ranged HANDOFF (the winning gambit flipped) -- expected,
        //    log it once at the transition;
        //  * the engine RE-DERIVED the style under us mid-combat -- re-assert so
        //    the follower stays in the gambit's stance (the cast probe proved
        //    the engine does this; count it).
        if (o.applied != o.stance) {
            a_cc->combatStyle = target;
            o.applied = o.stance;
            spdlog::info("[wstyle] {:08X}: stance HANDOFF -> {:08X} ({})",
                         fid, target->GetFormID(), Name(o.stance));
        } else if (a_cc->combatStyle != target) {
            // The engine re-derived the base style under us mid-combat (the cast
            // probe proved it does). Re-assert EVERY tick, but LOG ONLY THE
            // FIRST -- this is a default-ON feature and a persistent re-derive
            // would otherwise write one MFO.log line per combat tick. The first
            // line proves it happens; the count rides the state report.
            ++o.rederives;
            a_cc->combatStyle = target;
            if (o.rederives == 1)
                spdlog::info("[wstyle] {:08X}: engine RE-DERIVED the style under the swap "
                             "-- re-asserting {} ({}) each tick (silenced after first)",
                             fid, target->GetFormID(), Name(o.stance));
        }
    }

}
