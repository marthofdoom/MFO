// scheduler/IausSource.cpp -- the IAUS ChoiceSource and its lifecycle (contract in scheduler/IausSource.h).
// IAUS card I6. Runs on the AddTask job worker inside Scheduler::Tick (#4). LandingCrew's idea (iaus/Core.h).
#include "PCH.h"
#include "scheduler/IausSource.h"
#include "scheduler/Scheduler_internal.h"   // g_recent, g_proposedTarget, g_retreatHealLog (the scan's own maps)

#include "CasterConsent.h"
#include "ComposedCast.h"
#include "Config.h"
#include "Followers.h"
#include "Loadout.h"
#include "Targeting.h"
#include "Vocabulary.h"
#include "apmf/APMFBridge.h"
#include "cast/Actuation.h"
#include "iaus/SpellBook.h"

#include <algorithm>
#include <format>

namespace MFO::Scheduler {

    namespace {

        using Iaus::Family;
        using Iaus::PostureKind;

        // The class profile is card I8's. Until it lands every follower scores with the uncustomised catalogue.
        const Iaus::Profile& DefaultProfile() {
            static const Iaus::Profile p;
            return p;
        }

        // Log channels for Engine::Paced.
        constexpr std::uint8_t kChanDescribe = 0;   // [iaus-gen]: what was generated
        constexpr std::uint8_t kChanNoBook   = 1;   // [iaus] no fresh spell book
        constexpr std::uint8_t kChanStaff    = 2;   // [iaus] staff posture has no equip road
        constexpr double       kDescribeSec  = 10.0;

        [[nodiscard]] bool IsCastFamily(Family a_f) {
            // The families that hold a hand and a claim. A summon is a one-shot (the scan's own isCast excludes it).
            return a_f == Family::CastOffense || a_f == Family::CastControl || a_f == Family::CastHeal ||
                   a_f == Family::CastBuff || a_f == Family::CastWard;
        }

        [[nodiscard]] RE::ActorHandle HandleOf(RE::FormID a_actor) {
            auto* a = a_actor ? RE::TESForm::LookupByID<RE::Actor>(a_actor) : nullptr;
            return a ? a->GetHandle() : RE::ActorHandle{};
        }

        // PASSIVE PROBE (design fact 8, principle 5): the player's currentCombatTarget has never been shown to be
        // POPULATED in a fight. IAUS reads it (player_target, the player-focus relevance), so count what the
        // gather saw and say so every 10 s while any IAUS follower is being served. Worker-only counters; a
        // count that straddles a load is one mixed line, nothing else reads them.
        struct PlayerTargetProbe {
            std::uint32_t set = 0, unset = 0;
            double        lastLog = -1.0e300;
            RE::FormID    lastTarget = 0;
        } g_ptProbe;

        void ProbePlayerTarget(const Iaus::Snapshot& a_snap, double a_now) {
            if (a_snap.playerTargetId != 0) { ++g_ptProbe.set; g_ptProbe.lastTarget = a_snap.playerTargetId; }
            else ++g_ptProbe.unset;
            if (a_now - g_ptProbe.lastLog < 10.0) return;
            g_ptProbe.lastLog = a_now;
            auto* pc = RE::PlayerCharacter::GetSingleton();
            std::string who = "none";
            if (g_ptProbe.lastTarget != 0) {
                auto* t = RE::TESForm::LookupByID<RE::Actor>(g_ptProbe.lastTarget);
                who = std::format("{} ({:08X})", t && t->GetName() ? t->GetName() : "?", g_ptProbe.lastTarget);
            }
            spdlog::info("[iaus-probe] player currentCombatTarget: set on {} of {} IAUS laps since the last line (last "
                         "seen: {}); player IsInCombat={}. If it stays 0 in a real fight the player_target "
                         "consideration and that half of the player focus have no input.",
                         g_ptProbe.set, g_ptProbe.set + g_ptProbe.unset, who, pc && pc->IsInCombat() ? 1 : 0);
            g_ptProbe.set = g_ptProbe.unset = 0;
            g_ptProbe.lastTarget = 0;
        }

