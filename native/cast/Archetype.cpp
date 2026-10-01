// cast/Archetype.cpp -- the SPELL ARCHETYPE CLASSIFIER and the passive [archetype] / [cast-call]
// probes. feat/mfo-spell-archetype-probe, task 7 "complex spells" (ClickUp 86e3dmtr1), slice P0.
//
// PASSIVE, NO BEHAVIOUR CHANGE. Every function here reads SPEL / MGEF record data (never an
// engine table, never an actor's live state) and writes log lines. No road, gate, outcome,
// co-save record or hook is touched. Calling it from any thread is safe: the classifier is a
// pure function of form data, its cache and the once-per-session ledgers sit behind leaf mutexes
// that are never held across a log call or an engine call.
//
// WHY (principle 5, observe before building): the engine's NPC combat AI builds a caster only
// for a spell whose highest-scoring effect has a row in a fixed 23-row table; a spell with no row
// is never cast by the AI, so an APMF claim for it stands to its TTL and nothing fires (the
// `[cfc] ... NO observed cast` shape, field datum 841D7023). This file lets the next Deck log say
// WHICH spell, of WHAT shape, with WHAT predicted row, on WHICH MFO road. The design is
// scratchpad complex-spells-research.md sections 3.1 and 6; the evidence it produces decides P2-P5.
//
// THE ENGINE-ROW MIRROR IS A PREDICTION. It is copied from Harbinger's documentation
// (/home/user/apmf Docs/STATUS.md lines 135-161, the caster-type census head of work; table at
// AE 0x20163B0 / SE 0x1DF2B00, 23 rows, identical on both). MFO cannot read the engine table or
// the effect score; Harbinger's `[ctcensus] OPEN ... predicted native[...]` line is the authority
// to check this against. Every log line labels the value `predicted=`.
//
// FORK FIELDS READ (CommonLibSSE-NG mit-3.7 fork, fde0f3a, include/RE/...):
//   SpellItem::data (SpellItem.h:46-57 `Data`): flags (kNoDualCastMods = 1<<23, :27), chargeTime
//     (:51), castingType (:52), delivery (:53), range (:55); SpellItem::IsTwoHanded (:67, vfunc
//     0x67, `equipSlot && equipSlot->flags & 1`). MagicItem::effects (MagicItem.h:126).
//   Effect::effectItem.area (Effect.h:13-22 EffectItem, :37), Effect::baseEffect (:39),
//     Effect::cost (:40).
//   EffectSetting::data (EffectSetting.h:40-98 EffectSettingData): flags kHostile (:48),
//     kSnapToNavMesh (:51); archetype (:88, +0x58), primaryAV (:89, +0x5C), projectileBase (:90,
//     +0x60), explosion (:91, +0x68), secondaryAV (:94, +0x78). EffectSetting::IsHostile (:144).
//   BGSProjectile::IsBarrier (BGSProjectile.h:111). EffectArchetypes::ArchetypeID
//     (EffectArchetypes.h:10-60). ActorValue kHealth 24, kMagicka 25, kStamina 26, kDamageResist 39,
//     kParalysis 53, kInvisibility 54, kWardPower 63 (ActorValues.h:32-34, :47, :61-62, :71).
//   MagicSystem::Delivery / CastingType (MagicSystem.h:30-47).
#include "Actuation.h"
#include "CasterConsent.h"

#include <unordered_set>

namespace MFO::Actuation {

    namespace {

        using Arch = RE::EffectArchetype;
        using AV   = RE::ActorValue;

        // One required value of the table key's `self` / `hostile` bits. `stated` records whether
        // the DOC's row names that dimension at all. An unstated dimension is matched against the
        // value the row naturally has (the `req`); an effect with the OPPOSITE value is not
        // silently treated as a miss -- the docs do not say, so the prediction is Unknown.
        enum class Req : std::uint8_t { No, Yes, Any };

        struct RowSpec {
            Arch        arch;
            int         av;          // required AV id for an AV-keyed archetype; -1 = not applicable
            Req         self;        bool selfStated;
            Req         hostile;     bool hostileStated;
            EngineRow   caster;      // NoRow = the table's "no caster (null creator)" rows
            const char* cite;        // the doc line mirrored
        };

