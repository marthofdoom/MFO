// classes/StealthEntry.cpp -- card C13: the Thief's FREE STEALTH ENTRY (batch I item 2, T2).
//
// marth (class walkthrough): "a free stealth entry every 5 fights, and decide based on stats for the other 4" (Thief; Master Thief 1 in
// 3). One fight in five (three) opens guaranteed undetected; on the others his sneak rolls against the foes' detection, and that roll is
// the ENGINE's own (G2 Layer 1's verdict), never a result MFO makes up.
//
// THE COUNTER is the one the MCLS record already carries (FollowerClasses::thiefFights / masterThiefFights, "fights since the last
// guaranteed entry", Classes.h): no co-save field and no layout change here. A new fight (the IAUS gather's fight clock starts a new one)
// counts one; the fight that would be the Nth is the GUARANTEED one and zeroes the counter.
//
// WHAT "GUARANTEED UNDETECTED" MEANS HERE (G1 + G2, no new lever): on a guaranteed fight the IAUS reads a foe the engine has no verdict
// on yet (its group has no record of him: Hide::Verdict Unknown, the first laps of a fight) as UNAWARE, which is what lets G2's hold row
// stand and a blow from stealth be struck from the first lap, where on any other fight Unknown means "not known" and passes nothing. It
// is a read of the same input, not a result: the moment ANY foe's group record says it has him (verdict Yes) the entry is SPENT and
// every foe is read by the engine alone again. It is also spent by his first blow and by the fight's end.
//
// Threads: every entry point runs on the pump's AddTask worker (the IAUS gather, IausSource::Record, the combat-end teardown); the
// map has its own leaf mutex, no engine call and no other lock is taken under it. Runtime-only; nothing is saved here (the counter is
// MCLS, written through SetStealthFights, which refuses when the class save is read-only: the entry then still applies this fight).
#include "Classes_internal.h"

#include <mutex>
#include <unordered_map>

namespace MFO::Classes {

    namespace {
        std::mutex g_mx;
        struct Fight {
            bool guaranteed = false;
            bool spent = false;
        };
        std::unordered_map<RE::FormID, Fight> g_fights;

        // Fights per guaranteed entry (marth): Thief 5, Master Thief 3. 0 = this class has none.
        int EveryFor(std::uint8_t a_equipped) {
            if (a_equipped == static_cast<std::uint8_t>(Id::Thief)) return 5;
            if (a_equipped == static_cast<std::uint8_t>(Id::MasterThief)) return 3;
            return 0;
        }
    }

    bool StealthEntryLive(RE::FormID a_follower, bool a_newFight) {
        if (a_follower == 0) return false;
        const std::uint8_t eq = Active() ? GetEquipped(a_follower) : kNoClass;
        const int every = EveryFor(eq);
        if (every == 0) {
            std::scoped_lock lk(g_mx);
            g_fights.erase(a_follower);   // not a Thief (any more): no entry, nothing to remember
            return false;
        }
        if (a_newFight) {
            auto [thief, master] = GetStealthFights(a_follower);
            std::uint16_t& cnt = eq == static_cast<std::uint8_t>(Id::Thief) ? thief : master;
            const bool guaranteed = static_cast<int>(cnt) + 1 >= every;
            cnt = guaranteed ? std::uint16_t{ 0 } : static_cast<std::uint16_t>(cnt + 1);
            const bool saved = SetStealthFights(a_follower, thief, master);
            {
                std::scoped_lock lk(g_mx);
                g_fights[a_follower] = Fight{ guaranteed, false };
            }
            spdlog::info("[stealth-entry] {:08X}: a new fight, the {} counter is now {} of {}{}{}", a_follower,
                         eq == static_cast<std::uint8_t>(Id::Thief) ? "Thief" : "Master Thief", cnt, every,
                         guaranteed ? ": this is the GUARANTEED stealth-entry fight (he starts hidden through G1 + G2 until a foe's group "
                                      "record has him, his first blow, or the fight's end)"
                                    : ": an ordinary fight (his sneak rolls against the engine's own detection)",
                         saved ? "" : " (the class save is read-only: the counter is not kept)");
        }
        std::scoped_lock lk(g_mx);
        const auto it = g_fights.find(a_follower);
        return it != g_fights.end() && it->second.guaranteed && !it->second.spent;
    }

    void StealthEntrySpend(RE::FormID a_follower, const char* a_why) {
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_fights.find(a_follower);
            if (it == g_fights.end() || !it->second.guaranteed || it->second.spent) return;
            it->second.spent = true;
        }
        spdlog::info("[stealth-entry] {:08X}: the free entry is SPENT: {}", a_follower, a_why ? a_why : "");
    }

    void StealthEntryForget(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        g_fights.erase(a_follower);
    }

    void StealthEntryClear() {
        std::scoped_lock lk(g_mx);
        g_fights.clear();
    }
}