        const char* PostureOpcode(PostureKind a_k) {
            switch (a_k) {
            case PostureKind::Melee:  return Vocab::kActEquipMelee;
            case PostureKind::Bow:    return Vocab::kActEquipRanged;
            case PostureKind::Silver: return Vocab::kActEquipSilver;
            case PostureKind::Staff:  // no act.equip_staff exists (Actuation::EquipWeapon's ranged category is bow / crossbow)
            case PostureKind::None:   break;
            }
            return nullptr;
        }

        // RELEASE WHAT A RUNNING DECISION HOLDS (the toggle edge). Every call is the existing, idempotent release
        // the combat-end teardown and the `!castSeen` lap already make; nothing here is a new engine road.
        void StandDown(RE::FormID a_id, const char* a_why) {
            // The cast: the spell in hand, the offense claim, the heal claim and its hold records, the [cfc] watch,
            // the hand locks (they carry the old source's RANK), the cast-control latch.
            Loadout::ReleaseSpell(a_id, /*a_includeRight=*/true);
            APMFBridge::ReleaseOffenseCast(a_id);
            Actuation::HealHandsCombatEnded(a_id);   // closes every heal-hold record (its line reads "combat-ended")
            ComposedCast::End(a_id);
            ComposedCast::ClearWatch(a_id);
            Actuation::ClearCastLock(a_id);
            CasterConsent::Clear(a_id);
            APMFBridge::ReleaseHealApproach(a_id, a_why);
            APMFBridge::ReleaseOffenseApproach(a_id, a_why);
            // The WEAPON stays in his hand, but its hold's RANK is the old source's (review F1): a table row index
            // would outrank every IAUS heal on the left hand. Unranked until the new source's equip stamps its own.
            Actuation::UnrankForcedHold(a_id);
            // The attack: the commanded target and its pending proposal. The new source names its own.
            Targeting::Clear(a_id);
            g_proposedTarget.erase(a_id);
            // The table's positional window / the fail dedupe: an anchor from the old source means nothing now.
            g_recent.erase(a_id);
            // The IAUS memory: the running decision, the G10 want list, the fight clock.
            Iaus::Engine::Forget(a_id);
            Iaus::Candidates::Forget(a_id);
        }

    }

    // ── The source ───────────────────────────────────────────────────────────────────────────────────────────

    IausSource::IausSource(RE::Actor* a_self, RE::FormID a_id, std::uint8_t a_rapportRank)
        : m_self(a_self), m_id(a_id), m_tick(Iaus::Engine::MakeTick(a_id, a_rapportRank)) {
        const Iaus::Tuning& T = Iaus::DefaultTuning();
        // THE CANDIDATE CYCLE, in this order: Build, FillCooldowns, Decide, DecidePosture, NoteDecision. m_set is a
        // member and is never moved while the candidates (string_views into its snapshot) are in use.
        m_built = Iaus::Candidates::Build(a_self, m_set, T);
        if (!m_built) return;   // dead, disabled or unloaded: no candidates, Next reports "no more"
        auto& acts = m_set.gen.actions;
        Iaus::Engine::FillCooldowns(a_id, acts, m_tick, T);
        m_d = Iaus::Engine::Decide(a_id, acts, DefaultProfile(), T, m_tick, &m_ms);
        // The posture layer needs a fresh book (what he carries) and a readable foe to measure against. Without
        // either it has NO ANSWER, which the reconciliation reads as unknown, never as "no equip order wanted".
        m_postureKnown = m_set.snap.hasBook && !m_set.gen.selfUnreadable && m_set.gen.foesSeen > 0;
        if (m_postureKnown)
            m_posture = Iaus::DecidePosture(Iaus::Engine::MindFor(a_id), m_set.gen.postures, DefaultProfile(), T,
                                            a_rapportRank);
        Iaus::Candidates::NoteDecision(a_id, m_d, acts);

        // The walk: the posture winner first, then the action order.
        if (m_posture >= 0 && static_cast<std::size_t>(m_posture) < m_set.gen.postures.size()) {
            const PostureKind k = m_set.gen.postures[static_cast<std::size_t>(m_posture)].f.posture;
            if (PostureOpcode(k)) {
                m_walk.push_back({ true, static_cast<std::size_t>(m_posture) });
            } else if (k == PostureKind::Staff && !m_set.gen.postures[static_cast<std::size_t>(m_posture)].f.postureCurrent &&
                       Iaus::Engine::Paced(a_id, kChanStaff, 30.0, m_tick)) {
                // NOT MASKED: the posture layer wants the staff and MFO has no staff equip road. No equip order is
                // issued, so the reconciliation releases any melee / bow hold and his own AI may draw it.
                spdlog::info("[iaus] {:08X}: posture wants the STAFF, and no act.equip_staff road exists yet -- no equip "
                             "order this lap (his own AI may draw it)", a_id);
            }
        }
        for (std::size_t i = 0; i < m_d.order.size(); ++i) m_walk.push_back({ false, i });

        ProbePlayerTarget(m_set.snap, m_tick.in.now);
        if (!m_set.snap.hasBook && Iaus::Engine::Paced(a_id, kChanNoBook, 10.0, m_tick))
            spdlog::info("[iaus] {:08X}: no fresh spell book this lap -- attack candidates only (no cast, drink or "
                         "posture; the first lap of a fight, a load, or VR)", a_id);
    }