        constexpr int AVi(AV a) { return static_cast<int>(a); }

        // The mirrored table. Source: /home/user/apmf Docs/STATUS.md at the cited lines (checkout
        // 3e6654b). Key = (archetype, AV, self, hostile) where self = SPELL delivery == Self (the
        // resolver's +0x4c, STATUS.md:135-139) and the row is that of the spell's highest-scoring
        // effect with a non-null creator (:139-141). The table is complete at 23 rows (count it:
        // 3 + 2 + 3 + 1 + 2 + 2 + 1 + 2 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 = 23 after expanding the
        // grouped doc rows), so a key that matches none of them has NO row (STATUS.md:163-165).
        // Rows with a ONE-sided entry in the doc state only what the doc states.
        constexpr RowSpec kRows[] = {
            // STATUS.md:146  ValueMod Health, other, hostile -> Offensive
            { Arch::kValueModifier, AVi(AV::kHealth),  Req::No,  true,  Req::Yes, true,  EngineRow::Offensive,  "STATUS:146" },
            // STATUS.md:147  ValueMod Magicka / Stamina, other, hostile -> no caster (null creator)
            { Arch::kValueModifier, AVi(AV::kMagicka), Req::No,  true,  Req::Yes, true,  EngineRow::NoRow,      "STATUS:147" },
            { Arch::kValueModifier, AVi(AV::kStamina), Req::No,  true,  Req::Yes, true,  EngineRow::NoRow,      "STATUS:147" },
            // STATUS.md:148  Stagger / Disarm, other, hostile
            { Arch::kStagger,       -1,                Req::No,  true,  Req::Yes, true,  EngineRow::Stagger,    "STATUS:148" },
            { Arch::kDisarm,        -1,                Req::No,  true,  Req::Yes, true,  EngineRow::Disarm,     "STATUS:148" },
            // STATUS.md:149  CommandSummoned, Banish (AV 1), TurnUndead (AV 1), hostile -> TargetEffect
            //   (the doc names no self value for these: `other` is the natural one)
            { Arch::kCommandSummoned, -1,              Req::No,  false, Req::Yes, true,  EngineRow::TargetEffect, "STATUS:149" },
            { Arch::kBanish,        1,                 Req::No,  false, Req::Yes, true,  EngineRow::TargetEffect, "STATUS:149" },
            { Arch::kTurnUndead,    1,                 Req::No,  false, Req::Yes, true,  EngineRow::TargetEffect, "STATUS:149" },
            // STATUS.md:150  Paralysis (AV Paralysis), hostile -> Paralyze (self unstated: `other`)
            { Arch::kParalysis,     AVi(AV::kParalysis), Req::No, false, Req::Yes, true, EngineRow::Paralyze,   "STATUS:150" },
            // STATUS.md:151  Script, hostile (other) or non-hostile (self) -> Script
            { Arch::kScript,        -1,                Req::No,  true,  Req::Yes, true,  EngineRow::Script,     "STATUS:151" },
            { Arch::kScript,        -1,                Req::Yes, true,  Req::No,  true,  EngineRow::Script,     "STATUS:151" },
            // STATUS.md:152  ValueMod Health / Magicka, self, beneficial -> Restore
            { Arch::kValueModifier, AVi(AV::kHealth),  Req::Yes, true,  Req::No,  true,  EngineRow::Restore,    "STATUS:152" },
            { Arch::kValueModifier, AVi(AV::kMagicka), Req::Yes, true,  Req::No,  true,  EngineRow::Restore,    "STATUS:152" },
            // STATUS.md:153  ValueMod Stamina, self, beneficial -> no caster
            { Arch::kValueModifier, AVi(AV::kStamina), Req::Yes, true,  Req::No,  true,  EngineRow::NoRow,      "STATUS:153" },
            // STATUS.md:154  ValueMod WardPower, self -> Ward (hostile unstated: non-hostile)
            { Arch::kValueModifier, AVi(AV::kWardPower), Req::Yes, true, Req::No, false, EngineRow::Ward,       "STATUS:154" },
            // STATUS.md:155  SummonCreature, self or other -> Summon (hostile unstated: non-hostile)
            { Arch::kSummonCreature, -1,               Req::Any, true,  Req::No,  false, EngineRow::Summon,     "STATUS:155" },
            // STATUS.md:156  Cloak, self -> Cloak
            { Arch::kCloak,         -1,                Req::Yes, true,  Req::No,  false, EngineRow::Cloak,      "STATUS:156" },
            // STATUS.md:157  Light, self -> Light
            { Arch::kLight,         -1,                Req::Yes, true,  Req::No,  false, EngineRow::Light,      "STATUS:157" },
            // STATUS.md:158  Invisibility (AV Invisibility), self -> Invisibility
            { Arch::kInvisibility,  AVi(AV::kInvisibility), Req::Yes, true, Req::No, false, EngineRow::Invisibility, "STATUS:158" },
            // STATUS.md:159  BoundWeapon, self -> BoundItem
            { Arch::kBoundWeapon,   -1,                Req::Yes, true,  Req::No,  false, EngineRow::BoundItem,  "STATUS:159" },
            // STATUS.md:160  ValueMod DamageResist, self -> Armor
            { Arch::kValueModifier, AVi(AV::kDamageResist), Req::Yes, true, Req::No, false, EngineRow::Armor,   "STATUS:160" },
            // STATUS.md:161  Reanimate, other -> Reanimate (hostile unstated: non-hostile)
            { Arch::kReanimate,     -1,                Req::No,  true,  Req::No,  false, EngineRow::Reanimate,  "STATUS:161" },
        };

