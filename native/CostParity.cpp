#include "PCH.h"
#include "CostParity.h"
#include "Config.h"
#include "Followers.h"
#include "State.h"
#include <cmath>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace MFO::CostParity {

    namespace {
        using Clock = std::chrono::steady_clock;

        struct Curve { float base = 0, scale = 1, mult = 1; };
        std::atomic<bool> g_active{ false };
        Curve             g_pc, g_np;   // written once at kDataLoaded, read-only afterwards

        // The engine's skill cost curve, as disassembled (AE 1.6.1170 id 26505 @0x41AA40):
        //   (1 - powf(Base * skill, Scale)) * Mult      applied to the cost, NO clamp (the engine clamps the final cost at 0)
        // The skill is the school's actor value truncated to an int and floored at 0 (0x14FF30 / 0x14C2xx: cvttss2si, cmovg).
        float Factor(const Curve& c, int a_skill) {
            return (1.0f - std::pow(c.base * static_cast<float>(a_skill < 0 ? 0 : a_skill), c.scale)) * c.mult;
        }

        // Leaf mutex over everything below.
        std::mutex g_mx;
        // (caster << 32 | spell) -> when MFO's own direct deduct last billed it.
        std::unordered_map<std::uint64_t, Clock::time_point> g_directBill;
        // Rate limit for the per-key log line.
        std::unordered_map<std::uint64_t, Clock::time_point> g_lastLog;

        struct Channel {
            RE::FormID  caster = 0, spell = 0;
            Clock::time_point paidThrough, lastSeen;
        };
        std::vector<Channel> g_channels;

        constexpr auto kDedupeWindow = std::chrono::seconds(3);   // a direct bill, then the sink event for the same cast (ms apart)
        constexpr auto kLogEvery     = std::chrono::seconds(5);

        std::uint64_t Key(RE::FormID c, RE::FormID s) { return (static_cast<std::uint64_t>(c) << 32) | s; }

        struct Parity {
            float engine = 0, parity = 0, skill = 0, playerF = 0, npcF = 0;
            RE::ActorValue school = RE::ActorValue::kNone;
        };

        // EP 0x26 (kModSpellCost) is HandleEntryPoint(ep, owner, spell, &cost): the engine's own call in CalculateCost
        // (id 11321 @0x14C1F0) is `mov ecx,0x26; mov rdx,<caster>; mov r8,<spell>; lea r9,[cost]; call 0x140385F30` (0x14C2B8-0x14C2C8).
        // Two object arguments (owner, spell) and the value pointer: it is NOT the 4-argument EP 0x1D shape.
        float PerkStep(RE::Actor* a_caster, RE::SpellItem* a_spell, float a_value) {
            float v = a_value;
            RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModSpellCost, a_caster,
                                                static_cast<RE::MagicItem*>(a_spell), &v);
            return v;
        }

        // Compute P for (follower, spell). False = parity does not apply (the caller uses the engine cost).
        bool Compute(RE::Actor* a_caster, RE::SpellItem* a_spell, Parity& o) {
            if (!Active() || !a_caster || !a_spell) return false;
            o.engine = a_spell->CalculateMagickaCost(a_caster);
            // The engine nulls the caster for the curve on a power or lesser power (0x14C234, GetSpellType 2/3): no curve, nothing to top up.
            const auto type = a_spell->GetSpellType();
            if (type == RE::MagicSystem::SpellType::kPower || type == RE::MagicSystem::SpellType::kLesserPower) return false;
            o.school = a_spell->GetAssociatedSkill();   // vfunc 0x66 (0x330): the engine's own school getter, 0x14FF30 / the autocalc loop
            const int sv = static_cast<int>(o.school);
            if (sv < 6 || sv > 23) return false;        // the engine applies the curve only to the skill actor values 6..23
            auto* avo = a_caster->AsActorValueOwner();
            if (!avo) return false;
            const int skill = static_cast<int>(avo->GetActorValue(o.school));
            o.skill   = static_cast<float>(skill < 0 ? 0 : skill);
            o.playerF = Factor(g_pc, skill);
            o.npcF    = Factor(g_np, skill);
            // Nothing to charge when the player's curve is not the costlier one at this skill, or the NPC factor is too small to divide by.
            if (!(o.npcF > 1.0e-4f) || !(o.playerF > o.npcF + 1.0e-6f)) return false;
            if (!(o.engine > 0.0f)) return false;
            // The entry point's affine form on this caster and spell: perk(v) = a*v + b (Multiply, Add; a Set gives a = 0).
            const float p1 = PerkStep(a_caster, a_spell, 1.0f);
            const float p2 = PerkStep(a_caster, a_spell, 101.0f);
            const float a  = (p2 - p1) / 100.0f;
            const float b  = p1 - a;
            if (!(std::fabs(a) > 1.0e-6f)) return false;   // a Set-value perk makes the cost independent of the curve
            const float x = (o.engine - b) / a;             // the NPC-curved, pre-perk cost
            float P = a * (x * o.playerF / o.npcF) + b;
            if (P < 0.0f) P = 0.0f;
            if (!(P > o.engine + 1.0e-3f)) return false;
            o.parity = P;
            return true;
        }

        bool LogDue(std::uint64_t a_key) {
            std::scoped_lock lk(g_mx);
            const auto now = Clock::now();
            auto& t = g_lastLog[a_key];
            if (t != Clock::time_point{} && now - t < kLogEvery) return false;
            t = now;
            return true;
        }

        // Deduct `a_delta` magicka, never below 0. WORKER (the same domain as every direct deduct).
        float Deduct(RE::Actor* a_actor, float a_delta) {
            auto* avo = a_actor ? a_actor->AsActorValueOwner() : nullptr;
            if (!avo || !(a_delta > 0.0f)) return 0.0f;
            const float pool  = avo->GetActorValue(RE::ActorValue::kMagicka);
            const float spend = std::clamp(a_delta, 0.0f, std::max(0.0f, pool));
            if (spend > 0.0f)
                avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kMagicka, -spend);
            return spend;
        }

        // Is `a_spell` still channelling in either hand of `a_actor`?
        bool Channelling(RE::Actor* a_actor, RE::FormID a_spell) {
            for (const auto src : { RE::MagicSystem::CastingSource::kLeftHand, RE::MagicSystem::CastingSource::kRightHand }) {
                auto* mc = a_actor->GetMagicCaster(src);
                if (!mc || !mc->currentSpell || mc->currentSpell->GetFormID() != a_spell) continue;
                if (mc->state.get() == RE::MagicCaster::State::kCasting) return true;
            }
            return false;
        }
    }

    bool Active() { return g_active.load(std::memory_order_relaxed); }

    void Reset() {
        std::scoped_lock lk(g_mx);
        g_directBill.clear();
        g_lastLog.clear();
        g_channels.clear();
    }

    void StartupCheck() {
        g_active = false;
        if (!Config::g_followerSpellCostParity.load()) {
            spdlog::info("[costparity] bFollowerSpellCostParity=0 -- followers pay the engine cost unchanged");
            return;
        }
        auto* gsc = RE::GameSettingCollection::GetSingleton();
        if (!gsc) {
            spdlog::error("[costparity] GameSettingCollection unavailable -- followers pay the engine cost unchanged");
            return;
        }
        // Six GMSTs. A missing one refuses the whole feature: never run on a guessed constant.
        const char* missing = nullptr;
        auto get = [&](const char* a_name) -> float {
            auto* st = gsc->GetSetting(a_name);
            if (!st) { if (!missing) missing = a_name; return 0.0f; }
            return st->GetFloat();
        };
        Curve pc, np;
        pc.base = get("fMagicCasterPCSkillCostBase"); pc.mult = get("fMagicCasterPCSkillCostMult"); pc.scale = get("fMagicPCSkillCostScale");
        np.base = get("fMagicCasterSkillCostBase");   np.mult = get("fMagicCasterSkillCostMult");   np.scale = get("fMagicSkillCostScale");
        if (missing) {
            spdlog::error("[costparity] GMST {} not found -- the cost curves cannot be compared; followers pay the engine cost "
                          "unchanged", missing);
            return;
        }
        if (!std::isfinite(pc.base * 100.0f) || !std::isfinite(np.base * 100.0f) || pc.base < 0.0f || np.base < 0.0f) {
            spdlog::error("[costparity] a Base GMST is negative or not finite (player {}, NPC {}) -- followers pay the engine "
                          "cost unchanged", pc.base, np.base);
            return;
        }
        g_pc = pc; g_np = np;
        spdlog::info("[costparity] player curve: Base {:.4f} Scale {:.3f} Mult {:.3f}; NPC curve: Base {:.4f} Scale {:.3f} Mult {:.3f}",
                     pc.base, pc.scale, pc.mult, np.base, np.scale, np.mult);
        for (const int sk : { 0, 25, 50, 75, 100 }) {
            const float pf = Factor(pc, sk), nf = Factor(np, sk);
            spdlog::info("[costparity] skill {:3d}: player factor {:.3f}, NPC factor {:.3f}, NPC pays {:.0f}% of the player{}",
                         sk, pf, nf, pf > 0.0f ? 100.0f * nf / pf : 0.0f,
                         pf > nf + 1.0e-6f ? "" : " (nothing to charge at this skill)");
        }
        g_active = true;
        spdlog::info("[costparity] ACTIVE: a managed follower is charged the PLAYER-curve cost (his own skill in the spell's school, "
                     "then his own cost perks) wherever the player curve is costlier; direct casts are charged it, animated casts "
                     "are topped up by the difference");
    }

    float Cost(RE::Actor* a_caster, RE::SpellItem* a_spell) {
        if (!a_caster || !a_spell) return 0.0f;
        const float engine = a_spell->CalculateMagickaCost(a_caster);
        if (!Active() || !Followers::IsTrackedFast(a_caster->GetFormID())) return engine;
        Parity p;
        const auto key = Key(a_caster->GetFormID(), a_spell->GetFormID());
        {   // MFO bills this cast itself: the sink's top-up must not bill it again (applies or not, the engine did not charge it)
            std::scoped_lock lk(g_mx);
            g_directBill[key] = Clock::now();
        }
        if (!Compute(a_caster, a_spell, p)) return engine;
        if (LogDue(key))
            spdlog::info("[costparity] {:08X} direct {} ({:08X}): skill {:.0f} in {}, player cost {:.2f}, engine cost {:.2f}, "
                         "delta {:.2f} -- charged the player cost", a_caster->GetFormID(),
                         a_spell->GetName() ? a_spell->GetName() : "?", a_spell->GetFormID(), p.skill,
                         static_cast<int>(p.school), p.parity, p.engine, p.parity - p.engine);
        return p.parity;
    }

    void OnObservedCast(RE::FormID a_casterID, RE::FormID a_spellID) {
        if (!Active()) return;
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_casterID);
        auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(a_spellID);
        if (!actor || !spell) return;
        // Managed = tracked, and MFO not switched off for him. No record (a summon or a runtime follower) counts as managed (Q41).
        if (const auto it = g_followers.find(a_casterID); it != g_followers.end() && !it->second.mfoEnabled) return;
        const auto key = Key(a_casterID, a_spellID);
        {
            std::scoped_lock lk(g_mx);
            if (const auto it = g_directBill.find(key); it != g_directBill.end() && Clock::now() - it->second < kDedupeWindow)
                return;   // MFO's direct road already billed this cast
        }
        Parity p;
        if (!Compute(actor, spell, p)) return;
        const float delta = p.parity - p.engine;
        const bool  conc  = spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
        if (conc) {
            std::scoped_lock lk(g_mx);
            for (auto& c : g_channels)
                if (c.caster == a_casterID && c.spell == a_spellID) { c.lastSeen = Clock::now(); return; }
            g_channels.push_back({ a_casterID, a_spellID, Clock::now(), Clock::now() });
            // The channel's first second is billed by the Tick that follows; this line is the start.
            spdlog::info("[costparity] {:08X} animated {} ({:08X}) concentration: skill {:.0f} in {}, player cost {:.2f}/s, engine "
                         "cost {:.2f}/s, delta {:.2f}/s -- topping up per second", a_casterID,
                         spell->GetName() ? spell->GetName() : "?", a_spellID, p.skill, static_cast<int>(p.school),
                         p.parity, p.engine, delta);
            return;
        }
        const float spent = Deduct(actor, delta);
        spdlog::info("[costparity] {:08X} animated {} ({:08X}): skill {:.0f} in {}, player cost {:.2f}, engine cost {:.2f}, delta "
                     "{:.2f} -- topped up {:.2f}", a_casterID, spell->GetName() ? spell->GetName() : "?", a_spellID, p.skill,
                     static_cast<int>(p.school), p.parity, p.engine, delta, spent);
    }

    void Tick() {
        std::vector<Channel> open;
        {
            std::scoped_lock lk(g_mx);
            if (g_channels.empty()) return;
            open = g_channels;
        }
        const auto now = Clock::now();
        std::vector<std::pair<RE::FormID, RE::FormID>> ended;
        std::vector<Channel> kept;
        for (auto c : open) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(c.caster);
            auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(c.spell);
            if (!actor || !spell) { ended.emplace_back(c.caster, c.spell); continue; }
            const bool live = Channelling(actor, c.spell);
            if (live) c.lastSeen = now;
            // Bill up to the last moment the channel was seen alive (a final bill when it has just ended).
            const float sec = std::max(0.0f, std::chrono::duration<float>(c.lastSeen - c.paidThrough).count());
            Parity p;
            if (sec > 0.0f && Compute(actor, spell, p)) {
                const float spent = Deduct(actor, (p.parity - p.engine) * sec);
                if (LogDue(Key(c.caster, c.spell) ^ 1u))
                    spdlog::info("[costparity] {:08X} animated {} ({:08X}) concentration: skill {:.0f}, player cost {:.2f}/s, engine "
                                 "cost {:.2f}/s, delta {:.2f}/s x {:.2f}s -- topped up {:.2f}", c.caster,
                                 spell->GetName() ? spell->GetName() : "?", c.spell, p.skill, p.parity, p.engine,
                                 p.parity - p.engine, sec, spent);
            }
            c.paidThrough = c.lastSeen;
            if (live) kept.push_back(c); else ended.emplace_back(c.caster, c.spell);
        }
        std::scoped_lock lk(g_mx);
        for (auto& c : g_channels) {   // fold the bookkeeping back (a channel opened meanwhile is left untouched)
            for (const auto& k : kept)
                if (k.caster == c.caster && k.spell == c.spell) { c.paidThrough = k.paidThrough; c.lastSeen = k.lastSeen; }
        }
        g_channels.erase(std::remove_if(g_channels.begin(), g_channels.end(), [&](const Channel& c) {
            for (const auto& e : ended) if (e.first == c.caster && e.second == c.spell) return true;
            return false; }), g_channels.end());
    }
}