    IausSource::~IausSource() {
        if (!m_built) return;
        const Iaus::Tuning& T = Iaus::DefaultTuning();
        const char* name = m_self && m_self->GetName() ? m_self->GetName() : "?";
        // The [iaus] line (on change, plus every logRepeatSec; a NEW fall-past chain always). bIausExplain is card
        // I7's MCM key (Config is outside this card), so the [iaus-why] lines stay off here.
        Iaus::Engine::Log(m_id, name, m_d, m_set.gen.actions, DefaultProfile(), T, m_tick, m_pickedStep, m_notes, m_ms,
                          /*a_explain=*/false);
        // What was generated: the only view of the candidate set, rate-limited.
        if (Iaus::Engine::Paced(m_id, kChanDescribe, kDescribeSec, m_tick)) {
            std::string posture = "none wanted";
            if (!m_postureKnown) posture = "unknown (no book or no readable foe)";
            else if (m_posture >= 0)
                posture = std::format("{}{}", Iaus::Name(m_set.gen.postures[static_cast<std::size_t>(m_posture)].f.posture),
                                      m_postureNote.empty() ? "" : std::format(" ({})", m_postureNote));
            spdlog::info("[iaus-gen] {} ({:08X}) {} | posture {} | gate tree={} strict={} spells={} power={} taunt={} "
                         "retreat={} | taunt busy={} | precision len {:.0f} reach {:.0f}",
                         name, m_id, Iaus::Candidates::Describe(m_set), posture, m_set.gate.treeOn ? 1 : 0,
                         m_set.gate.strict ? 1 : 0, m_set.gate.spellsOnClass ? 1 : (m_set.gate.spellsFromMastered ? 2 : 0),
                         m_set.gate.powerAttack ? 1 : 0, m_set.gate.taunt ? 1 : 0, m_set.gate.disengage ? 1 : 0,
                         m_set.snap.tauntBusy ? 1 : 0, m_set.snap.book.kit.precisionWeaponLength, m_set.snap.meleeReach);
        }
        // The board's "Thinking" panel data (card I7 renders it).
        Iaus::Engine::Publish(m_id, Iaus::MakeReadout(m_d, m_set.gen.actions, Iaus::Engine::MindFor(m_id), T, m_tick.in.now));
    }