        // The 17 archetypes whose key carries the effect's AV (STATUS.md:135-139: info flag bit 1,
        // AE 0x1FD3028 / SE 0x1DB0028); they get a SECOND lookup on the secondary AV. Every other
        // archetype keys AV 0xFF, so its rows ignore the AV here (av = -1 above).
        bool AvKeyed(Arch a) {
            switch (a) {
            case Arch::kValueModifier: case Arch::kAbsorb: case Arch::kDualValueModifier:
            case Arch::kCalm: case Arch::kDemoralize: case Arch::kFrenzy: case Arch::kInvisibility:
            case Arch::kNightEye: case Arch::kParalysis: case Arch::kTurnUndead:
            case Arch::kValueAndParts: case Arch::kAccumulateMagnitude: case Arch::kPeakValueModifier:
            case Arch::kRally: case Arch::kEnhanceWeapon: case Arch::kBanish: case Arch::kGrabActor:
                return true;
            default: return false;
            }
        }

        enum class Hit : std::uint8_t { Miss, Unsure, Match };

        struct Lookup { Hit hit = Hit::Miss; EngineRow caster = EngineRow::NoRow; const char* why = ""; };

        Lookup LookupAv(Arch a, int av, bool self, bool hostile) {
            Lookup out;
            for (const auto& r : kRows) {
                if (r.arch != a) continue;
                if (AvKeyed(a) && r.av != av) continue;
                bool selfOk = (r.self == Req::Any) || ((r.self == Req::Yes) == self);
                bool hostOk = (r.hostile == Req::Any) || ((r.hostile == Req::Yes) == hostile);
                if (selfOk && hostOk) { out = { Hit::Match, r.caster, r.cite }; return out; }
                // A mismatch on a dimension the doc does NOT state is a question, not a miss.
                const bool selfBad = !selfOk, hostBad = !hostOk;
                const bool onlyUnstatedBad = (!selfBad || !r.selfStated) && (!hostBad || !r.hostileStated);
                if (onlyUnstatedBad && out.hit != Hit::Unsure) {
                    out = { Hit::Unsure, EngineRow::Unknown,
                            selfBad ? "row's self condition unstated in the docs"
                                    : "row's hostile condition unstated in the docs" };
                }
            }
            return out;
        }

