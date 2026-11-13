// iaus/Catalogue.cpp -- the consideration catalogue (design section 3): names, roles, input extraction, the
// per-family consideration sets, and DefaultTuning() -- every curve parameter and weight, each with its reason.
// PURE (iaus/Core.h). I9 tunes by editing DefaultTuning(); the parameters are data, not code.
//
// HOW TO READ A ROW: "doc" = the parameter is the design's own (iaus-design.md section 3). "chosen" = the design
// names the shape without a number, or its number contradicts its own worked example; the reason is given.
#include "iaus/Iaus_internal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace MFO::Iaus {

    namespace {
                float B(bool v) { return v ? 1.0f : 0.0f; }
        float Clamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

        // The distance at which the posture layer's ranged and melee curves cross (design: d/1200). 1200 units
        // is about 17 m, the vanilla "ranged is worth it" band; the curves' x0 = 0.45 puts the crossover at
        // ~540 units (~7.7 m), just outside a power-attack lunge.
        constexpr float kPostureDist = 1200.0f;
    }

    std::string_view Name(Family f) {
        switch (f) {
        case Family::Attack:       return "attack";
        case Family::PowerAttack:  return "power_attack";
        case Family::Taunt:        return "taunt";
        case Family::Sneak:        return "sneak";
        case Family::Retreat:      return "flee";
        case Family::DrinkHealth:  return "drink_health";
        case Family::DrinkStamina: return "drink_stamina";
        case Family::DrinkMagicka: return "drink_magicka";
        case Family::Posture:      return "equip";
        case Family::CastOffense:  return "cast_offense";
        case Family::CastControl:  return "cast_control";
        case Family::CastHeal:     return "cast_heal";
        case Family::CastBuff:     return "cast_buff";
        case Family::CastSummon:   return "cast_summon";
        case Family::CastWard:     return "cast_ward";
        case Family::Scroll:       return "use_scroll";
        case Family::Exploit:      return "exploit";
        case Family::Regroup:      return "regroup";
        case Family::GiveGround:   return "give_ground";
        case Family::Block:        return "block";
        case Family::Bash:         return "bash";
        case Family::Disengage:    return "disengage";
        case Family::BackOff:      return "back_off";
        case Family::Step:         return "step";
        case Family::DrinkBuff:    return "drink_buff";
        case Family::kCount:       break;
        }
        return "?";
    }

    std::string_view Name(PostureKind p) {
        switch (p) {
        case PostureKind::None:   return "none";
        case PostureKind::Melee:  return "melee";
        case PostureKind::Bow:    return "ranged";
        case PostureKind::Staff:  return "staff";
        case PostureKind::Silver: return "silver";
        }
        return "?";
    }

    std::string_view Name(Cons c) {
        static constexpr std::string_view kNames[] = {
            "range", "range", "los", "threat", "on_me", "on_player", "on_ally", "player_target", "current_target",
            "finish", "affordable", "mp_reserve", "cooldown", "recast", "variety", "will_work", "target_cond",
            "already_active", "hand_free", "splash_safe", "exploit", "class_fit",
            "foe_blocking", "stamina", "foe_stronger", "taunted", "self_hp", "unseen", "fight_start", "self_hp",
            "no_heal", "disengage", "need", "stock", "no_heal", "ranged_dist", "melee_dist", "ammo",
            "staff_charge", "melee_reach", "unholy", "mundane_vs_unholy", "class_weapon", "area", "controlled",
            "foe_count", "recipient_hp", "heal_reach", "early", "in_melee", "live_summon", "foes", "caster_on_me",
            "self_hp", "scroll_stock", "mp_low", "engaged",
            "caster", "downed", "outnumbered",
            "damage", "overkill", "survive", "danger", "heal_size", "player_dying", "recipient_dying",
            "chase", "helpless", "taunt_chance", "heal_claimed", "heal_urgency",
            "control_worth", "magicka_buys", "range", "mortal", "mortal_danger", "trade",
            "need", "far", "need", "need", "need",
            "strike", "hold", "need", "flee_heal", "need", "guard_line",
            "buff_threat", "buff_stealth",
            "buff_foes", "perks", "consumable_worth",   // batch 3 (review F7: the old name threat was FoeThreat's; every appended name is unique)
        };
        static_assert(std::size(kNames) == kCons, "Cons names out of step with the enum");
        const auto i = static_cast<std::size_t>(c);
        return i < kCons ? kNames[i] : "?";
    }

    Role RoleOf(Cons c) {
        switch (c) {
        case Cons::CooldownHard: case Cons::WillWork: case Cons::TargetCond: case Cons::AlreadyActive:
        case Cons::HandFree: case Cons::SplashSafe: case Cons::FoeTaunted: case Cons::Unseen:
        case Cons::NoHealRetreat: case Cons::DisengageClass: case Cons::Stock: case Cons::NoHealDrink:
        case Cons::Ammo: case Cons::StaffCharge: case Cons::MeleeReach: case Cons::ClassWeapon:
        case Cons::FoeControlled: case Cons::HealReach: case Cons::SummonLive: case Cons::HoldStill: case Cons::BuffStealth:
        case Cons::ConsumableWorth:   // feat/consumables fix F3: a veto only (worth >= floor scores as before; it must not enter the compensation n)
            return Role::Gate;
        case Cons::BoostCaster: case Cons::BoostDowned: case Cons::BoostOutnumbered:
        case Cons::BoostPlayerDying: case Cons::BoostRecipientDying: case Cons::BoostControlled:
        case Cons::MortalTaunt: case Cons::BoostMortalDanger: case Cons::BoostStrike: case Cons::BoostFleeHeal: case Cons::BoostGuardLine:
        case Cons::BoostBuffThreat: case Cons::BoostPosturePerks:
            return Role::Boost;
        default:
            return Role::Shape;
        }
    }

    Key KeyOf(const Candidate& c) { return Key{ c.family, c.f.posture, c.form, c.target }; }

    std::uint64_t CooldownId(const Key& k) {
        return (static_cast<std::uint64_t>(k.family) << 40) | (static_cast<std::uint64_t>(k.posture) << 32) |
               static_cast<std::uint64_t>(k.form);
    }

    bool FactsFinite(const Facts& f) {
        const float v[] = { f.dist, f.reach, f.foeLevelRatio, f.foeHpPct, f.cost, f.magicka, f.magickaMax,
                            f.hardCooldownSec, f.hardCooldownLeft, f.resist, f.foesInArea, f.selfHp, f.selfStamina,
                            f.selfMagicka, f.recipientHp, f.combatSec, f.emergency,
                            f.expDamage, f.bestDamage, f.foeHealth, f.expHeal, f.recipientMissing, f.chase, f.tauntChance,
                            f.controlSec, f.castsWanted, f.backOffNeed, f.buffThreat, f.buffRegenPct, f.consumableWorth };
        for (const float x : v)
            if (!std::isfinite(x)) return false;
        // +inf is the documented "not recently" / "nothing is hitting" (the estimates' unknown)
        return !std::isnan(f.sinceFired) && !std::isnan(f.selfTtl) && !std::isnan(f.tauntTtl) && !std::isnan(f.playerTtl) &&
               !std::isnan(f.recipientTtl) && !std::isnan(f.meleeThreatDist) && !std::isnan(f.threatTtl);
    }

    float EmergencyLevel(float worst, const Tuning& t) {
        const float span = t.emergencyHpHigh - t.emergencyHpLow;
        if (!(span > 0.0f)) return worst <= t.emergencyHpLow ? 1.0f : 0.0f;
        return Clamp((t.emergencyHpHigh - worst) / span, 0.0f, 1.0f);
    }

    void BumpRow(Profile& p, Family f, RoleTag tag, int index, int rows) {
        if (rows <= 0 || index < 0 || index >= rows) return;
        const float bump = 0.5f * static_cast<float>(rows - index) / static_cast<float>(rows);
        if (tag == RoleTag::None) p.family[static_cast<std::size_t>(f)] += bump;
        else                      p.role[static_cast<std::size_t>(tag)] += bump;
    }

    void SetWeight(Profile& p, Family f, Cons c, float w) {
        for (auto& o : p.weights)
            if (o.family == f && o.cons == c) { o.w = w; return; }
        p.weights.push_back({ f, c, w });
    }

    // ── The default tuning ─────────────────────────────────────────────────────────────────────────────
    const Tuning& ShippedTuning() {
        static const Tuning t = [] {
            Tuning d;
            auto& C = d.curve;
            auto at = [&](Cons c) -> Curve& { return C[static_cast<std::size_t>(c)]; };

            // range: x = distance / the action's reach. doc: Step 0 above 1.15x reach.
            // Melee: CHOSEN flat 1.0 up to 1.0x reach, then down to 0.8 at the 1.15x gate (review round 1, finding
            // 4). The doc's Inv(2) "close is best" shaded a plain attack on the current target under the 0.20 act
            // floor at 0.9-1.15x reach (0.16), so a melee follower at its weapon edge would Idle; and inside the
            // weapon's reach every distance hits the same, so closeness is not a utility. The 1.0-1.15 band stays a
            // mild shade (the engine closes the last step itself).
            {
                static constexpr std::array<std::array<float, 2>, 3> p{ { { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.15f, 0.8f } } };
                at(Cons::RangeMelee) = VetoAbove(Piece(p), 1.15f);
            }
            // Spells doc Logi(-10, 0.85): ~1 inside half range, 0.5 at 85%, 0.05 at the 1.15x edge.
            at(Cons::RangeSpell) = VetoAbove(Logi(-10.0f, 0.85f), 1.15f);
            // los: x = Occluded 0 / Unknown 0.5 / Visible 1 -> doc 0.2 / 0.9 / 1.0 (not 0: the cast guard holds
            // and the engine re-aims).
            {
                static constexpr std::array<std::array<float, 2>, 3> p{ { { 0.0f, 0.2f }, { 0.5f, 0.9f }, { 1.0f, 1.0f } } };
                at(Cons::Los) = Piece(p);
            }
            // threat: x = clamp(foe level / my level, 0, 2) / 2 -> doc Lin(0.5, 0.3): 0.3 (trivial) .. 0.8 (2x my
            // level). A threat term never vetoes: the weakest foe still scores 0.3.
            at(Cons::FoeThreat) = Lin(0.5f, 0.3f);
            at(Cons::FoeOnMe) = Bin(0.6f, 1.0f);          // doc
            at(Cons::FoeOnPlayer) = Bin(0.6f, 1.0f);      // doc (player focus multiplies on top)
            at(Cons::FoeOnAlly) = Bin(0.7f, 1.0f);        // doc
            at(Cons::PlayerTarget) = Bin(0.7f, 1.0f);     // doc
            at(Cons::CurrentTarget) = Bin(0.85f, 1.0f);   // doc: a target change costs a "sizing up" tick
            at(Cons::FoeFinish) = Inv(0.5f, 0.4f);        // doc: x = foe HP pct; 1 - sqrt(hp), floor 0.4
            // affordable: x = (magicka - cost) / max. doc: Step 0 when cost > pool (x < 0), else Lin(0.4, 0.6).
            at(Cons::Affordable) = VetoBelow(Lin(0.4f, 0.6f), 0.0f);
            // mp_reserve: x = magicka pct left after the cast. doc Logi(12, 0.15) at w 0.5. Shapes only (marth: no
            // reserve floor, cast till dry): 0.14 at 0, 0.5 at 15%, 0.95 at 40%; never 0.
            at(Cons::MpReserve) = Logi(12.0f, 0.15f);
            at(Cons::CooldownHard) = Bin(0.0f, 1.0f);     // gate: inside a class row's hard cooldown
            // recast: x = seconds since this spell fired / softCooldownSec (4 s, doc). doc Lin(1/T, 0) recovers
            // 0 -> 1 over T; chosen floor 0.4 (Lin(0.6, 0.4)): from 0 a mage with one damage spell could not repeat
            // it for a whole lap, which REMOVES a move (memory attacks-must-be-additive) and adds pacing the
            // executors already own (memory cast-cooldown-inert-is-correct). It shades a repeat, never forbids it.
            at(Cons::CooldownSoft) = Lin(0.6f, 0.4f);
            // variety: x = (same form in the last 4 Fired) / 4. doc Lin(-0.15, 1.0) per repeat == Lin(-0.6, 1) on
            // x: 1.0 / 0.85 / 0.70 / 0.55 / 0.40, at w 0.5. Never 0.
            at(Cons::Variety) = Lin(-0.6f, 1.0f);
            at(Cons::WillWork) = Bin(0.0f, 1.0f);         // gate, Unknown passes (G10)
            at(Cons::TargetCond) = Bin(0.0f, 1.0f);       // gate (Turn Undead = undead only)
            at(Cons::AlreadyActive) = Bin(0.0f, 1.0f);    // gate
            at(Cons::HandFree) = Bin(0.0f, 1.0f);         // gate: never preempt a standing heal's hand (1.3)
            at(Cons::SplashSafe) = Bin(0.0f, 1.0f);       // gate: never splash the party (marth)
            // exploit: x = the target's resist AV for the spell's element (points; 0 for a non-elemental spell,
            // which is never resisted, so it sits with "neutral"). doc points: <= -25 1.0, 0 0.6, 50 0.3, 85 0.08,
            // >= 100 0 (veto: an immune foe).
            {
                static constexpr std::array<std::array<float, 2>, 5> p{ {
                    { -25.0f, 1.0f }, { 0.0f, 0.6f }, { 50.0f, 0.3f }, { 85.0f, 0.08f }, { 100.0f, 0.0f } } };
                at(Cons::Exploit) = Piece(p);
            }
            // class_fit (ruling Q34): x is computed from offClassBase / offClassEmergencyLift / offClassCap (see
            // Input); the curve is the identity so the Tuning fields stay the one place to tune it.
            at(Cons::ClassFit) = Lin(1.0f, 0.0f);

            at(Cons::FoeBlocking) = Bin(0.5f, 1.0f);      // doc: a power attack breaks a guard
            at(Cons::SelfStamina) = Logi(10.0f, 0.35f);   // doc
            at(Cons::FoeStronger) = Bin(0.7f, 1.0f);      // doc
            at(Cons::FoeTaunted) = Bin(0.0f, 1.0f);       // gate: already taunted
            at(Cons::TauntSelfHp) = Logi(10.0f, 0.45f);   // doc: a hurt tank stops pulling aggro
            at(Cons::Unseen) = Bin(0.0f, 1.0f);           // gate: the hold row stands only while every listed foe's group record says he is not detected (Unknown = not applicable)
            at(Cons::DisengageNeed) = VetoBelow(Lin(1.0f, 0.0f), 0.001f);   // card G3: chosen (not searched); the input is already a 0..1 need
            at(Cons::BoostFleeHeal) = Bin(1.0f, 2.0f);    // card G3, chosen: his own heal wins over a buff or a posture while he flees (offense is not generated then)
            at(Cons::HoldStill) = Bin(0.0f, 1.0f);        // gate: the hold row yields to the blow from stealth the moment one is ready
            at(Cons::FightStart) = Inv(1.0f);             // doc: x = combat seconds / 6, an opener only
            at(Cons::RetreatHp) = VetoAbove(Inv(3.0f), 0.30f);   // doc: only below 30% HP
            at(Cons::NoHealRetreat) = Bin(0.0f, 1.0f);    // gate, threshold Tuning::noHealRetreat (doc 0.3)
            at(Cons::DisengageClass) = Bin(0.0f, 1.0f);   // gate: Flag::kDisengage
            // need: x = the drink's own pct (HP / stamina / magicka). CHOSEN Logi(-11, 0.34), NOT the doc's
            // Inv(2.5): the doc's own worked example ("0.3 HP scores 0.6, 0.1 scores 0.93") is what the curve must
            // do, and Inv(2.5) gives 0.95 / 1.00 there and 0.82 at HALF health (a potion at 50% HP). Logi(-11,
            // 0.34) hits 0.61 at 0.3 and 0.92 at 0.1, 0.15 at 0.5, ~0 near full; vetoed at full (kHealFull 0.9995).
            at(Cons::DrinkNeed) = VetoAbove(Logi(-11.0f, 0.34f), 0.9995f);
            at(Cons::Stock) = Bin(0.0f, 1.0f);            // gate: none carried
            at(Cons::NoHealDrink) = Bin(0.0f, 1.0f);      // gate, threshold Tuning::noHealDrink (doc 0.5)
            // posture: x = distance to the current target / 1200. doc ranged Logi(8, 0.45), melee the inverse.
            at(Cons::PostureRanged) = Logi(8.0f, 0.45f);
            at(Cons::PostureMelee) = Logi(-8.0f, 0.45f);
            at(Cons::Ammo) = Bin(0.0f, 1.0f);             // gate: bow with no arrows
            at(Cons::StaffCharge) = Bin(0.0f, 1.0f);      // gate: empty staff
            at(Cons::MeleeReach) = Bin(0.0f, 1.0f);       // gate: OutOfMeleeReach vetoes melee
            at(Cons::Unholy) = Bin(0.2f, 1.0f);           // doc: silver, x = target Unholy
            // mundane_vs_unholy: CHOSEN, not in the doc. A plain melee weapon against an Unholy target scores
            // 0.5. Without it silver can only TIE melee at equal distance and the drawn melee weapon's momentum
            // (x1.5) means silver never wins; with it silver wins vs Unholy (0.97 vs 0.73 with momentum) and still
            // loses to melee vs anything else (0.28).
            at(Cons::MundaneVsUnholy) = Bin(1.0f, 0.5f);
            at(Cons::ClassWeapon) = Bin(0.0f, 1.0f);      // gate under strict adherence (G16)
            at(Cons::AreaValue) = Lin(1.0f, 0.5f);        // doc: x = foes in area / 3 (1 foe 0.83, 2+ 1.0)
            at(Cons::FoeControlled) = Bin(0.0f, 1.0f);    // gate
            at(Cons::FoeCountControl) = Bin(0.3f, 1.0f);  // doc: controlling the only foe wastes a turn
            at(Cons::HealNeed) = VetoAbove(Inv(2.0f), 0.9995f);   // doc: x = recipient HP pct, full = 0
            at(Cons::HealReach) = Bin(0.0f, 1.0f);        // gate: HealInReach
            // early: x = combat seconds / 20. doc Inv(1) over 20 s ("buffs early"); chosen floor 0.3: a pure Inv(1)
            // is 0 after 20 s, a VETO, so a buff that expired in a long fight could never be recast.
            at(Cons::BuffEarly) = Inv(1.0f, 0.3f);
            at(Cons::BuffMelee) = Bin(0.6f, 1.0f);        // chosen: armor / cloak buff on a recipient not in melee
            at(Cons::SummonLive) = Bin(0.0f, 1.0f);       // gate
            at(Cons::SummonFoes) = Lin(0.25f, 0.5f);      // doc: x = min(foes, 4) / 4
            // caster_on_me: CHOSEN Bin(0.2, 1). The doc names the input only. A ward with no hostile caster on me
            // is close to useless (0.2), but not vetoed, because "targets me" is the best read until G11.
            at(Cons::WardCaster) = Bin(0.2f, 1.0f);
            // ward self HP: doc Inv(1.5); chosen floor 0.15 so a full-HP mage under a caster's fire can still ward
            // first (Inv(1.5) is 0, a veto, at full HP).
            at(Cons::WardHp) = Inv(1.5f, 0.15f);
            at(Cons::ScrollStock) = Lin(0.2f, 0.8f);      // doc: x = count / 5, hoard the last one a little
            // mp_low: doc Inv(1) on self magicka ("scrolls shine when dry"); chosen floor 0.3: Inv(1) is 0 at full
            // magicka, which would veto every scroll for a full mage, so a scroll could never open a fight.
            at(Cons::ScrollMpLow) = Inv(1.0f, 0.3f);
            // engaged (Attack, Power attack): CHOSEN, replacing the doc's three separate on_me / on_player /
            // on_ally Bins in those two sets. A foe targets exactly ONE actor, so three Bins always shade every
            // foe by the two relations it cannot have at once (0.6 x 0.7 even for the foe hitting me): a constant
            // ~0.7 tax after compensation that carries no information and pushed a plain melee attack (0.18) under
            // the 0.20 act floor in the harness. One term keeps the doc's RELATIVE values exactly (its Bins give
            // player 0.42 : me 0.42 : ally 0.36 : none 0.25 -> 1.0 / 1.0 / 0.86 / 0.6) without the tax. Taunt keeps
            // the separate Bins at w 2 (it asks a different question: is an ally or the player under attack).
            {
                static constexpr std::array<std::array<float, 2>, 4> p{ {
                    { 0.0f, 0.6f }, { 1.0f / 3.0f, 0.857f }, { 2.0f / 3.0f, 1.0f }, { 1.0f, 1.0f } } };
                at(Cons::FoeEngaged) = Piece(p);
            }
            at(Cons::BoostCaster) = Bin(1.0f, 1.15f);     // doc: x1.15 on a caster foe
            at(Cons::BoostDowned) = Bin(1.0f, 1.4f);      // doc: x1.4 heal on a downed recipient
            at(Cons::BoostOutnumbered) = Bin(1.0f, 1.3f); // doc: x1.3 summon when foes > party

            // ── estimates (iaus/Estimate.h; sim v2 phase 2, Docs/research/2026-11/iaus-sim-v2.md "Phase 2") ──
            // damage: x = this spell's expected damage on the target / his best castable spell's on it. The best
            // spell is 1.0, one doing a third of it 0.53. Never 0: a weak spell is still a cast.
            at(Cons::DamageValue) = Lin(0.7f, 0.3f);
            // overkill: x = target health / expected damage, capped at 1. A cast that fits scores 1; one doing
            // five times what is left 0.5. Shapes only: finishing a foe is never wrong, wasting most of a big spell is.
            {
                static constexpr std::array<std::array<float, 2>, 3> p{ { { 0.0f, 0.35f }, { 0.5f, 0.75f }, { 1.0f, 1.0f } } };
                at(Cons::Overkill) = Piece(p);
            }
            // survive (taunt): x = seconds he would live with this foe on him too / 12. Under 3 s the taunt is
            // nearly thrown away (0.15, never a veto: a sacrifice must stay possible), 6 s 0.7, 12 s and more 1.
            {
                static constexpr std::array<std::array<float, 2>, 4> p{ { { 0.0f, 0.15f }, { 0.25f, 0.3f }, { 0.5f, 0.7f }, { 1.0f, 1.0f } } };
                at(Cons::TauntSurvive) = Piece(p);
            }
            // danger (retreat): x = max(low-health danger, seconds-to-live danger), see Input. No danger = veto, as
            // the 30% health gate was.
            at(Cons::RetreatDanger) = VetoBelow(Lin(0.665f, 0.0f), 0.001f);   // slope: set A2 (was 1.0)
            // heal size: x = missing health / the heal, capped at 1. A heal he can use in full 1.0, one four times
            // too big 0.55.
            at(Cons::HealSize) = Lin(0.6f, 0.4f);
            at(Cons::BoostPlayerDying) = Bin(1.0f, 1.8f);      // the player has under kDyingSec to live
            at(Cons::BoostRecipientDying) = Bin(1.0f, 1.4f);   // the recipient has
            // chase (melee attack). Ruling Q70 (marth 2026-11-11, clarifying Q64): TARGET choice is not gated by
            // reach, a melee follower may name any foe in chase range; the reach gate is for the SWINGS IAUS times
            // (the power attack keeps RangeMelee). x = distance / the chase cap: 1.0 in reach, 0.75 at 15% of the cap
            // (about 600 units), 0.5 at the cap, vetoed beyond it.
            {
                static constexpr std::array<std::array<float, 2>, 4> p{ { { 0.0f, 1.0f }, { 0.05f, 1.0f }, { 0.15f, 0.75f }, { 1.0f, 0.5f } } };
                at(Cons::RangeChase) = VetoAbove(Piece(p), 1.0f);
            }
            at(Cons::BoostControlled) = Bin(1.0f, 1.3f);       // a helpless foe: every blow lands
            // taunt chance (ruling Q69): x = the chance the attempt pins this foe. A 0.9 taunt scores 0.92, a coin flip
            // 0.6, the 0.1 floor 0.28: a long shot is dear (30 s cooldown) but never forbidden.
            at(Cons::TauntChance) = Lin(0.8f, 0.2f);
            // heal claimed: another follower's heal is already on its way to this recipient. A second heal mostly
            // overheals (sim: 16 to 24% of heal casts were doubles): 0.35, not a veto (a dying recipient may need both).
            at(Cons::HealClaimed) = Bin(1.0f, 0.35f);
            // heal urgency: x = the recipient's seconds to live / 10 at the damage he is taking. 0 s 1.0, 5 s 0.8, 10 s
            // and more 0.55: a heal before the blow lands, not after (not applicable while nothing is hitting him).
            {
                static constexpr std::array<std::array<float, 2>, 3> p{ { { 0.0f, 1.0f }, { 0.5f, 0.8f }, { 1.0f, 0.55f } } };
                at(Cons::HealUrgency) = Piece(p);
            }

            // taunt range: x = distance / the taunt's reach. Flat inside, vetoed beyond 1.15 x (the same outer edge the aimed
            // curve had, so no taunt that was a candidate stops being one): the taunt is a call, it lands the same at 5 m
            // and at 1 m. (The aimed-spell curve shaded a taunt at 90% of its reach to 0.38.)
            at(Cons::RangeTaunt) = VetoAbove(Lin(0.0f, 1.0f), 1.15f);
            // mortal taunt (a multiplier, applies only to a follower who dies for good): x = seconds he would live with this
            // foe on him / 12. Under 3 s the taunt is all but refused (x0.15), 6 s x0.6, 12 s and more x1. For a follower
            // who stands up again the survive term alone decides, as before.
            {
                static constexpr std::array<std::array<float, 2>, 4> p{ { { 0.0f, 0.15f }, { 0.25f, 0.15f }, { 0.5f, 0.6f }, { 1.0f, 1.0f } } };
                at(Cons::MortalTaunt) = Piece(p);
            }
            // mortal danger (retreat, health potion): x1.5 for a follower who dies for good. His life is not a resource.
            at(Cons::BoostMortalDanger) = Bin(1.0f, 1.5f);
            // blow from stealth (G2 Layer 1): x1.5 on an Attack / Power attack at a foe whose group does not detect him while he
            // sneaks. CHOSEN, not measured: the engine multiplies the damage itself (weapon game setting x perk entry 18, a
            // factor 1.5 to 3 and more), so this is only the nudge that makes him take the unaware foe before an aware one. I9
            // tunes it from the [stealth-probe] hit/q87 lines.
            at(Cons::BoostStrike) = Bin(1.0f, 1.5f);
            // trade (the melee weapon category): x = seconds he lives at the damage coming in / 10. Ten seconds and more
            // 1.0, two seconds 0.44: a follower who cannot trade blows does not put the bow away because the foe came near.
            // Not applicable while nothing is known to be hitting him.
            at(Cons::PostureTrade) = Lin(0.7f, 0.3f);
            // give ground / block / bash need: see Input. Under 0.05 there is no reason: veto.
            at(Cons::GroundNeed) = VetoBelow(Lin(1.0f, 0.0f), 0.05f);
            at(Cons::BackOffNeed) = VetoBelow(Lin(1.0f, 0.0f), 0.05f);   // batch I, chosen (the sim tunes it): the input is already a 0..1 need
            at(Cons::BlockNeed) = VetoBelow(Lin(1.0f, 0.0f), 0.05f);
            // BUFF POTIONS (feat/buff-potions, ClickUp 1dfcuabd18f). PLACEHOLDERS, NOT SEARCHED: no number below came from the sim or the oracle.
            // The sim search and the oracle set them later (marth: "sim calibrates it", Q-potion-value); until then each is the plainest shape
            // that does its job. buff_threat: x = the share of the kept foes whose incoming element the resist potion answers (already 0..1);
            // under 0.05 there is nothing to resist, veto. buff_stealth: a gate (he sneaks, or is a sneak class). fight_start (existing, curve
            // untouched) and need (existing DrinkNeed curve, untouched) are reused for the fortify and regen kinds.
            at(Cons::BuffThreat) = VetoBelow(Lin(1.0f, 0.0f), 0.05f);
            at(Cons::BuffStealth) = Bin(0.0f, 1.0f);
            // Q-S-5 (marth 2026-11-12: "the line should block at least 60% of attacks directed at them when 2+ foes are taunted"): while the
            // Knight line holds two or more foes taunted by HIM the guard outranks his blows (x2.0 over Block's own 1.0 weight). It does NOT
            // outrank a health potion by weight alone (a held guard is 2.0 x 1.25 momentum = 2.5, the potion at most 2.2): Generate turns the
            // line OFF (tauntedOnMe 0) below kDrinkHealthLine with a health potion in stock, so a hurt Knight drinks first. CHOSEN, tuned
            // in the sim (iaus-sim-v2 section 31.12).
            at(Cons::BoostGuardLine) = Bin(1.0f, 2.0f);
            // buff_threat (batch 3, iaus-sim-v2.md 35): FLAT 1.0 through (0, 1) and (1, 1), so the shipped catalogue scores every buff as
            // before (a boost of exactly 1). The two points are the searched knobs (py0: no foe in the fight, py1: four or more): the sim
            // decides whether a buff is worth less with no foe and more against many; a value is written only through the driver's gates.
            {
                static constexpr std::array<std::array<float, 2>, 2> p{ { { 0.0f, 1.0f }, { 1.0f, 1.0f } } };
                at(Cons::BoostBuffThreat) = Piece(p);
            }
            // perks (batch 3 part C, marth 11-12): FLAT 1.0 through (0, 1), (0.5, 1), (1, 1): bit-identical. x = this posture's share of his weapon-damage
            // perk multipliers (0.5 = equal investment). The three points are the searched knobs (the sim decides how much perk investment weighs).
            {
                static constexpr std::array<std::array<float, 2>, 3> p{ { { 0.0f, 1.0f }, { 0.5f, 1.0f }, { 1.0f, 1.0f } } };
                at(Cons::BoostPosturePerks) = Piece(p);
            }
            at(Cons::BashNeed) = VetoBelow(Lin(1.0f, 0.0f), 0.05f);
            // regroup need: the largest of three reasons, see Input. Under 0.05 there is no reason: veto.
            at(Cons::RegroupNeed) = VetoBelow(Lin(1.0f, 0.0f), 0.05f);
            // regroup far: x = distance to the anchor / 1000. Inside 300 units he is already there (veto), 350 0.5, 800
            // and more 1.0, past 3000 it is not a regroup any more (veto: that is a retreat or a travel).
            {
                static constexpr std::array<std::array<float, 2>, 3> p{ { { 0.3f, 0.4f }, { 0.35f, 0.5f }, { 0.8f, 1.0f } } };
                at(Cons::RegroupFar) = VetoAbove(VetoBelow(Piece(p), 0.3f), 3.0f);
            }
            // control worth: x = min(1, seconds held / 10) x (0.4 + 0.6 x the foe's health share). Ten seconds on a fresh foe
            // 1.0; a 2 s stagger-length hold 0.36; a long hold on a foe at a quarter health 0.66. A hold is worth the blows
            // it stops and the free blows it gives, and both need the foe to live through it. Never 0.
            at(Cons::ControlWorth) = Lin(0.8f, 0.2f);
            // magicka buys: x = min(1, casts the fight still asks for / 3). Three casts and more 1.0, one cast 0.45, none
            // (the foes left die to one blow) 0.18: a potion drunk for a fight that is over is the bot's tell.
            at(Cons::MagickaBuys) = Lin(0.82f, 0.18f);
            // consumable_worth (feat/consumables): x = Worth(need, cost share), 0..1 (iaus/ConsumableWorth.h). A GATE (RoleOf): vetoed below
            // WorthTable::floor (0.2), 1 and not counted in n above it. Flat-safe: it is NOT APPLICABLE (Input Off) for every candidate that sets no Facts::consumableWorth.
            at(Cons::ConsumableWorth) = VetoBelow(Lin(1.0f, 0.0f), d.worth.floor);
            // Action weights (section 3.2, all doc). Heal 1.2 and drinks 1.1 are the only ones above 1: saving a
            // life beats dealing damage at equal utility.
            auto aw = [&](Family f, float w) { d.actionWeight[static_cast<std::size_t>(f)] = w; };
            // PARAMETER SET A2 (sim v2, Docs/research/2026-11/iaus-sim-v2.md sections 16.3 and 24.2): searched on LoreRim,
            // kept only where Tuxborn, Vanilla and a second seed agree. Against the doc values in brackets, with the
            // estimates NOT measured (the game today) it is worth +1.4 to +3.8 J on every list and seed, and more with them.
            //   power attack 2.394 [0.9]   a timed heavy blow is the melee follower's strongest decision
            //   taunt 0.75 [1.0]           retreat 0.665 [0.5]
            //   magicka potion 0.275 [1.1] a potion rarely buys what the fight still needs (see MagickaBuys)
            //   offense casts 1.769 [1.0]  control casts 3.6 [0.9]   heal casts 2.4 [1.2]
            //   health potion 2.2 [1.1]    NOT from the search (it is neutral there, 67.25 against 67.23): raised with the
            //                              power attack so a follower at a fifth of his health still drinks before he swings
            //                              (the harness check "the health potion ranks first" is the intent it keeps)
            // The control weight stands in for an input that does not exist yet (what the hold buys against what damage
            // buys, gap G13): expect it to come down when that input lands.
            aw(Family::Attack, 0.8f);      aw(Family::PowerAttack, 2.394f); aw(Family::Taunt, 0.75f);
            aw(Family::Sneak, 0.6f);       aw(Family::Retreat, 0.665f);    aw(Family::DrinkHealth, 2.2f);
            aw(Family::DrinkStamina, 1.1f); aw(Family::DrinkMagicka, 0.275f); aw(Family::Posture, 1.0f);
            aw(Family::CastOffense, 1.769f); aw(Family::CastControl, 3.6f); aw(Family::CastHeal, 2.4f);
            aw(Family::CastBuff, 0.7f);    aw(Family::CastSummon, 0.9f);   aw(Family::CastWard, 0.8f);
            aw(Family::Scroll, 0.9f);      aw(Family::Exploit, 1.0f);
            aw(Family::Disengage, 1.0f);   // card G3, chosen (not from the search): gated by its generator, paced by its hard cooldown
            aw(Family::BackOff, 1.0f);     // batch I, chosen: like the other moves, it has to be clearly worth it (the need's own veto and the hard cooldown pace it)
            aw(Family::Step, 1.197f);      // batch I item 5, chosen 0.9; tuning round 2026-11-12 (iaus-sim-v2 31.11 gates) 1.197: above the hold row it replaces (Sneak 0.6) and below a blow (a blow from stealth is generated at once when ready)
            aw(Family::DrinkBuff, 0.5f);   // PLACEHOLDER (feat/buff-potions): NOT from the sim, not hand-tuned. MEASURED (harness, lap one, Knight melee): a fortify
                                           // or resist candidate scores 0.500 against an Attack's 0.42 (weight 0.8), so on the first lap a buff potion CAN outrank a
                                           // blow; Generate.cpp gates fortify on the worth-the-gold rule (WorthTable::floor, feat/consumables; it replaced the old kBuffFortifyFoeRatio level rule). The sim / oracle search sets the real number.
            aw(Family::Regroup, 0.8f);     aw(Family::GiveGround, 1.0f);   aw(Family::Block, 1.0f);   aw(Family::Bash, 1.0f);     // chosen: a move is not a blow, it has to be clearly worth it

            // Rapport sharpness (section 5.1, doc; ruling Q33: no emergency exemption -- the floor-gated pool
            // already makes a clearly best emergency action win at every rank).
            d.sharp = { { { 5, 0.60f, 0.12f }, { 4, 0.70f, 0.08f }, { 3, 0.80f, 0.05f }, { 2, 0.90f, 0.02f },
                          { 1, 1.00f, 0.0f } } };
            // Player focus m (section 5.2, ruling Q35): boost only.
            d.focus = { 1.00f, 1.10f, 1.20f, 1.35f, 1.50f };
            return d;
        }();
        return t;
    }

    Tuning& LiveTuning() { static Tuning t = ShippedTuning(); return t; }
    const Tuning& DefaultTuning() { return LiveTuning(); }

    bool SetTuningScalar(Tuning& a_t, std::string_view a_name, float a_v, std::string* a_why) {
        struct Scalar { const char* name; float Tuning::* m; };
        static constexpr Scalar kScalars[] = {   // the sim's kTuneScalars (tools/iaus_sim/Tune.h), same names
            { "t.momentum", &Tuning::momentum }, { "t.postureMomentum", &Tuning::postureMomentum }, { "t.preempt", &Tuning::preemptMargin }, { "t.actFloor", &Tuning::actFloor },
            { "t.softCd", &Tuning::softCooldownSec }, { "t.occHold", &Tuning::occlusionHoldSec }, { "t.kite", &Tuning::kiteDistance }, { "t.blockBase", &Tuning::blockBase },
            { "t.tauntCd", &Tuning::rowlessTauntCooldownSec }, { "t.powerCd", &Tuning::rowlessPowerCooldownSec }, { "t.disengageCd", &Tuning::disengageCooldownSec },
            { "t.noHealDrink", &Tuning::noHealDrink }, { "t.noHealRetreat", &Tuning::noHealRetreat }, { "t.emergencyHpHigh", &Tuning::emergencyHpHigh }, { "t.emergencyHpLow", &Tuning::emergencyHpLow },
            { "t.offClassBase", &Tuning::offClassBase }, { "t.groundCd", &Tuning::groundCooldownSec }, { "t.blockCd", &Tuning::blockCooldownSec }, { "t.bashCd", &Tuning::bashCooldownSec },
            { "t.backOffCd", &Tuning::backOffCooldownSec }, { "t.stepCd", &Tuning::stepCooldownSec }, { "t.regroupCd", &Tuning::regroupCooldownSec },
        };
        if (!std::isfinite(a_v)) { if (a_why) *a_why = "not a finite number"; return false; }
        for (const Scalar& k : kScalars) if (a_name == k.name) {
            // every one is a multiplier, margin, line or a cooldown / duration in seconds: negative is never valid; the two HP lines are fractions
            const bool hpLine = a_name == "t.emergencyHpHigh" || a_name == "t.emergencyHpLow";
            if (a_v < 0.0f || (hpLine && a_v > 1.0f)) { if (a_why) *a_why = hpLine ? "out of range: must be 0..1" : "out of range: must be >= 0"; return false; }
            a_t.*(k.m) = a_v; return true;
        }
        if (a_why) *a_why = "unknown t.<name> scalar";
        return false;
    }

    namespace detail {

        namespace {
            using U = Use;
            constexpr U kAttack[] = {
                { Cons::RangeChase, 1 }, { Cons::RangeSpell, 1 }, { Cons::Los, 1 }, { Cons::FoeThreat, 1 },
                { Cons::BoostCaster, 1 }, { Cons::FoeEngaged, 1 },
                { Cons::PlayerTarget, 1 }, { Cons::CurrentTarget, 1.5f }, { Cons::FoeFinish, 0.7f },
                { Cons::CooldownHard, 1 }, { Cons::ClassFit, 1 }, { Cons::BoostControlled, 1 }, { Cons::BoostStrike, 1 },
            };
            constexpr U kPowerAttack[] = {
                { Cons::RangeMelee, 1 }, { Cons::RangeSpell, 1 }, { Cons::Los, 1 }, { Cons::FoeThreat, 1 },
                { Cons::BoostCaster, 1 }, { Cons::FoeEngaged, 1 },
                { Cons::PlayerTarget, 1 }, { Cons::CurrentTarget, 1.5f }, { Cons::FoeFinish, 0.7f },
                { Cons::CooldownHard, 1 }, { Cons::ClassFit, 1 },
                { Cons::FoeBlocking, 1 }, { Cons::SelfStamina, 1 }, { Cons::FoeStronger, 1 }, { Cons::BoostControlled, 1 }, { Cons::BoostStrike, 1 },
            };
            constexpr U kTaunt[] = {
                { Cons::RangeTaunt, 1 }, { Cons::Los, 1 }, { Cons::FoeOnAlly, 2 }, { Cons::FoeOnPlayer, 2 },
                { Cons::TauntSelfHp, 1 }, { Cons::FoeTaunted, 1 }, { Cons::CooldownHard, 1 }, { Cons::ClassFit, 1 },
                { Cons::TauntSurvive, 1.995f },   /* set A2 */ { Cons::BoostPlayerDying, 1 }, { Cons::TauntChance, 1 }, { Cons::MortalTaunt, 1 },
            };
            constexpr U kGiveGround[] = { { Cons::GroundNeed, 1 }, { Cons::CooldownHard, 1 } };
            constexpr U kDisengage[] = { { Cons::DisengageClass, 1 }, { Cons::DisengageNeed, 1 }, { Cons::CooldownHard, 1 } };   // card G3
            constexpr U kBackOff[] = { { Cons::BackOffNeed, 1 }, { Cons::CooldownHard, 1 } };   // batch I (iaus/Reposition.h fills the need)
            // batch I item 5: the hold row's own gates (hidden from every listed foe, no blow ready) + its pacing; the step is generated
            // only when a point is known, so the row it outranks is the one that stands still.
            constexpr U kStep[] = { { Cons::Unseen, 1 }, { Cons::HoldStill, 1 }, { Cons::ClassFit, 1 }, { Cons::CooldownHard, 1 } };
            constexpr U kBlock[] = { { Cons::BlockNeed, 1 }, { Cons::BoostGuardLine, 1 }, { Cons::CooldownHard, 1 } };
            constexpr U kBash[] = { { Cons::RangeMelee, 1 }, { Cons::Los, 1 }, { Cons::BashNeed, 1 }, { Cons::SelfStamina, 1 }, { Cons::CooldownHard, 1 }, { Cons::FoeThreat, 1 } };
            constexpr U kRegroup[] = { { Cons::RegroupNeed, 1 }, { Cons::RegroupFar, 1 }, { Cons::CooldownHard, 1 } };
            constexpr U kSneak[] = { { Cons::Unseen, 1 }, { Cons::HoldStill, 1 }, { Cons::ClassFit, 1 } };   // G2 Layer 1: the hold row (Generate.cpp Holds)
            constexpr U kRetreat[] = { { Cons::RetreatDanger, 1 }, { Cons::NoHealRetreat, 1 }, { Cons::DisengageClass, 1 }, { Cons::BoostMortalDanger, 1 } };
            constexpr U kDrinkHealth[] = {
                { Cons::DrinkNeed, 1 }, { Cons::Stock, 1 }, { Cons::NoHealDrink, 1 }, { Cons::CooldownHard, 1 },
                { Cons::BoostMortalDanger, 1 }, { Cons::BoostFleeHeal, 1 }, { Cons::ConsumableWorth, 1 },
            };
            // feat/buff-potions: ONE family, one candidate per carried buff kind (Facts::buffKind); a consideration that does not apply to the kind
            // is not applicable (Input returns Off), so a resist potion is scored by buff_threat alone, a fortify by fight_start, a regen by need,
            // an invisibility by buff_stealth. already_active, stock and the hard cooldown apply to all.
            constexpr U kDrinkBuff[] = {
                { Cons::BuffThreat, 1 }, { Cons::FightStart, 1 }, { Cons::BuffStealth, 1 }, { Cons::DrinkNeed, 1 },
                { Cons::AlreadyActive, 1 }, { Cons::Stock, 1 }, { Cons::CooldownHard, 1 }, { Cons::ConsumableWorth, 1 },
            };
            constexpr U kDrinkOther[] = { { Cons::DrinkNeed, 1 }, { Cons::Stock, 1 }, { Cons::CooldownHard, 1 }, { Cons::MagickaBuys, 1 }, { Cons::ConsumableWorth, 1 } };
            constexpr U kPosture[] = {
                { Cons::PostureRanged, 1 }, { Cons::PostureMelee, 1 }, { Cons::Ammo, 1 }, { Cons::StaffCharge, 1 },
                { Cons::MeleeReach, 1 }, { Cons::Unholy, 1 }, { Cons::MundaneVsUnholy, 1 }, { Cons::ClassWeapon, 1 },
                { Cons::PostureTrade, 1 }, { Cons::BoostPosturePerks, 1 },
            };
            constexpr U kOffense[] = {
                { Cons::RangeSpell, 1 }, { Cons::Los, 1 }, { Cons::HandFree, 1 }, { Cons::CooldownHard, 1 },
                { Cons::CooldownSoft, 1 }, { Cons::FoeThreat, 1 }, { Cons::BoostCaster, 1 }, { Cons::FoeOnPlayer, 1 },
                { Cons::PlayerTarget, 1 }, { Cons::CurrentTarget, 1 }, { Cons::Exploit, 1.3f },
                { Cons::Affordable, 1 }, { Cons::MpReserve, 0.5f }, { Cons::SplashSafe, 1 }, { Cons::AreaValue, 1 },
                { Cons::Variety, 0.5f }, { Cons::WillWork, 1 }, { Cons::TargetCond, 1 }, { Cons::ClassFit, 1 },
                { Cons::DamageValue, 0.9975f }, { Cons::Overkill, 0.931f },   // set A2
            };
            constexpr U kExploit[] = {
                { Cons::RangeSpell, 1 }, { Cons::Los, 1 }, { Cons::HandFree, 1 }, { Cons::CooldownHard, 1 },
                { Cons::CooldownSoft, 1 }, { Cons::FoeThreat, 1 }, { Cons::BoostCaster, 1 }, { Cons::FoeOnPlayer, 1 },
                { Cons::PlayerTarget, 1 }, { Cons::CurrentTarget, 1 }, { Cons::Exploit, 2.0f },
                { Cons::Affordable, 1 }, { Cons::MpReserve, 0.5f }, { Cons::SplashSafe, 1 }, { Cons::AreaValue, 1 },
                { Cons::Variety, 0.5f }, { Cons::WillWork, 1 }, { Cons::TargetCond, 1 }, { Cons::ClassFit, 1 },
            };
            constexpr U kControl[] = {
                { Cons::RangeSpell, 1 }, { Cons::Los, 1 }, { Cons::HandFree, 1 }, { Cons::CooldownHard, 1 },
                { Cons::CooldownSoft, 1 }, { Cons::FoeThreat, 1.5f }, { Cons::BoostCaster, 1 },
                { Cons::FoeControlled, 1 }, { Cons::WillWork, 1 }, { Cons::TargetCond, 1 },
                { Cons::FoeCountControl, 1 }, { Cons::Affordable, 1 }, { Cons::MpReserve, 0.5f }, { Cons::ClassFit, 1 },
                { Cons::ControlWorth, 1 },
            };
            constexpr U kHeal[] = {
                { Cons::HealReach, 1 }, { Cons::Los, 1 }, { Cons::HandFree, 1 }, { Cons::CooldownHard, 1 },
                { Cons::HealNeed, 1 }, { Cons::BoostDowned, 1 }, { Cons::BoostFleeHeal, 1 }, { Cons::AlreadyActive, 1 }, { Cons::Affordable, 1 },
                { Cons::MpReserve, 0.5f }, { Cons::ClassFit, 1 },
                { Cons::HealSize, 0.8f }, { Cons::BoostRecipientDying, 1 }, { Cons::HealClaimed, 1 }, { Cons::HealUrgency, 0.7f },
            };
            constexpr U kBuff[] = {
                { Cons::HealReach, 1 }, { Cons::HandFree, 1 }, { Cons::CooldownHard, 1 }, { Cons::AlreadyActive, 1 },
                { Cons::BuffEarly, 1 }, { Cons::BuffMelee, 1 }, { Cons::Affordable, 1 }, { Cons::MpReserve, 0.5f },
                { Cons::ClassFit, 1 }, { Cons::BoostBuffThreat, 1 },
            };
            constexpr U kSummon[] = {
                { Cons::HandFree, 1 }, { Cons::CooldownHard, 1 }, { Cons::CooldownSoft, 1 }, { Cons::SummonLive, 1 },
                { Cons::SummonFoes, 1 }, { Cons::BoostOutnumbered, 1 }, { Cons::Affordable, 1 },
                { Cons::MpReserve, 0.5f }, { Cons::ClassFit, 1 },
            };
            constexpr U kWard[] = {
                { Cons::HandFree, 1 }, { Cons::CooldownHard, 1 }, { Cons::WardCaster, 1 }, { Cons::WardHp, 1 },
                { Cons::AlreadyActive, 1 }, { Cons::Affordable, 1 }, { Cons::MpReserve, 0.5f }, { Cons::ClassFit, 1 },
            };
            // Scrolls: the spell family's set without the magicka terms (a scroll costs none), plus the stock
            // gate, scroll_stock and mp_low (section 3.2).
            constexpr U kScrollOffense[] = {
                { Cons::RangeSpell, 1 }, { Cons::Los, 1 }, { Cons::HandFree, 1 }, { Cons::CooldownHard, 1 },
                { Cons::CooldownSoft, 1 }, { Cons::FoeThreat, 1 }, { Cons::BoostCaster, 1 }, { Cons::FoeOnPlayer, 1 },
                { Cons::PlayerTarget, 1 }, { Cons::CurrentTarget, 1 }, { Cons::Exploit, 1.3f },
                { Cons::SplashSafe, 1 }, { Cons::AreaValue, 1 }, { Cons::Variety, 0.5f }, { Cons::WillWork, 1 },
                { Cons::TargetCond, 1 }, { Cons::ClassFit, 1 },
                { Cons::Stock, 1 }, { Cons::ScrollStock, 1 }, { Cons::ScrollMpLow, 1 },
            };
            constexpr U kScrollHeal[] = {
                { Cons::HealReach, 1 }, { Cons::Los, 1 }, { Cons::HandFree, 1 }, { Cons::CooldownHard, 1 },
                { Cons::HealNeed, 1 }, { Cons::BoostDowned, 1 }, { Cons::BoostFleeHeal, 1 }, { Cons::AlreadyActive, 1 }, { Cons::ClassFit, 1 },
                { Cons::Stock, 1 }, { Cons::ScrollStock, 1 }, { Cons::ScrollMpLow, 1 },
            };
            constexpr U kScrollBuff[] = {
                { Cons::HealReach, 1 }, { Cons::HandFree, 1 }, { Cons::CooldownHard, 1 }, { Cons::AlreadyActive, 1 },
                { Cons::BuffEarly, 1 }, { Cons::BuffMelee, 1 }, { Cons::ClassFit, 1 },
                { Cons::Stock, 1 }, { Cons::ScrollStock, 1 }, { Cons::ScrollMpLow, 1 },
            };
        }

        std::span<const Use> SetFor(Family f, Family scrollOf) {
            switch (f) {
            case Family::Attack:       return kAttack;
            case Family::PowerAttack:  return kPowerAttack;
            case Family::Taunt:        return kTaunt;
            case Family::Sneak:        return kSneak;
            case Family::Retreat:      return kRetreat;
            case Family::DrinkHealth:  return kDrinkHealth;
            case Family::DrinkStamina:
            case Family::DrinkMagicka: return kDrinkOther;
            case Family::DrinkBuff:    return kDrinkBuff;
            case Family::Posture:      return kPosture;
            case Family::CastOffense:  return kOffense;
            case Family::CastControl:  return kControl;
            case Family::CastHeal:     return kHeal;
            case Family::CastBuff:     return kBuff;
            case Family::CastSummon:   return kSummon;
            case Family::CastWard:     return kWard;
            case Family::Exploit:      return kExploit;
            case Family::Regroup:      return kRegroup;
            case Family::GiveGround:   return kGiveGround;
            case Family::Block:        return kBlock;
            case Family::Bash:         return kBash;
            case Family::Disengage:    return kDisengage;
            case Family::BackOff:      return kBackOff;
            case Family::Step:         return kStep;
            case Family::Scroll:
                if (scrollOf == Family::CastHeal) return kScrollHeal;
                if (scrollOf == Family::CastBuff) return kScrollBuff;
                return kScrollOffense;
            case Family::kCount:       break;
            }
            return {};
        }

        bool PlayerRelevant(const Candidate& c) {
            const bool support = c.family == Family::CastHeal || c.family == Family::CastBuff ||
                                 (c.family == Family::Scroll &&
                                  (c.scrollOf == Family::CastHeal || c.scrollOf == Family::CastBuff));
            return (support && c.f.recipientIsPlayer) || c.f.foeOnPlayer || c.f.playerTarget;
        }

        // The retreat's danger (RetreatDanger) as one number: shared by the Retreat family and by card G3's non-sneak flee need.
        float RetreatDangerOf(const Facts& f, const Tuning& t) {
            // The 30% health gate as it was (1 - hp^3 under 0.30, nothing above), or seconds to live under 8.
            const float hp = Clamp(f.selfHp, 0.0f, 1.0f);
            const float byHp = hp <= 0.30f ? 1.0f - hp * hp * hp : 0.0f;
            const float byTtl = std::isfinite(f.selfTtl) ? Clamp(1.0f - f.selfTtl / 8.0f, 0.0f, 1.0f) : 0.0f;
            // KITE: he fights at range and a foe that has to close is nearly on him. Distance alone is not danger (a
            // skeever is no reason to stop shooting): it counts by how little he could afford the trade, 0 when he would
            // live 20 s and more with the foes that target him on him, all of it under 8 s. Unknown = no kite.
            float byKite = 0.0f;
            if (t.kiteDistance > 0.0f && f.rangedDrawn && std::isfinite(f.meleeThreatDist) && std::isfinite(f.threatTtl))
                byKite = Clamp(1.0f - f.meleeThreatDist / t.kiteDistance, 0.0f, 1.0f) * Clamp((20.0f - f.threatTtl) / 12.0f, 0.0f, 1.0f);
            return (std::max)((std::max)(byHp, byTtl), byKite);
        }

        In Input(Cons k, const Candidate& c, const Mind& m, float bestHeal, const Tuning& t) {
            const Facts& f = c.f;
            const bool melee = f.posture == PostureKind::Melee || f.posture == PostureKind::Silver;
            switch (k) {
            case Cons::RangeMelee:    return (f.reach > 0.0f && f.meleeRange) ? f.dist / f.reach : In::Off();
            case Cons::RangeSpell:    return (f.reach > 0.0f && !f.meleeRange) ? f.dist / f.reach : In::Off();
            case Cons::RangeTaunt:    return (f.reach > 0.0f && c.family == Family::Taunt) ? f.dist / f.reach : In::Off();
            case Cons::Los:
                switch (f.sight) {
                case Sight::None:     return In::Off();
                case Sight::Visible:  return 1.0f;
                case Sight::Unknown:  return 0.5f;
                case Sight::Occluded:
                    // KEEP THE CURRENT ATTACK (marth Q93): a running attack on the foe he is already on is not
                    // dropped by a short occlusion. Bounded by Tuning::occlusionHoldSec. f.occludedSec is the age of the
                    // ray f.sight reads (own or engine, Generate's Foe), and 0 = untracked, which takes no hold.
                    if ((c.family == Family::Attack || c.family == Family::PowerAttack) && f.currentTarget &&
                        t.occlusionHoldSec > 0.0f && f.occludedSec > 0.0f && f.occludedSec < t.occlusionHoldSec)
                        return 1.0f;
                    return 0.0f;
                }
                return In::Off();
            case Cons::FoeThreat:     return Clamp(f.foeLevelRatio, 0.0f, 2.0f) / 2.0f;
            case Cons::FoeOnMe:       return B(f.foeOnMe);
            case Cons::FoeOnPlayer:   return B(f.foeOnPlayer);
            case Cons::FoeOnAlly:     return B(f.foeOnAlly);
            case Cons::PlayerTarget:  return B(f.playerTarget);
            case Cons::CurrentTarget: return B(f.currentTarget);
            case Cons::FoeFinish:     return Clamp(f.foeHpPct, 0.0f, 1.0f);
            case Cons::Affordable:
            case Cons::MpReserve:
                if (!(f.cost > 0.0f) || !(f.magickaMax > 0.0f)) return In::Off();
                return (f.magicka - f.cost) / f.magickaMax;
            case Cons::CooldownHard:
                if (!(f.hardCooldownSec > 0.0f)) return In::Off();
                return f.hardCooldownLeft > 0.0f ? 0.0f : 1.0f;
            case Cons::CooldownSoft:
                if (c.form == 0 || !(t.softCooldownSec > 0.0f)) return In::Off();
                return (std::min)(1.0f, (std::max)(0.0f, f.sinceFired) / t.softCooldownSec);
            case Cons::Variety: {
                if (c.form == 0) return In::Off();
                int same = 0;
                for (std::uint8_t i = 0; i < m.recentN; ++i)
                    if (m.recent[i] == c.form) ++same;
                return static_cast<float>(same) / 4.0f;
            }
            case Cons::WillWork:      return f.willWork == Tri::No ? 0.0f : 1.0f;
            case Cons::TargetCond:    return f.targetCond == Tri::No ? 0.0f : 1.0f;
            case Cons::AlreadyActive: return B(!f.alreadyActive);
            case Cons::HandFree:      return B(f.handFree);
            case Cons::SplashSafe:    return f.hasArea ? B(f.splashSafe) : In::Off();
            case Cons::Exploit:       return f.resist;
            case Cons::ClassFit:
                // Ruling Q34. On-class: not applicable (no shading), unless the strict reading is on and this is
                // an emergency and the class is not mastered. Off-class: only a MASTERED class's ability is valid
                // at all; it scores low, and an emergency lifts it (capped).
                if (f.onClass) {
                    if (t.emergencyOnlyMastered && !f.fromMastered && f.emergency >= t.emergencyStrictAt) return 0.0f;
                    return In::Off();
                }
                if (!f.fromMastered) return 0.0f;
                return (std::min)(t.offClassCap, t.offClassBase + t.offClassEmergencyLift * Clamp(f.emergency, 0.0f, 1.0f));
            case Cons::FoeBlocking:   return B(f.foeBlocking);
            case Cons::SelfStamina:   return f.selfStamina;
            case Cons::FoeStronger:   return B(f.foeStronger);
            case Cons::FoeTaunted:    return B(!f.foeTaunted);
            case Cons::TauntSelfHp:   return std::isfinite(f.tauntTtl) ? In::Off() : In(f.selfHp);   // superseded by survive once measured
            case Cons::Unseen:
                if (f.unseen == Tri::Unknown) return In::Off();
                return B(f.unseen == Tri::Yes);
            case Cons::HoldStill:     return (c.family == Family::Sneak || c.family == Family::Step) ? B(!f.strikeReady) : In::Off();
            case Cons::BoostStrike:   return B(f.selfSneaking && f.foeUnaware == Tri::Yes);
            case Cons::FightStart:
                if (c.family == Family::DrinkBuff && !f.buffOpener) return In::Off();   // a fortify only; the resist / regen / stealth kinds are not openers
                return (std::max)(0.0f, f.combatSec) / 6.0f;
            case Cons::RetreatHp:     return f.selfHp;
            case Cons::NoHealRetreat: return bestHeal > t.noHealRetreat ? 0.0f : 1.0f;
            case Cons::DisengageClass: return B(f.disengageClass);
            case Cons::DisengageNeed:
                if (c.family != Family::Disengage) return In::Off();
                return f.disengageFlee ? RetreatDangerOf(f, t) : Clamp(f.detectShare, 0.0f, 1.0f);
            case Cons::BoostFleeHeal: return B(f.fleeHeal);
            case Cons::BoostGuardLine: return c.family == Family::Block ? In(B(f.tauntedOnMe >= kGuardLineFoes)) : In::Off();
            case Cons::BuffThreat:    return (c.family == Family::DrinkBuff && f.buffThreat >= 0.0f) ? In(Clamp(f.buffThreat, 0.0f, 1.0f)) : In::Off();
            case Cons::BuffStealth:   return (c.family == Family::DrinkBuff && f.buffStealth != Tri::Unknown) ? In(B(f.buffStealth == Tri::Yes)) : In::Off();
            case Cons::BoostPosturePerks: return c.family == Family::Posture && f.perkShare >= 0.0f ? In(f.perkShare) : In::Off();
            case Cons::BoostBuffThreat: return c.family == Family::CastBuff ? In(static_cast<float>((std::min)((std::max)(f.foeCount, 0), 4)) / 4.0f) : In::Off();
            case Cons::DrinkNeed:
                if (c.family == Family::DrinkBuff) return f.buffRegenPct >= 0.0f ? In(Clamp(f.buffRegenPct, 0.0f, 1.0f)) : In::Off();   // a regen kind reads its resource's pct
                if (c.family == Family::DrinkStamina) return f.selfStamina;
                if (c.family == Family::DrinkMagicka) return f.selfMagicka;
                return f.selfHp;
            case Cons::ConsumableWorth: return f.consumableWorth >= 0.0f ? In(Clamp(f.consumableWorth, 0.0f, 1.0f)) : In::Off();
            case Cons::Stock:         return f.stock < 0 ? In::Off() : In(B(f.stock > 0));
            case Cons::NoHealDrink:   return bestHeal > t.noHealDrink ? 0.0f : 1.0f;
            case Cons::PostureRanged:
                return (f.posture == PostureKind::Bow || f.posture == PostureKind::Staff) ? f.dist / kPostureDist : In::Off();
            case Cons::PostureMelee:  return melee ? f.dist / kPostureDist : In::Off();
            case Cons::Ammo:          return f.posture == PostureKind::Bow ? B(!f.outOfAmmo) : In::Off();
            case Cons::StaffCharge:   return f.posture == PostureKind::Staff ? B(f.staffCharged) : In::Off();
            case Cons::MeleeReach:    return melee ? B(!f.foeOutOfMeleeReach) : In::Off();
            case Cons::Unholy:        return f.posture == PostureKind::Silver ? B(f.foeUnholy) : In::Off();
            case Cons::MundaneVsUnholy: return f.posture == PostureKind::Melee ? B(f.foeUnholy) : In::Off();
            case Cons::ClassWeapon:   return B(f.classAllowsWeapon);
            case Cons::AreaValue:     return f.hasArea ? (std::max)(0.0f, f.foesInArea) / 3.0f : In::Off();
            case Cons::FoeControlled: return B(!f.foeControlled);
            case Cons::FoeCountControl: return B(f.foeCount >= 2);
            case Cons::HealNeed:      return f.recipientHp;
            case Cons::HealReach:     return f.inHealReach == Tri::No ? 0.0f : 1.0f;
            case Cons::BuffEarly:     return (std::max)(0.0f, f.combatSec) / 20.0f;
            case Cons::BuffMelee:     return f.meleeBuff ? B(f.recipientInMelee) : In::Off();
            case Cons::SummonLive:    return B(!f.liveSummon);
            case Cons::SummonFoes:    return static_cast<float>((std::min)((std::max)(f.foeCount, 0), 4)) / 4.0f;
            case Cons::WardCaster:    return B(f.casterOnMe);
            case Cons::WardHp:        return f.selfHp;
            case Cons::ScrollStock:   return f.stock < 0 ? In::Off() : In(static_cast<float>(f.stock) / 5.0f);
            case Cons::ScrollMpLow:   return f.selfMagicka;
            case Cons::FoeEngaged:
                // One foe has ONE combat target, so on_player / on_me / on_ally are exclusive: coded 1 / 2/3 / 1/3,
                // 0 = it targets none of us.
                if (f.foeOnPlayer) return 1.0f;
                if (f.foeOnMe) return 2.0f / 3.0f;
                if (f.foeOnAlly) return 1.0f / 3.0f;
                return 0.0f;
            case Cons::BoostCaster:   return B(f.foeCaster);
            case Cons::BoostDowned:   return B(f.recipientDowned);
            case Cons::BoostOutnumbered: return B(f.foeCount > f.partyCount);
            case Cons::DamageValue:
                // Not applicable only while NOTHING he can cast has a measured damage on this target. Once his best is known,
                // a spell that does no damage (a noise, a debuff) is 0 against it: unmeasured must not read as "as good as any".
                // MFO-B684: a spell whose own damage was NOT measured (expDamage < 0) is not applicable either. Only a
                // MEASURED 0 scores 0.
                if (!(f.bestDamage > 0.0f) || f.expDamage < 0.0f) return In::Off();
                return Clamp(f.expDamage / f.bestDamage, 0.0f, 1.0f);
            case Cons::Overkill:
                if (!(f.expDamage > 0.0f) || !(f.foeHealth > 0.0f)) return In::Off();
                return Clamp(f.foeHealth / f.expDamage, 0.0f, 1.0f);
            case Cons::TauntSurvive:
                if (!std::isfinite(f.tauntTtl)) return In::Off();
                return Clamp(f.tauntTtl / 12.0f, 0.0f, 1.0f);
            case Cons::RetreatDanger: return RetreatDangerOf(f, t);
            case Cons::HealSize:
                if (!(f.expHeal > 0.0f)) return In::Off();
                return Clamp(f.recipientMissing / f.expHeal, 0.0f, 1.0f);
            case Cons::RangeChase:
                if (!f.meleeRange) return In::Off();
                if (!(f.chase > 0.0f)) return (f.reach > 0.0f) ? In(f.dist / (f.reach * 20.0f)) : In::Off();   // no cap known: 20 reaches
                return f.dist / f.chase;
            case Cons::BoostControlled:     return B(f.foeHelpless);
            case Cons::TauntChance:         return f.tauntChance < 0.0f ? In::Off() : In(Clamp(f.tauntChance, 0.0f, 1.0f));
            // Off when nobody else is healing him: an input that is on counts toward n (the I4 rule), and a
            // constant 1.0 on every heal raised each heal's compensation (review of 303c14d, F4).
            case Cons::HealClaimed:         return f.recipientBeingHealed ? In(1.0f) : In::Off();
            case Cons::HealUrgency:         return std::isfinite(f.recipientTtl) ? In(Clamp(f.recipientTtl / 10.0f, 0.0f, 1.0f)) : In::Off();
            case Cons::GroundNeed: {
                if (c.family != Family::GiveGround) return In::Off();
                // BEATEN: what is hitting him now leaves him seconds (6 s and more = no need).
                float need = std::isfinite(f.selfTtl) ? Clamp(1.0f - f.selfTtl / 6.0f, 0.0f, 1.0f) : 0.0f;
                // KITE: he fights at range, a foe that has to close is inside 450 units, and he could not afford it on him.
                if (f.rangedDrawn && std::isfinite(f.meleeThreatDist) && std::isfinite(f.threatTtl))
                    need = (std::max)(need, Clamp(1.0f - f.meleeThreatDist / 450.0f, 0.0f, 1.0f) * Clamp((20.0f - f.threatTtl) / 12.0f, 0.0f, 1.0f));
                return need;
            }
            case Cons::BackOffNeed:
                return c.family == Family::BackOff ? In(Clamp(f.backOffNeed, 0.0f, 1.0f)) : In::Off();
            case Cons::BlockNeed:
                if (c.family != Family::Block) return In::Off();
                // Q-S-5: two or more taunted foes on the line = the full need whatever the time-to-live estimate says (it is unknown by default).
                if (f.tauntedOnMe >= kGuardLineFoes) return 1.0f;
                if (!f.foeSwinging) return In(0.0f);
                // A swing is in the air: half the need for that alone, the rest by how few seconds he has.
                return t.blockBase + (1.0f - t.blockBase) * (std::isfinite(f.selfTtl) ? Clamp(1.0f - f.selfTtl / 8.0f, 0.0f, 1.0f) : 0.0f);
            case Cons::BashNeed:
                if (c.family != Family::Bash) return In::Off();
                return f.foeCasting ? 1.0f : (f.foeOnMe ? 0.4f : 0.15f);
            case Cons::RegroupNeed: {
                if (c.family != Family::Regroup) return In::Off();
                float need = 0.0f;
                // PEEL: the player is being hit and has seconds left (10 s = no need, 0 s = all of it)
                if (f.recipientIsPlayer && std::isfinite(f.recipientTtl)) need = (std::max)(need, Clamp(1.0f - f.recipientTtl / 10.0f, 0.0f, 1.0f));
                // FALL BACK: he has seconds left himself and the anchor stands away from what is hitting him
                if (f.anchorSafer && std::isfinite(f.selfTtl)) need = (std::max)(need, Clamp(1.0f - f.selfTtl / 8.0f, 0.0f, 1.0f));
                // TO THE HEALER: he is hurt (under 60 percent) and the anchor heals
                if (f.anchorHealer) need = (std::max)(need, Clamp((0.6f - Clamp(f.selfHp, 0.0f, 1.0f)) / 0.6f, 0.0f, 1.0f));
                return need;
            }
            case Cons::RegroupFar:    return c.family == Family::Regroup ? In(f.dist / 1000.0f) : In::Off();
            case Cons::PostureTrade:
                if (!melee || !std::isfinite(f.selfTtl)) return In::Off();
                return Clamp(f.selfTtl / 10.0f, 0.0f, 1.0f);
            case Cons::MortalTaunt:
                if (!f.mortal || !std::isfinite(f.tauntTtl)) return In::Off();
                return Clamp(f.tauntTtl / 12.0f, 0.0f, 1.0f);
            case Cons::BoostMortalDanger: return B(f.mortal);
            case Cons::ControlWorth:
                if (!(f.controlSec > 0.0f)) return In::Off();
                return Clamp(f.controlSec / 10.0f, 0.0f, 1.0f) * (0.4f + 0.6f * Clamp(f.foeHpPct, 0.0f, 1.0f));
            case Cons::MagickaBuys:
                if (c.family != Family::DrinkMagicka || f.castsWanted < 0.0f) return In::Off();
                return Clamp(f.castsWanted / 3.0f, 0.0f, 1.0f);
            case Cons::BoostPlayerDying:    return B(std::isfinite(f.playerTtl) && f.playerTtl < 5.0f);
            case Cons::BoostRecipientDying: return B(std::isfinite(f.recipientTtl) && f.recipientTtl < 5.0f);
            case Cons::kCount:        break;
            }
            return In::Off();
        }

    }

}