    bool IausSource::MakeChoice(const Item& a_item, Eval::Choice& a_out, std::string& a_whyNot) const {
        Eval::Choice c;
        c.source = Eval::Source::Iaus;
        if (a_item.posture) {
            const Iaus::Candidate& p = m_set.gen.postures[a_item.idx];
            c.actionOpcode = PostureOpcode(p.f.posture);
            // A stable identity that outranks NO action decision (every fired decision has epoch >= 1, so its rank
            // is below kRankBase): the equip never takes a hand from a standing heal or a cast in flight.
            c.rank = Iaus::kRankBase;
            a_out = std::move(c);
            return true;
        }
        const Iaus::Step& step = m_d.order[a_item.idx];
        const Iaus::Candidate& cand = m_set.gen.actions[step.idx];
        c.rank = step.rank;
        // An explicit target is ALWAYS a live handle (design 1.4). A target that no longer resolves is passed over
        // here, never handed to Fire: a cast-target choice without its handle would fall down Fire's ladder.
        if (cand.target != 0) {
            c.target = HandleOf(cand.target);
            if (!c.target.get()) { a_whyNot = "target no longer resolves"; return false; }
        }
        switch (cand.family) {
        case Family::Attack:       c.actionOpcode = Vocab::kActAttack; break;
        case Family::PowerAttack:  c.actionOpcode = Vocab::kActPowerAttack; break;
        case Family::Taunt:        c.actionOpcode = Vocab::kActTaunt; break;
        case Family::Retreat:      c.actionOpcode = Vocab::kActFlee; break;
        case Family::DrinkHealth:  c.actionOpcode = Vocab::kActDrinkHealthPotion; break;
        case Family::DrinkStamina: c.actionOpcode = Vocab::kActDrinkStaminaPotion; break;
        case Family::DrinkMagicka: c.actionOpcode = Vocab::kActDrinkMagickaPotion; break;
        case Family::CastOffense:
        case Family::CastControl:
        case Family::CastHeal:
        case Family::CastBuff:
        case Family::CastSummon:
        case Family::CastWard: {
            c.actionParam = cand.form;
            bool selfDelivery = false;
            for (const auto& e : m_set.snap.book.entries)
                if (e.form == cand.form && e.source == Iaus::SpellSource::Spell) { selfDelivery = e.selfDelivery; break; }
            if (cand.target == 0 || selfDelivery) {
                // Himself (a self heal, buff, ward, summon) or a Self-delivered blast centred on him.
                c.actionOpcode = Vocab::kActCastSelf;
                c.target = {};
            } else {
                c.actionOpcode = Vocab::kActCastTarget;
                // The handle above IS the target (Fire's ladder takes a set handle first). The subject only says
                // where a handle that dies between here and the fire would fall: a HOSTILE spell falls to AUTO
                // (hostile nature -> enemies, never the player or an ally), a beneficial one to the player.
                const bool hostile = cand.family == Family::CastOffense || cand.family == Family::CastControl;
                c.subject = static_cast<std::uint8_t>(hostile ? Vocab::Subject::Self : Vocab::Subject::Player);
            }
            break;
        }
        case Family::Sneak:
        case Family::Posture:
        case Family::Scroll:
        case Family::Exploit:
        case Family::kCount:
            a_whyNot = "no action road for this family yet";
            return false;
        }
        a_out = std::move(c);
        return true;
    }

    bool IausSource::Next(Eval::Choice& a_out) {
        if (g_followers.find(m_id) == g_followers.end()) return false;   // the record is gone: the scan keeps its last choice
        while (const Item* it = Current()) {
            std::string whyNot;
            if (MakeChoice(*it, a_out, whyNot)) return true;
            if (!it->posture) m_notes.push_back({ m_d.order[it->idx].idx, std::move(whyNot) });
            ++m_pos;
        }
        a_out = Eval::Choice{};   // rank -1: no more candidates
        a_out.source = Eval::Source::Iaus;
        return true;
    }

    void IausSource::Reject(const Eval::Choice&, std::string_view a_why) {
        if (const Item* it = Current()) {
            if (it->posture) m_postureNote = std::string(a_why);
            else             m_notes.push_back({ m_d.order[it->idx].idx, std::string(a_why) });
        }
        ++m_pos;
    }