        // The predicted row of ONE effect. Unknown carries a reason in a_why.
        EngineRow RowForEffect(const RE::EffectSetting* b, bool a_self, std::string& a_why) {
            const Arch a      = b->data.archetype;
            const bool hostile = b->data.flags.all(RE::EffectSetting::EffectSettingData::Flag::kHostile);
            const int p = AVi(b->data.primaryAV), s = AVi(b->data.secondaryAV);
            Lookup l = LookupAv(a, p, a_self, hostile);
            if (l.hit == Hit::Miss && AvKeyed(a)) {
                Lookup l2 = LookupAv(a, s, a_self, hostile);   // the engine's second lookup, STATUS:135-139
                if (l2.hit != Hit::Miss) l = l2;
            }
            if (l.hit == Hit::Match) return l.caster;
            if (l.hit == Hit::Unsure) { a_why = l.why; return EngineRow::Unknown; }
            // Banish / TurnUndead: the doc writes their key AV as "1" without saying what the effect
            // field holding it is. An effect whose AV differs is a question, not a proven no-row.
            if ((a == Arch::kBanish || a == Arch::kTurnUndead) && hostile && !a_self) {
                a_why = "Banish/TurnUndead row keys AV 1 in the docs; this effect's AV differs, source field unclear";
                return EngineRow::Unknown;
            }
            return EngineRow::NoRow;
        }

        const char* DeliveryName(RE::MagicSystem::Delivery d) {
            switch (d) {
            case RE::MagicSystem::Delivery::kSelf:           return "Self";
            case RE::MagicSystem::Delivery::kTouch:          return "Touch";
            case RE::MagicSystem::Delivery::kAimed:          return "Aimed";
            case RE::MagicSystem::Delivery::kTargetActor:    return "TargetActor";
            case RE::MagicSystem::Delivery::kTargetLocation: return "TargetLocation";
            default:                                         return "?";
            }
        }
        const char* CastingName(RE::MagicSystem::CastingType c) {
            switch (c) {
            case RE::MagicSystem::CastingType::kConstantEffect: return "Constant";
            case RE::MagicSystem::CastingType::kFireAndForget:  return "FF";
            case RE::MagicSystem::CastingType::kConcentration:  return "Conc";
            case RE::MagicSystem::CastingType::kScroll:         return "Scroll";
            default:                                            return "?";
            }
        }

