// classes/Xp.cpp -- class XP (card C4): fights won while a class is equipped, plus heal and summon credit.
//
// WHAT IS CREDITED. A follower collects PENDING credit during a fight (a kill by him or his commanded summon, an assist on a
// party kill he was fighting beside, an observed landed heal), and the pending credit is turned into XP ONCE, at the fight's
// end, if the fight was WON: a hostile was killed by the party and the party was not wiped. The XP goes to his EQUIPPED class
// only (State.cpp AwardXp, one compound g_mx write). Nothing here changes the co-save layout: MCLS already stores xp + level.
//
// THE CURVE (marth 2026-11-10: pace "~3 h / 6 h / 10 h"; the FE research keeps the Master tier at "12 to 14 h" -> 13 h):
//   hours to master  T1 3, T2 6, T3 10, T4 13
//   fights an hour   10    (Docs/BALANCE.md: ~10 distinct fights an hour)
//   points a fight   ~100  (participation 60 + about two kills at 20; the weights below are sized so a typical fight sums to it)
//   XpToMaster       = hours x 10 x 100  ->  3000 / 6000 / 10000 / 13000
//   threshold(L)     = XpToMaster x ((L - 1) / (cap - 1))^1.3, L = 1..cap  (level 1 = 0 points; the cap's threshold = XpToMaster)
// The exponent 1.3 makes each level a little dearer than the one before, so a level means more play time higher up the line.
// fClassXpRate (MCM) multiplies every award, so 1.0 is the sized pace and 2.0 halves the hours.
//
// THREADING. Pending credit lives in g_pending under g_mx (a LEAF mutex: nothing is called while it is held). Writers:
//   XpOnDeath        job worker, inside Rapport's gated death task (PumpTickGate held by the caller)
//   XpNoteHeal       main thread (the HealObs verdict closure)
//   XpOnPartyFightEnd job worker (the Scheduler's party-out-of-combat teardown), plus one MainThread::Post for the summon read
// AwardXp is thread-safe by itself (State.cpp). No engine write is made anywhere in this file; reads only.
#include "Classes_internal.h"
#include "Config.h"
#include "Followers.h"
#include "MainThread.h"
#include "cast/Actuation.h"   // LiveCommandedActors (cast/Summon.cpp): the summon-uptime read
#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace MFO::Classes {

    namespace {
        using Clock = std::chrono::steady_clock;

        // ── the sizing (see the header comment) ─────────────────────────────────────────────────────────────────
        constexpr float kHoursToMaster[4] = { 3.0f, 6.0f, 10.0f, 13.0f };   // tier 1..4
        constexpr float kFightsPerHour    = 10.0f;
        constexpr float kPointsPerFight   = 100.0f;
        constexpr float kCurveExponent    = 1.3f;

        // ── the per-fight weights (points before the rate) ──────────────────────────────────────────────────────
        constexpr std::uint32_t kPtsParticipation = 60;   // the main source: the fight was won and he took part (R2 of the FE research)
        constexpr std::uint32_t kPtsKill          = 20;   // a kill by him or his commanded summon
        constexpr std::uint32_t kPtsAssist        = 8;    // a party kill he was fighting beside (combat within the shared grace)
        constexpr std::uint32_t kPtsHeal          = 6;    // a landed heal on someone else ([heal-obs] verdict, one per 3 s per pair)
        constexpr std::uint32_t kPtsSelfHeal      = 2;    // a landed heal on himself
        constexpr int           kMaxHealsCounted  = 10;
        constexpr int           kMaxSelfHealsCounted = 5;
        constexpr std::uint32_t kPtsSummonPer3s   = 1;    // a summon still up at the fight's end: one point per 3 s of the fight ...
        constexpr float         kSummonSecCap     = 120.0f;   // ... counted up to 2 minutes (40 points)
        constexpr std::uint32_t kPtsFightCap      = 400;  // one fight never pays more than this (a runaway-credit bound)
        constexpr float         kWonLookbackSec   = 120.0f;   // a heal-only follower shares a win the party scored this long before his first credit
        constexpr double        kSummonReadWaitSec = 3.0;     // give the main-thread summon read this long, then settle without it

        struct Pending {
            Clock::time_point firstAt{};        // his first credit of this fight
            int  kills = 0, assists = 0, heals = 0, selfHeals = 0;
            bool summonPosted = false, summonResolved = false, summonUp = false;
            Clock::time_point summonPostedAt{};
        };
        std::mutex                                g_mx;
        std::unordered_map<RE::FormID, Pending>   g_pending;
        std::unordered_map<RE::FormID, Clock::time_point> g_lastLog;   // the award line's rate limit
        Clock::time_point                         g_lastPartyKill{};   // when the party last killed a hostile (any follower)
        constexpr double kXpLogEverySec = 2.0;   // one [classes] XP line per follower per this long (awards are once per fight anyway)

        double Since(Clock::time_point a_t, Clock::time_point a_now) { return std::chrono::duration<double>(a_now - a_t).count(); }

        bool HasClass(RE::FormID a_fid) {
            const auto c = GetEquipped(a_fid);
            return c != kNoClass && LevelCap(static_cast<Id>(c)) != 0;
        }

        // Caller holds g_mx.
        Pending& Touch(RE::FormID a_fid, Clock::time_point a_now) {
            auto& p = g_pending[a_fid];
            if (p.firstAt.time_since_epoch().count() == 0) p.firstAt = a_now;
            return p;
        }

        struct Curves {
            std::array<std::array<std::uint32_t, 25>, 4> t{};   // per tier 1..4, the first LevelCap(tier) entries are used
            Curves() {
                for (int tier = 1; tier <= 4; ++tier) {
                    const int cap = tier == 1 ? 10 : tier == 2 ? 15 : tier == 3 ? 20 : 25;   // = LevelCap by tier (Mastery.cpp)
                    const float total = kHoursToMaster[tier - 1] * kFightsPerHour * kPointsPerFight;
                    for (int l = 1; l <= cap; ++l) {
                        const float frac = static_cast<float>(l - 1) / static_cast<float>(cap - 1);
                        t[static_cast<std::size_t>(tier - 1)][static_cast<std::size_t>(l - 1)] =
                            static_cast<std::uint32_t>(std::lround(total * std::pow(frac, kCurveExponent)));
                    }
                }
            }
        };
        const Curves& GetCurves() { static const Curves c; return c; }
    }

    std::span<const std::uint32_t> XpCurve(Id a_class) {
        const auto cap = LevelCap(a_class);
        if (cap == 0) return {};
        const int tier = kTable[static_cast<std::size_t>(a_class)].tier;
        return std::span<const std::uint32_t>(GetCurves().t[static_cast<std::size_t>(tier - 1)].data(), cap);
    }

    std::uint32_t XpToMaster(Id a_class) {
        const auto c = XpCurve(a_class);
        return c.empty() ? 0u : c.back();
    }

    void detail::XpReset() {
        std::scoped_lock lk(g_mx);
        g_pending.clear();
        g_lastLog.clear();
        g_lastPartyKill = {};
    }

    // WORKER, inside Rapport's gated death task. Who gets credit: the follower who killed (or whose commanded summon killed),
    // and every other follower who was in combat within the shared grace (an assist). The PLAYER'S kill and the player's summons'
    // kills count as a party kill for the win test, and credit the followers beside him as assists. A kill by anyone else is a
    // world death and is ignored; so is a victim that is not hostile to the player.
    void XpOnDeath(RE::Actor* a_victim, RE::Actor* a_killer) {
        if (!Active() || !a_victim || !a_killer) return;
        if (a_victim->IsPlayerRef() || a_victim->IsPlayerTeammate()) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !a_victim->IsHostileToActor(player)) return;

        RE::Actor* credited = a_killer;
        if (credited->IsCommandedActor()) {
            if (auto cmd = credited->GetCommandingActor(); cmd) credited = cmd.get();   // a summon's kill is its master's
        }
        const bool killerIsPlayer = credited->IsPlayerRef();
        const RE::FormID killerFid = credited->GetFormID();
        const bool killerIsOurs = !killerIsPlayer && Followers::IsTracked(killerFid);
        if (!killerIsPlayer && !killerIsOurs) return;   // a world death

        const auto snap = Followers::ActiveSnapshot();
        if (!snap) return;
        const float grace = Config::g_sharedCombatGrace.load();
        const auto now = Clock::now();
        int kills = 0, assists = 0;
        std::scoped_lock lk(g_mx);
        g_lastPartyKill = now;
        for (const RE::FormID fid : *snap) {
            if (fid == 0 || !HasClass(fid)) continue;
            if (killerIsOurs && fid == killerFid) {
                ++Touch(fid, now).kills;
                ++kills;
            } else if (Followers::SecondsSinceCombat(fid) <= grace) {
                ++Touch(fid, now).assists;
                ++assists;
            }
        }
        spdlog::debug("[classes] XP feed: hostile {:08X} died to {:08X} -- {} kill credit(s), {} assist(s)",
                      a_victim->GetFormID(), killerFid, kills, assists);
    }

    // MAIN thread, the HealObs verdict (a heal that RAISED the recipient's health).
    void XpNoteHeal(RE::FormID a_caster, RE::FormID a_target) {
        if (!Active() || a_caster == 0 || !HasClass(a_caster)) return;
        const auto now = Clock::now();
        std::scoped_lock lk(g_mx);
        auto& p = Touch(a_caster, now);
        if (a_caster == a_target) ++p.selfHeals; else ++p.heals;
    }

    // WORKER. Called on EVERY party-out-of-combat service of a follower (the teardown block is idempotent), so this acts on the
    // first call after a fight that left pending credit and is a map miss after that.
    void XpOnPartyFightEnd(RE::FormID a_follower) {
        Pending p;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_pending.find(a_follower);
            if (it == g_pending.end()) return;
            const auto now = Clock::now();
            auto& q = it->second;
            if (!q.summonResolved) {
                if (!q.summonPosted) {
                    // Is a summon of his still up? A read of the process's commandedActors, which is MAIN-thread data (cast/Summon.cpp
                    // LiveCommandedActors). The verdict lands in the pending record; the award waits for it (a few services).
                    q.summonPosted = true;
                    q.summonPostedAt = now;
                    const RE::FormID fid = a_follower;
                    MainThread::Post([fid]() {
                        bool up = false;
                        if (auto* f = RE::TESForm::LookupByID<RE::Actor>(fid); f && f->Is3DLoaded())
                            up = !Actuation::LiveCommandedActors(f).empty();
                        std::scoped_lock lk2(g_mx);
                        if (const auto jt = g_pending.find(fid); jt != g_pending.end()) {
                            jt->second.summonUp = up;
                            jt->second.summonResolved = true;
                        }
                    });
                    return;
                }
                if (Since(q.summonPostedAt, now) < kSummonReadWaitSec) return;
                q.summonResolved = true;   // the read never came back (a load dropped it): settle without summon credit
            }
            p = q;
            g_pending.erase(it);
        }

        const auto now = Clock::now();
        const auto equipped = GetEquipped(a_follower);
        if (equipped == kNoClass || static_cast<std::size_t>(equipped) >= kClassCount) return;   // he left his class since
        const Id cls = static_cast<Id>(equipped);
        if (LevelCap(cls) == 0) return;   // Dragonborn earns no class XP

        bool won = (p.kills + p.assists) > 0;
        if (!won) {
            std::scoped_lock lk(g_mx);
            won = g_lastPartyKill.time_since_epoch().count() != 0 &&
                  g_lastPartyKill >= p.firstAt - std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(kWonLookbackSec));
        }
        // NOT WIPED: the player alive and at least one follower alive. A reload follows a wipe, so this mostly guards the
        // edge where the player went down as the last foe did.
        bool wiped = true;
        if (auto* pl = RE::PlayerCharacter::GetSingleton(); pl && !pl->IsDead()) wiped = false;
        if (wiped) { spdlog::info("[classes] XP {:08X}: fight over with the party down -- no award", a_follower); return; }
        if (!won) {
            spdlog::debug("[classes] XP {:08X}: credit pending ({} heal(s)) but no hostile fell to the party -- no award", a_follower, p.heals);
            return;
        }

        std::uint32_t pts = kPtsParticipation + kPtsKill * static_cast<std::uint32_t>(p.kills) + kPtsAssist * static_cast<std::uint32_t>(p.assists)
                          + kPtsHeal * static_cast<std::uint32_t>(std::min(p.heals, kMaxHealsCounted))
                          + kPtsSelfHeal * static_cast<std::uint32_t>(std::min(p.selfHeals, kMaxSelfHealsCounted));
        std::uint32_t summonPts = 0;
        if (p.summonUp) {
            const float fightSec = std::min(static_cast<float>(Since(p.firstAt, now)) + 20.0f, kSummonSecCap);   // the credit window opens 20 s before his first credit
            summonPts = kPtsSummonPer3s * static_cast<std::uint32_t>(fightSec / 3.0f);
            pts += summonPts;
        }
        pts = std::min(pts, kPtsFightCap);
        const float rate = Config::g_classXpRate.load();
        if (rate <= 0.0f) return;
        const std::uint32_t award = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(static_cast<float>(pts) * rate)));

        const auto r = AwardXp(a_follower, cls, award, XpCurve(cls));
        if (!r.applied) {
            spdlog::debug("[classes] XP {:08X}: {} points for {} refused (not Writable, not equipped, or already mastered)",
                          a_follower, award, Get(cls).slug);
            return;
        }
        bool logIt = false;
        {
            std::scoped_lock lk(g_mx);
            auto& last = g_lastLog[a_follower];
            if (last.time_since_epoch().count() == 0 || Since(last, now) >= kXpLogEverySec) { last = now; logIt = true; }
        }
        if (logIt || r.levelAfter != r.levelBefore)
            spdlog::info("[classes] XP {:08X} {}: +{} (fight won: {} kill, {} assist, {} heal, {} self-heal, summon {}; base {} x rate {:.2f}) -> {} / {} "
                         "level {}{}", a_follower, Get(cls).slug, award, p.kills, p.assists, p.heals, p.selfHeals, summonPts, pts, rate, r.xp,
                         XpToMaster(cls), static_cast<int>(r.levelAfter),
                         r.levelAfter != r.levelBefore ? std::format(" (LEVEL UP from {})", static_cast<int>(r.levelBefore)) : std::string{});
    }
}