    std::uint32_t IausSource::FiredIdentity(const Eval::Choice&) const {
        const Item* it = Current();
        return (it && !it->posture) ? m_d.order[it->idx].epoch : 0u;
    }

    bool IausSource::HesitationExempt(const Eval::Choice&) const {
        // Self-defence skips the retarget beat, as the table's foe_attacking_me* rows do.
        const Item* it = Current();
        return it && !it->posture && m_set.gen.actions[m_d.order[it->idx].idx].f.foeOnMe;
    }

    void IausSource::Record(const Eval::Choice& a_choice, const Actuation::Outcome& a_outcome,
                            std::chrono::steady_clock::time_point) {
        const Item* it = Current();   // the item the scan stopped on (it never rejected it)
        if (!it) return;
        const bool fired = a_outcome.result == Actuation::Result::Fired;
        if (it->posture) {
            if (fired) {
                Iaus::Engine::NotePostureFired(m_id, m_set.gen.postures[it->idx], m_tick);
                m_postureNote = "swapped";
                Iaus::SpellBook::Invalidate(m_id);   // the measured weapon length follows the hand
            }
            return;
        }
        const Iaus::Step& step = m_d.order[it->idx];
        const Iaus::Candidate& cand = m_set.gen.actions[step.idx];
        if (fired) {
            m_pickedStep = static_cast<int>(it->idx);
            // An approach leg (the power attack closing to melee) is the decision in effect, not the swing: it
            // must not start the hard cooldown of the swing it is closing for (the G9 rule, Outcome::approach).
            Iaus::Engine::NoteFired(m_id, cand, step, m_tick, /*a_startCooldown=*/!a_outcome.approach);
            // MFO-B658: the book's buff / ward "already on", the live summon and the potion stock are up to 5 s
            // old and no container event covers the first three. Re-measure at once after such a fire.
            switch (cand.family) {
            case Family::CastBuff: case Family::CastWard: case Family::CastSummon: case Family::Scroll:
            case Family::DrinkHealth: case Family::DrinkStamina: case Family::DrinkMagicka:
                Iaus::SpellBook::Invalidate(m_id);
                break;
            default: break;
            }
            return;
        }
        // AN ATTACK ALREADY IN EFFECT. Attack's "already on that target" is an opaque NoOp, not a Fired: the latch
        // persists and nothing new happened. For IAUS it still means THIS decision is the one in effect, so it
        // becomes the running decision (momentum, the window, the identity), once. Read from the latch itself, not
        // from the reason text. No cooldown is stamped and the variety ring is untouched (form 0).
        if (cand.family == Family::Attack && a_outcome.result == Actuation::Result::NoOp && a_choice.target &&
            Targeting::Current(m_id) == a_choice.target) {
            const Iaus::Mind& m = Iaus::Engine::MindFor(m_id);
            if (!(m.hasRunning && m.running == Iaus::KeyOf(cand)))
                Iaus::Engine::NoteFired(m_id, cand, step, m_tick, /*a_startCooldown=*/false);
            m_pickedStep = static_cast<int>(it->idx);   // the [iaus] line says "pick=", not "none fired": the attack IS in effect
        }
    }

    void IausSource::AmendScan(bool a_stopped, bool& a_suppressed, bool& a_castSeen, std::vector<int>& a_matchedCast) const {
        if (!m_built) return;
        if (!a_stopped && m_d.verdict == Iaus::Verdict::Hold) a_suppressed = true;
        // The running cast decision is still asked for while its candidate clears the floor (design 1.3). The Mind
        // is read AFTER the scan, so a decision this lap replaced (a new one fired, or an attack was adopted) no
        // longer counts and its loan is released crisply by the `!castSeen` lap.
        const Iaus::Mind& m = Iaus::Engine::MindFor(m_id);
        if (!m.hasRunning || !IsCastFamily(m.running.family)) return;
        for (const auto& r : m_d.ranked) {
            if (r.idx >= m_set.gen.actions.size()) continue;
            if (!(Iaus::KeyOf(m_set.gen.actions[r.idx]) == m.running)) continue;
            const int rank = Iaus::RankOf(m.epoch);
            a_castSeen = true;
            if (std::find(a_matchedCast.begin(), a_matchedCast.end(), rank) == a_matchedCast.end())
                a_matchedCast.push_back(rank);
            break;
        }
    }