        SpellArchetype Compute(RE::SpellItem* sp) {
            SpellArchetype o;
            o.delivery   = sp->data.delivery;
            o.casting    = sp->data.castingType;
            o.noDualMods = sp->data.flags.all(RE::SpellItem::SpellFlag::kNoDualCastMods);
            o.chargeTime = sp->data.chargeTime;
            o.range      = sp->data.range;
            o.twoHanded  = sp->IsTwoHanded();
            switch (CasterConsent::ClassifySpell(sp)) {
            case CasterConsent::SpellKind::Offense: o.kindName = "Offense"; break;
            case CasterConsent::SpellKind::Heal:    o.kindName = "Heal";    break;
            default:                                o.kindName = "Buff";    break;
            }
            o.restoration = IsRestorationSpell(sp);

            const bool self = o.delivery == RE::MagicSystem::Delivery::kSelf;
            bool anySummon = false, anyReanimate = false, anyScript = false, anyBound = false,
                 anyCloak = false, anyLight = false, anyWard = false;
            // Per-effect rows, the distinct caster rows, the costliest effect (Effect::cost).
            std::vector<EngineRow> rows;
            float bestCost = -1.0f;
            std::size_t bestIdx = 0;
            std::string unknownWhy;
            for (auto* eff : sp->effects) {
                auto* b = eff ? eff->baseEffect : nullptr;
                if (!b) continue;
                ++o.effectCount;
                const Arch a = b->data.archetype;
                anySummon    |= a == Arch::kSummonCreature;
                anyReanimate |= a == Arch::kReanimate;
                anyScript    |= a == Arch::kScript;
                anyBound     |= a == Arch::kBoundWeapon;
                anyCloak     |= a == Arch::kCloak;
                anyLight     |= a == Arch::kLight;
                anyWard      |= a == Arch::kValueModifier && b->data.primaryAV == AV::kWardPower;
                o.anyHostile |= b->data.flags.all(RE::EffectSetting::EffectSettingData::Flag::kHostile);
                o.snapToNavMesh |= b->data.flags.all(RE::EffectSetting::EffectSettingData::Flag::kSnapToNavMesh);
                o.area = std::max(o.area, eff->effectItem.area);
                if (b->data.explosion) o.hasExplosion = true;
                if (auto* proj = b->data.projectileBase) {
                    o.hasProjectile = true;
                    if (proj->IsBarrier()) o.hasBarrier = true;
                }
                std::string why;
                const EngineRow r = RowForEffect(b, self, why);
                if (r == EngineRow::Unknown && unknownWhy.empty()) unknownWhy = why;
                if (eff->cost > bestCost) { bestCost = eff->cost; bestIdx = rows.size(); }
                rows.push_back(r);
            }

            // Combine. STATUS.md:139-141: the spell takes the row of its highest-scoring effect that
            // has a non-null creator. The score is not readable from here, so with several differing
            // rows the costliest effect is the stand-in (flagged rowApprox).
            std::vector<EngineRow> casters;
            bool anyUnknown = false;
            for (auto r : rows) {
                if (r == EngineRow::Unknown) { anyUnknown = true; continue; }
                if (r == EngineRow::NoRow) continue;
                if (std::find(casters.begin(), casters.end(), r) == casters.end()) casters.push_back(r);
            }
            if (rows.empty()) {
                o.row = EngineRow::NoRow; o.rowWhy = "spell has no effects";
            } else if (casters.empty()) {
                if (anyUnknown) { o.row = EngineRow::Unknown; o.rowWhy = unknownWhy; }
                else {
                    o.row = EngineRow::NoRow;
                    o.rowWhy = "no table row matches any effect's (archetype, AV, self, hostile)";
                }
            } else if (casters.size() == 1 && !anyUnknown) {
                o.row = casters.front();
            } else {
                // several candidate rows, or a candidate beside an effect the docs cannot place
                const EngineRow costliest = rows[bestIdx];
                o.row = (costliest != EngineRow::NoRow && costliest != EngineRow::Unknown) ? costliest
                                                                                           : casters.front();
                o.rowApprox = true;
                o.rowWhy = anyUnknown ? "an effect's row is unknown and the highest-scoring effect cannot be read; costliest effect used"
                                      : "several effects map to different rows and the engine's effect score cannot be read; costliest effect used";
            }

            // Shape. Precedence: the effect archetype shapes first (they decide the caster and the
            // road), then delivery-driven shapes. Research 3.1.
            using D = RE::MagicSystem::Delivery;
            if (o.casting == RE::MagicSystem::CastingType::kConstantEffect) o.shape = SpellShape::Constant;
            else if (anySummon)    o.shape = SpellShape::Summon;
            else if (anyReanimate) o.shape = SpellShape::Reanimate;
            else if (anyScript)    o.shape = SpellShape::Script;
            else if (anyBound)     o.shape = SpellShape::BoundWeapon;
            else if (anyCloak)     o.shape = SpellShape::Cloak;
            else if (anyWard)      o.shape = SpellShape::Ward;
            else if (anyLight)     o.shape = SpellShape::Light;
            else if (o.delivery == D::kTargetLocation) {
                if (o.casting == RE::MagicSystem::CastingType::kConcentration) o.shape = SpellShape::Wall;
                else if (o.hasProjectile)                                       o.shape = SpellShape::Rune;
                else                                                            o.shape = SpellShape::TLBurst;
            }
            else if (o.delivery == D::kAimed && (o.hasExplosion || o.area > 0)) o.shape = SpellShape::AimedAoE;
            else if (o.delivery == D::kTouch) o.shape = SpellShape::Touch;
            else if (self && o.anyHostile && o.area > 0) o.shape = SpellShape::SelfAoE;
            else o.shape = SpellShape::Plain;
            return o;
        }

        // Leaf mutexes. Never held across a log or an engine call.
        std::mutex g_cacheMx;
        std::unordered_map<RE::FormID, SpellArchetype> g_cache;

        std::mutex g_noteMx;
        std::unordered_set<std::uint64_t> g_noted;        // (follower, spell, road)
        std::unordered_set<std::uint64_t> g_warned;       // (follower, spell)

        const char* RoadName(ArchRoad r) {
            switch (r) {
            case ArchRoad::OwnedClaim: return "owned-offense-claim";
            case ArchRoad::ConcClaim:  return "concentration-claim";
            case ArchRoad::HealClaim:  return "heal-claim";
            case ArchRoad::Direct:     return "direct-force";
            case ArchRoad::Summon:     return "summon-one-shot";
            case ArchRoad::Legacy:     return "legacy-force-cast";
            }
            return "?";
        }
        bool IsClaimRoad(ArchRoad r) {
            return r == ArchRoad::OwnedClaim || r == ArchRoad::ConcClaim || r == ArchRoad::HealClaim;
        }
        std::string RowText(const SpellArchetype& a) {
            std::string t = EngineRowName(a.row);
            if (a.rowApprox) t += "(approx)";
            if (!a.rowWhy.empty()) { t += " ["; t += a.rowWhy; t += "]"; }
            return t;
        }
    }

    const char* ShapeName(SpellShape s) {
        switch (s) {
        case SpellShape::Plain:       return "Plain";
        case SpellShape::Constant:    return "Constant";
        case SpellShape::Summon:      return "Summon";
        case SpellShape::Reanimate:   return "Reanimate";
        case SpellShape::Script:      return "Script";
        case SpellShape::BoundWeapon: return "BoundWeapon";
        case SpellShape::Cloak:       return "Cloak";
        case SpellShape::Ward:        return "Ward";
        case SpellShape::Light:       return "Light";
        case SpellShape::Rune:        return "Rune";
        case SpellShape::Wall:        return "Wall";
        case SpellShape::TLBurst:     return "TLBurst";
        case SpellShape::AimedAoE:    return "AimedAoE";
        case SpellShape::Touch:       return "Touch";
        case SpellShape::SelfAoE:     return "SelfAoE";
        }
        return "?";
    }

    const char* EngineRowName(EngineRow r) {
        switch (r) {
        case EngineRow::NoRow:        return "none";
        case EngineRow::Unknown:      return "unknown";
        case EngineRow::Offensive:    return "Offensive";
        case EngineRow::Restore:      return "Restore";
        case EngineRow::Stagger:      return "Stagger";
        case EngineRow::Disarm:       return "Disarm";
        case EngineRow::TargetEffect: return "TargetEffect";
        case EngineRow::Paralyze:     return "Paralyze";
        case EngineRow::Script:       return "Script";
        case EngineRow::Ward:         return "Ward";
        case EngineRow::Summon:       return "Summon";
        case EngineRow::Cloak:        return "Cloak";
        case EngineRow::Light:        return "Light";
        case EngineRow::Invisibility: return "Invisibility";
        case EngineRow::BoundItem:    return "BoundItem";
        case EngineRow::Armor:        return "Armor";
        case EngineRow::Reanimate:    return "Reanimate";
        }
        return "?";
    }

    SpellArchetype ClassifyArchetype(RE::SpellItem* a_spell) {
        if (!a_spell) { SpellArchetype n; n.rowWhy = "null spell"; return n; }
        const auto id = a_spell->GetFormID();
        {
            std::lock_guard lk(g_cacheMx);
            if (auto it = g_cache.find(id); it != g_cache.end()) return it->second;
        }
        SpellArchetype a = Compute(a_spell);   // outside the lock: pure form reads
        std::lock_guard lk(g_cacheMx);
        return g_cache.emplace(id, std::move(a)).first->second;
    }

    std::string DescribeArchetype(RE::SpellItem* a_spell) {
        if (!a_spell) return "no spell";
        const auto a = ClassifyArchetype(a_spell);
        return std::format("\"{}\" ({:08X}) shape={} predicted={}",
                           a_spell->GetName() ? a_spell->GetName() : "?", a_spell->GetFormID(),
                           ShapeName(a.shape), RowText(a));
    }