    // ── Lifecycle ────────────────────────────────────────────────────────────────────────────────────────────

    bool IausToggleEdge(RE::Actor* a_self, RE::FormID a_id, bool a_iausNow) {
        if (!Iaus::Engine::NoteSource(a_id, a_iausNow)) return false;
        spdlog::info("[iaus] {} ({:08X}) combat source: {} -- the running decision is stood down (cast locks, heal "
                     "hold, cast claims, cast latch, target latch, window); the weapon hold is left to the new "
                     "source's reconciliation",
                     a_self && a_self->GetName() ? a_self->GetName() : "?", a_id,
                     a_iausNow ? "gambit table -> IAUS" : "IAUS -> gambit table");
        StandDown(a_id, "combat source changed");
        return true;
    }

    void IausCombatEnded(RE::FormID a_id) {
        Iaus::Engine::EndDecision(a_id);
        Iaus::Candidates::CombatEnded(a_id);
    }

    void IausSweep() {
        static std::uint32_t s_pump = 0;   // worker only
        if ((++s_pump & 31u) != 0) return;   // every 32nd pump, about 4 s: a dismissed follower's Mind is small and inert
        for (const RE::FormID id : Iaus::Engine::Noted()) {
            const bool tracked = std::find(Followers::g_activeIds.begin(), Followers::g_activeIds.end(), id) !=
                                 Followers::g_activeIds.end();
            if (tracked && g_followers.find(id) != g_followers.end()) continue;
            // He left the party (the dismissal road already released every hold on this worker) or lost his record.
            Iaus::Engine::Dismiss(id);
            Iaus::Candidates::Forget(id);
        }
    }

    void IausRetreatSelfHeal(RE::Actor* a_self, RE::FormID a_id, std::uint8_t a_rapportRank,
                             std::chrono::steady_clock::time_point a_now) {
        // The table walk's three gates, unchanged: StopCombat has landed, he can act, bCastSelf is on.
        if (!a_self || APMFBridge::InCombat(a_self) || Actuation::CannotActReason(a_self) || !Config::g_castSelf.load()) return;
        const Iaus::Tuning& T = Iaus::DefaultTuning();
        Iaus::Candidates::Set set;
        if (!Iaus::Candidates::Build(a_self, set, T)) return;
        const Iaus::Mind& mind = Iaus::Engine::MindFor(a_id);
        const Iaus::Candidate* best = nullptr;
        float bestScore = T.actFloor;
        for (const auto& c : set.gen.actions) {
            if (c.family != Family::CastHeal || c.target != 0) continue;   // himself only
            const float s = Iaus::ScoreOne(c, mind, DefaultProfile(), T, a_rapportRank, 0.0f, nullptr);
            if (s > bestScore) { bestScore = s; best = &c; }
        }
        if (!best) return;
        auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(best->form);
        if (!sp || !Actuation::HealsHealth(sp)) return;
        // Only a package-free direct road runs under a retreat (the table walk's test).
        if (ComposedCast::ChooseHealRoad(a_self, sp, a_self) == ComposedCast::HealRoad::Claim) return;
        Eval::Choice c;
        c.source       = Eval::Source::Iaus;
        c.rank         = Iaus::RankOf(mind.epoch);
        c.actionOpcode = Vocab::kActCastSelf;
        c.actionParam  = best->form;
        const Actuation::Outcome o = Actuation::Fire(a_self, c);
        if (o.transparent) return;
        auto& last = g_retreatHealLog[a_id];
        if (a_now - last > std::chrono::seconds(3)) {
            last = a_now;
            spdlog::info("[retreat-heal] {:08X}: IAUS self-heal {} ({:08X}, score {:.2f}) while retreating: {}",
                         a_id, sp->GetName() ? sp->GetName() : "?", best->form, bestScore, o.reason);
        }
    }

}