    void NoteArchetypeRoad(RE::Actor* a_follower, RE::SpellItem* a_spell, ArchRoad a_road) {
        if (!a_follower || !a_spell) return;
        const auto fid = a_follower->GetFormID();
        const auto sid = a_spell->GetFormID();
        const std::uint64_t pair = (static_cast<std::uint64_t>(fid) << 32) | sid;
        bool first = false, warn = false;
        {
            std::lock_guard lk(g_noteMx);
            // (follower, spell) with the road folded in through a multiplicative mix; the ledger is
            // a log dedup, so a (vanishingly unlikely) collision would only swallow one info line.
            first = g_noted.insert(pair ^ ((static_cast<std::uint64_t>(a_road) + 1) * 0x9E3779B97F4A7C15ull)).second;
            // The per-tick fast path (a claim road is asked every tick): nothing left to log.
            if (!first && (!IsClaimRoad(a_road) || g_warned.count(pair) != 0)) return;
        }
        const auto a = ClassifyArchetype(a_spell);
        if (first) {
            const char* sname = a_spell->GetName() ? a_spell->GetName() : "?";
            const char* fname = a_follower->GetName() ? a_follower->GetName() : "?";
            spdlog::info("[archetype] {:08X} {} cast \"{}\" ({:08X}): road={} shape={} predicted={} delivery={} "
                         "casting={} kind={} restoration={} twoHanded={} noDualMods={} charge={:.2f}s range={:.0f} "
                         "area={} projectile={}{} explosion={} snapNav={} effects={}",
                         fid, fname, sname, sid, RoadName(a_road), ShapeName(a.shape), RowText(a),
                         DeliveryName(a.delivery), CastingName(a.casting), a.kindName, a.restoration,
                         a.twoHanded, a.noDualMods, a.chargeTime, a.range, a.area, a.hasProjectile,
                         a.hasBarrier ? "(barrier)" : "", a.hasExplosion, a.snapToNavMesh, a.effectCount);
        }
        if (IsClaimRoad(a_road) && a.row == EngineRow::NoRow) {
            std::lock_guard lk(g_noteMx);
            warn = g_warned.insert(pair).second;
        }
        if (warn) {
            spdlog::warn("[archetype] {:08X} claim for \"{}\" ({:08X}) on road {} but the predicted engine caster row "
                         "is NONE ({}) -- the engine has no caster for this spell, so this claim cannot fire "
                         "(shape={}; prediction mirrored from APMF STATUS.md:135-161, check [ctcensus])",
                         fid, a_spell->GetName() ? a_spell->GetName() : "?", sid, RoadName(a_road),
                         a.rowWhy.empty() ? "no reason recorded" : a.rowWhy, ShapeName(a.shape));
        }
    }

    void CastBreadcrumb(const char* a_road, RE::Actor* a_caster, RE::SpellItem* a_spell, RE::FormID a_targetId) {
        // Info level: plugin.cpp's log->flush_on(info) already flushes every info line. The explicit
        // flush below keeps the contract if that policy is ever relaxed to flush_on(warn): THIS line
        // must be on disk before the engine call that may hang the process.
        if (!a_spell) return;
        const auto a = ClassifyArchetype(a_spell);
        spdlog::info("[cast-call] about to CastSpellImmediate: road={} caster={:08X} spell=\"{}\" ({:08X}) "
                     "target={:08X} shape={} predicted={}",
                     a_road ? a_road : "?", a_caster ? a_caster->GetFormID() : 0u,
                     a_spell->GetName() ? a_spell->GetName() : "?", a_spell->GetFormID(), a_targetId,
                     ShapeName(a.shape), EngineRowName(a.row));
        if (auto* lg = spdlog::default_logger_raw()) lg->flush();
    }

    void ResetArchetypeLog() {
        std::lock_guard lk(g_noteMx);
        g_noted.clear();
        g_warned.clear();
    }

}
