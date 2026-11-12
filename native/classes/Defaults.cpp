// classes/Defaults.cpp -- default gambit sets per class and the per-class table memory (class update card C12).
//
// WHAT IT DOES (the contract is in Classes.h, "default gambit sets + per-class memory"):
//   EquipClass       store the live tables under what he leaves, equip, then hand back the class's stored tables or,
//                    the FIRST time, lay down its default combat rows (marth Q8 / Q29 / Q59).
//   UnequipClass     classes OFF for him: store, take the class off, hand back the tables he had without classes.
//   ResetClassTables the reset action: the equipped class's default combat rows again.
//   OnRankUp         marth Q74: a rank-up appends the next default rows while his table is still the untouched prefix.
// The MCLS side of every switch is ONE compound write (detail::SwapClass, State.cpp); this file only builds rows and
// replaces the follower's LIVE tables (FLWR's), which the caller owns.
//
// WHERE THE DEFAULT ROWS COME FROM (never list data): tier 2 = the class's DefaultGambit[] in ClassTable.cpp, applied
// rows only; tier 1 = today's base kit (Followers::ApplyDefaultKit, marth 2026-11-09 "T1 keeps today's kit"); tier 3+ =
// none shipped yet (parked by marth), so the live combat table is carried over. The T2 rows are the simulator's kept
// sets (Docs/research/2026-11/iaus-sim-v2.md section 27) as STARTING defaults; the values still want the in-game probe.
//
// SPELL ROLES: a default cast row names a SpellRole, never a form. The row is laid down DISABLED with no spell (an
// enabled cast row with no spell would win its condition and do nothing, blocking every row under it: the evaluator
// stops at the first true condition), then the follower's own known spell for the role is picked on the MAIN thread
// and written back on the worker, which enables the row. The pick reads the spell RECORDS through cast/'s own
// classifiers (CasterConsent::ClassifySpell, Actuation::ClassifyArchetype / HealsHealth / IsSummonSpell), never a second
// rule and never a per-list table. No known spell for a role: the row stays disabled and is logged, for the player to
// fill on the board. Nothing here retries or substitutes (principle 7).
//
// THREADING: EquipClass / UnequipClass / ResetClassTables / OnRankUp are WORKER-ONLY (a_st lives in g_followers).
// The spell pick is the only engine read: Actor::VisitSpells and CalculateMagickaCost read state the main thread
// mutates (the iaus/SpellBook.h rule), so it runs in ONE MainThread::Post, and its result goes back to the worker in an
// AddTask body under Diagnostics::PumpTickGate (the logistics/PlayerGiven.cpp Queue pattern), so a write queued before
// a load or revert never lands after it (the pump epoch changed); a body that meets the pump PAUSED for a save is
// queued again (LandPicks). MainThread::Clear drops a pending post on revert. On VR (no main-thread pump) the pick does
// not run: the cast rows stay disabled, logged once. A documented degrade, not a mask.
//
// KNOWN WINDOW: a save taken in the one or two frames between laying the rows down and the pick landing stores those
// cast rows disabled with no spell. They load that way and stay for the player to fill; nothing re-picks after a load.
#include "Classes_internal.h"
#include "CasterConsent.h"   // ClassifySpell
#include "Diagnostics.h"     // CurrentPumpEpoch / PumpTickGate
#include "Followers.h"       // ApplyDefaultKit, RepublishActiveMirror
#include "MainThread.h"
#include "cast/Actuation.h"  // ClassifyArchetype, HealsHealth, IsSummonSpell, CastRoadUnsupported, SpellNatureUndeclared

namespace MFO::Classes {

    namespace {
        struct DefaultRow {
            Gambit    row;
            SpellRole role = SpellRole::None;
        };

        // Batch L (cards G13 / G14): the two class default actions that exist in the LOGISTICS table only. They sit in the class's
        // DefaultGambit[] (one list per class) but are never part of its combat set.
        bool IsLogisticsOnlyAct(std::string_view a_act) { return a_act == Vocab::kActSteal || a_act == Vocab::kActPickLocks; }

        // The class's FULL default combat set in priority order (not yet cut to a rank). nullopt = no default set is
        // shipped for this class (tier 3+, parked): the caller leaves the live table alone.
        std::optional<std::vector<DefaultRow>> FullDefaults(Id a_class) {
            const ClassDef& def = Get(a_class);
            std::vector<DefaultRow> out;
            if (def.tier == 1) {
                // marth 2026-11-09: T1 keeps today's kit, no class default set (its DefaultGambit rows are pendingRuling).
                FollowerState kit{};
                Followers::ApplyDefaultKit(kit);
                for (auto& g : kit.combat()) out.push_back({ std::move(g), SpellRole::None });
                return out;
            }
            if (def.tier != 2) return std::nullopt;
            for (std::size_t i = 0; i < def.gambitCount; ++i) {
                const DefaultGambit& d = def.gambits[i];
                if (!d.IsApplied() || d.pendingRuling) continue;   // a "todo.*" placeholder / a row a ruling holds back
                if (IsLogisticsOnlyAct(d.act)) continue;           // batch L: act.steal / act.pick_locks belong to the LOGISTICS table
                Gambit g{};
                g.conditionOpcode = d.cond;
                g.actionOpcode    = d.act;
                g.conditionParam  = d.condParam;
                g.subjectSelector = d.subject;
                g.cooldownTenths  = CooldownTenthsFromSec(d.cooldownSec);   // card G9; only power-attack rows carry one (MFO-B2 of G9)
                out.push_back({ std::move(g), d.role });
            }
            return out;
        }

        // Same default row? The serialized identity MINUS the spell and the enabled bit (a role row's spell is picked
        // per follower and it starts disabled), so OnRankUp can tell an untouched default prefix from an edited table.
        bool SameDefault(const Gambit& a_live, const Gambit& a_def) {
            return a_live.conditionOpcode == a_def.conditionOpcode && a_live.actionOpcode == a_def.actionOpcode &&
                   a_live.conditionParam == a_def.conditionParam && a_live.subjectSelector == a_def.subjectSelector &&
                   a_live.subjectActorForm == 0 && a_live.cooldownTenths == a_def.cooldownTenths;
        }

        const char* RoleName(SpellRole a_r) {
            switch (a_r) {
                case SpellRole::None:        return "None";
                case SpellRole::Attack:      return "Attack";
                case SpellRole::Destruction: return "Destruction";
                case SpellRole::Fire:        return "Fire";
                case SpellRole::Frost:       return "Frost";
                case SpellRole::Shock:       return "Shock";
                case SpellRole::Area:        return "Area";
                case SpellRole::Heal:        return "Heal";
                case SpellRole::Summon:      return "Summon";
                case SpellRole::BoundWeapon: return "BoundWeapon";
                case SpellRole::Ward:        return "Ward";
                case SpellRole::Armor:       return "Armor";
                case SpellRole::Paralyze:    return "Paralyze";
                case SpellRole::Fear:        return "Fear";
                case SpellRole::Calm:        return "Calm";
                case SpellRole::Reanimate:   return "Reanimate";
                case SpellRole::TurnUndead:  return "TurnUndead";
            }
            return "?";
        }

        // ── the spell pick (MAIN THREAD) ────────────────────────────────────────────────────────────────────────
        struct Want {
            std::uint32_t uid = 0;        // the row's runtime uid (assigned when the row was laid down)
            SpellRole     role = SpellRole::None;
            bool          selfRow = false;   // act.cast_self: the spell must be Self delivery when the role is a heal
        };
        struct Pick {
            std::uint32_t uid = 0;
            SpellRole     role = SpellRole::None;
            RE::FormID    spell = 0;      // 0 = he knows no spell for the role
        };

        // What one spell record says, read once per spell.
        struct SpellFacts {
            RE::SpellItem* spell = nullptr;
            float cost = 0.0f;                 // magicka cost for THIS caster
            bool  offense = false, heal = false, summon = false, selfDelivery = false;
            bool  paralysis = false, demoralize = false, calm = false, turnUndead = false, anyControl = false;
            int   resistAv = -1;               // the costliest hostile effect's resist AV
            bool  destruction = false;         // ...and its school is Destruction
            Actuation::SpellShape shape = Actuation::SpellShape::Plain;
            bool  armorRow = false;
            bool  area = false;
        };

        SpellFacts ReadFacts(RE::SpellItem* a_sp, RE::Actor* a_caster) {
            using Arch = RE::EffectArchetypes::ArchetypeID;
            SpellFacts f;
            f.spell = a_sp;
            f.cost  = a_sp->CalculateMagickaCost(a_caster);
            const auto kind = CasterConsent::ClassifySpell(a_sp);
            f.offense = kind == CasterConsent::SpellKind::Offense;
            f.heal    = kind == CasterConsent::SpellKind::Heal && Actuation::HealsHealth(a_sp);
            f.summon  = Actuation::IsSummonSpell(a_sp);
            float costliestHostile = -1.0f;
            for (auto* eff : a_sp->effects) {
                auto* base = eff ? eff->baseEffect : nullptr;
                if (!base) continue;
                switch (base->GetArchetype()) {
                    case Arch::kParalysis:  f.paralysis = true; break;
                    case Arch::kDemoralize: f.demoralize = true; break;
                    case Arch::kCalm:       f.calm = true; break;
                    case Arch::kTurnUndead: f.turnUndead = true; break;
                    case Arch::kFrenzy:     f.anyControl = true; break;
                    default: break;
                }
                // The element and the school of the COSTLIEST hostile effect (the iaus/SpellBook.cpp rule).
                if ((base->IsHostile() || base->IsDetrimental()) && eff->cost > costliestHostile) {
                    costliestHostile = eff->cost;
                    const auto rv = base->data.resistVariable;
                    f.resistAv    = rv == RE::ActorValue::kNone ? -1 : static_cast<int>(rv);
                    f.destruction = base->GetMagickSkill() == RE::ActorValue::kDestruction;
                }
            }
            f.anyControl = f.anyControl || f.paralysis || f.demoralize || f.calm || f.turnUndead;
            const auto arch = Actuation::ClassifyArchetype(a_sp);
            f.selfDelivery = arch.delivery == RE::MagicSystem::Delivery::kSelf;
            f.shape    = arch.shape;
            f.armorRow = arch.row == Actuation::EngineRow::Armor;
            f.area     = arch.area > 0;
            return f;
        }

        bool Fits(const SpellFacts& f, SpellRole a_role, bool a_selfRow) {
            using Shape = Actuation::SpellShape;
            // A plain damage spell at a foe: hostile, not a control spell, not a summon / reanimate, not worn (Self).
            const bool attack = f.offense && !f.anyControl && !f.summon && f.shape != Shape::Reanimate && !f.selfDelivery;
            switch (a_role) {
                case SpellRole::None:        return false;
                case SpellRole::Attack:      return attack;
                case SpellRole::Destruction: return attack && f.destruction;
                case SpellRole::Fire:        return attack && f.resistAv == static_cast<int>(RE::ActorValue::kResistFire);
                case SpellRole::Frost:       return attack && f.resistAv == static_cast<int>(RE::ActorValue::kResistFrost);
                case SpellRole::Shock:       return attack && f.resistAv == static_cast<int>(RE::ActorValue::kResistShock);
                case SpellRole::Area:        return attack && f.area;
                case SpellRole::Heal:        return f.heal && f.selfDelivery == a_selfRow;   // a heal-other cannot be cast on himself
                case SpellRole::Summon:      return f.summon;
                case SpellRole::BoundWeapon: return f.shape == Shape::BoundWeapon;
                case SpellRole::Ward:        return f.shape == Shape::Ward;
                case SpellRole::Armor:       return f.armorRow && f.selfDelivery;
                case SpellRole::Paralyze:    return f.paralysis && !f.selfDelivery;
                case SpellRole::Fear:        return f.demoralize && !f.selfDelivery;
                case SpellRole::Calm:        return f.calm && !f.selfDelivery;
                case SpellRole::Reanimate:   return f.shape == Shape::Reanimate;
                case SpellRole::TurnUndead:  return f.turnUndead && !f.selfDelivery;
            }
            return false;
        }

        // MAIN THREAD. The follower's own spell for each wanted role: the costliest one he can pay for from a full
        // magicka pool (the strongest he can actually cast), else the cheapest that fits; ties go to the lower FormID.
        std::vector<Pick> PickSpells(RE::Actor* a_actor, const std::vector<Want>& a_wants) {
            struct Collect : RE::Actor::ForEachSpellVisitor {
                std::vector<RE::SpellItem*> spells;
                RE::BSContainer::ForEachResult Visit(RE::SpellItem* a_sp) override {
                    if (a_sp && Vocab::IsCastableSpell(a_sp) && !Actuation::CastRoadUnsupported(a_sp) &&
                        !Actuation::SpellNatureUndeclared(a_sp))
                        spells.push_back(a_sp);
                    return RE::BSContainer::ForEachResult::kContinue;
                }
            } vis;
            a_actor->VisitSpells(vis);
            std::vector<SpellFacts> facts;
            facts.reserve(vis.spells.size());
            for (auto* sp : vis.spells) facts.push_back(ReadFacts(sp, a_actor));
            auto* avo = a_actor->AsActorValueOwner();
            const float pool = avo ? avo->GetPermanentActorValue(RE::ActorValue::kMagicka) : 0.0f;

            std::vector<Pick> out;
            out.reserve(a_wants.size());
            for (const auto& w : a_wants) {
                const SpellFacts* best = nullptr;
                bool bestAffordable = false;
                for (const auto& f : facts) {
                    if (!Fits(f, w.role, w.selfRow)) continue;
                    const bool affordable = f.cost <= pool;
                    bool better = false;
                    if (!best) better = true;
                    else if (affordable != bestAffordable) better = affordable;
                    else if (f.cost != best->cost) better = affordable ? f.cost > best->cost : f.cost < best->cost;
                    else better = f.spell->GetFormID() < best->spell->GetFormID();
                    if (better) { best = &f; bestAffordable = affordable; }
                }
                out.push_back({ w.uid, w.role, best ? best->spell->GetFormID() : 0 });
            }
            return out;
        }

        bool IsCastAct(const std::string& a_act) {
            return a_act == Vocab::kActCastSelf || a_act == Vocab::kActCastTarget || a_act == Vocab::kActCastPlayer;
        }

        // Write the picks into the follower's live combat table. The write runs in an AddTask body under
        // Diagnostics::PumpTickGate (the worker domain that owns g_followers). The gate also refuses while the pump is
        // PAUSED for a save: then the epoch is unchanged and the write is tried again on a later FRAME (through
        // MainThread::Post; kLandTries frames bounds it and the give-up is an ERROR line, never silent). A changed epoch
        // is a load or a revert: that table is gone, and so is this write.
        constexpr int kLandTries = 600;
        void LandPicks(RE::FormID a_follower, Id a_class, std::uint64_t a_epoch, std::vector<Pick> a_picks, int a_triesLeft) {
            SKSE::GetTaskInterface()->AddTask([a_follower, a_class, a_epoch, a_triesLeft, picks = std::move(a_picks)]() mutable {
                {
                    Diagnostics::PumpTickGate gate(a_epoch);
                    if (gate) {
                        const auto it = g_followers.find(a_follower);
                        if (it == g_followers.end()) return;
                        for (const auto& p : picks) {
                            Gambit* row = nullptr;
                            for (auto& g : it->second.combat())
                                if (g.uid == p.uid) { row = &g; break; }
                            // The row must still be the one laid down: a cast row with no spell, still disabled. Anything
                            // else means the player (or a class switch) got there first, and his choice stands.
                            if (!row || row->actionParamForm != 0 || row->enabled || !IsCastAct(row->actionOpcode)) {
                                spdlog::info("[classes] {:08X} ({}): the {} row changed before its spell pick landed -- left as it is",
                                             a_follower, Get(a_class).slug, RoleName(p.role));
                                continue;
                            }
                            if (p.spell == 0) {
                                spdlog::warn("[classes] {:08X} ({}): knows no {} spell -- the default row '{}' -> '{}' stays DISABLED "
                                             "with no spell (pick one on the board)", a_follower, Get(a_class).slug, RoleName(p.role),
                                             row->conditionOpcode, row->actionOpcode);
                                continue;
                            }
                            row->actionParamForm = p.spell;
                            row->enabled = true;
                            spdlog::info("[classes] {:08X} ({}): default row '{}' -> '{}' [{}] casts {:08X}", a_follower,
                                         Get(a_class).slug, row->conditionOpcode, row->actionOpcode, RoleName(p.role), p.spell);
                        }
                        return;
                    }
                }
                if (Diagnostics::CurrentPumpEpoch() != a_epoch) return;   // a load / revert came between: that table is gone
                if (a_triesLeft <= 0) {
                    spdlog::error("[classes] {:08X} ({}): the pump stayed paused for {} tries -- {} default cast row(s) stay DISABLED "
                                  "with no spell (pick the spells on the board)", a_follower, Get(a_class).slug, kLandTries, picks.size());
                    return;
                }
                // Paused for a save. Come back through the main-thread pump, which drains once per FRAME (an AddTask queued
                // from inside an AddTask body may run again in the same drain, and would burn every try at once).
                MainThread::Post([a_follower, a_class, a_epoch, a_triesLeft, picks = std::move(picks)]() mutable {
                    LandPicks(a_follower, a_class, a_epoch, std::move(picks), a_triesLeft - 1);
                });
            });
        }

        // WORKER. Ask the main thread for the spells of the role rows just laid down (a_wants is never empty here).
        void PostPick(RE::FormID a_follower, Id a_class, std::vector<Want> a_wants) {
            if (!MainThread::IsInstalled()) {
                static std::atomic<bool> s_logged{ false };
                if (!s_logged.exchange(true))
                    spdlog::warn("[classes] no main-thread pump on this runtime (VR): the default CAST rows of a class stay disabled "
                                 "with no spell -- pick the spells on the board");
                return;
            }
            const auto epoch = Diagnostics::CurrentPumpEpoch();
            MainThread::Post([a_follower, a_class, epoch, wants = std::move(a_wants)]() {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_follower);
                if (!actor) {
                    spdlog::warn("[classes] {:08X} ({}): the follower did not resolve on the main thread -- {} default cast row(s) stay "
                                 "disabled with no spell", a_follower, Get(a_class).slug, wants.size());
                    return;
                }
                LandPicks(a_follower, a_class, epoch, PickSpells(actor, wants), kLandTries);
            });
        }

        // Turn default rows [a_from, a_to) into live rows appended to a_table; role rows are queued for the spell pick.
        void LayDown(const std::vector<DefaultRow>& a_defs, std::size_t a_from, std::size_t a_to, std::vector<Gambit>& a_table,
                     std::vector<Want>& a_wants) {
            for (std::size_t i = a_from; i < a_to && i < a_defs.size(); ++i) {
                Gambit g = a_defs[i].row;
                g.uid = NextRuleUID();
                if (a_defs[i].role != SpellRole::None) {
                    g.enabled = false;   // until its spell is picked (see the file header)
                    a_wants.push_back({ g.uid, a_defs[i].role, g.actionOpcode == Vocab::kActCastSelf });
                }
                a_table.push_back(std::move(g));
            }
        }

        // The class's default combat table for his rank (marth Q74: the TOP rows that fit his slots). false = no default
        // set is shipped for the class; a_st is untouched.
        bool ApplyDefaults(RE::FormID a_follower, Id a_class, FollowerState& a_st) {
            const auto defs = FullDefaults(a_class);
            if (!defs) return false;
            const std::size_t slots = SlotsForRank(a_st.rank, Table::Combat);
            std::vector<Gambit> table;
            std::vector<Want> wants;
            LayDown(*defs, 0, slots, table, wants);
            a_st.combat() = std::move(table);
            spdlog::info("[classes] {:08X} ({}): default combat table laid down -- {} of {} row(s) fit rank {} ({} slot(s)), {} cast "
                         "row(s) waiting for a spell pick", a_follower, Get(a_class).slug, a_st.combat().size(), defs->size(),
                         static_cast<int>(a_st.rank), slots, wants.size());
            if (!wants.empty()) PostPick(a_follower, a_class, std::move(wants));
            return true;
        }

        // Batch L (G13 / G14): the first equip of a class that ships logistics defaults (the Thief line: Steal when dark, Pick locks)
        // APPENDS them to the live logistics table, after a sneak row when the class sneaks always (kSneakAlways, card G1: the pre-check
        // needs a sneaking follower, and act.sneak is TRANSPARENT so the rows below it still run). A row he already has (same condition
        // and action) is not duplicated, and a full table keeps its rows (logged). Later equips restore the stored table: the player's
        // edits stand. Works with the tree OFF as ordinary gambits, the player adds them from the board.
        void LayLogisticsDefaults(RE::FormID a_follower, Id a_class, FollowerState& a_st) {
            const ClassDef& def = Get(a_class);
            std::vector<Gambit> rows;
            auto add = [&](const char* a_c, const char* a_a) {
                Gambit g{};
                g.conditionOpcode = a_c;
                g.actionOpcode    = a_a;
                rows.push_back(std::move(g));
            };
            bool any = false;
            for (std::size_t i = 0; i < def.gambitCount; ++i) {
                const DefaultGambit& d = def.gambits[i];
                if (d.IsApplied() && !d.pendingRuling && IsLogisticsOnlyAct(d.act)) any = true;
            }
            if (!any) return;
            if (def.flags & Flag::kSneakAlways) add(Vocab::kCondAlways, Vocab::kActSneak);
            for (std::size_t i = 0; i < def.gambitCount; ++i) {
                const DefaultGambit& d = def.gambits[i];
                if (d.IsApplied() && !d.pendingRuling && IsLogisticsOnlyAct(d.act)) add(d.cond, d.act);
            }
            auto& table = a_st.logistics();
            const std::size_t slots = SlotsForRank(a_st.rank, Table::Logistics);
            int added = 0, dup = 0, full = 0;
            for (auto& g : rows) {
                const bool have = std::any_of(table.begin(), table.end(), [&](const Gambit& t) {
                    return t.conditionOpcode == g.conditionOpcode && t.actionOpcode == g.actionOpcode; });
                if (have) { ++dup; continue; }
                if (table.size() >= slots) { ++full; continue; }
                g.uid = NextRuleUID();
                table.push_back(std::move(g));
                ++added;
            }
            spdlog::info("[classes] {:08X} ({}): default LOGISTICS rows laid down -- {} added, {} already present, {} did not fit rank {} ({} slot(s))",
                         a_follower, def.slug, added, dup, full, static_cast<int>(a_st.rank), slots);
        }

        // MFO-B567: a stored table may come from a record an older DLL kept verbatim across a load-order change, so its
        // FormIDs are not trusted. A cast row whose form is not a castable spell is DISABLED (the form is kept, shown on
        // the board), the check the live evaluator's cast paths make (LookupByID<SpellItem>).
        void CheckRestored(RE::FormID a_follower, std::vector<Gambit>& a_rows, const char* a_which) {
            for (auto& g : a_rows) {
                if (!g.enabled || !IsCastAct(g.actionOpcode) || g.actionParamForm == 0) continue;
                auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(g.actionParamForm);
                if (sp && Vocab::IsCastableSpell(sp)) continue;
                g.enabled = false;
                spdlog::warn("[classes] {:08X}: restored {} row '{}' -> '{}' names {:08X}, which is not a castable spell in this load "
                             "order -- the row is DISABLED", a_follower, a_which, g.conditionOpcode, g.actionOpcode, g.actionParamForm);
            }
        }
    }

    EquipResult EquipClass(RE::FormID a_follower, Id a_class, FollowerState& a_st) {
        if (static_cast<std::size_t>(a_class) >= kClassCount) return EquipResult::Refused;
        auto swap = detail::SwapClass(a_follower, static_cast<std::uint8_t>(a_class), a_st.combat(), a_st.logistics());
        if (!swap.done) return EquipResult::Refused;
        const std::string_view slug = Get(a_class).slug;
        const std::string from = swap.from == kNoClass ? std::string("classes OFF") : std::string(Get(static_cast<Id>(swap.from)).slug);

        if (swap.logistics) {
            CheckRestored(a_follower, *swap.logistics, "logistics");
            a_st.logistics() = std::move(*swap.logistics);
        } else {
            LayLogisticsDefaults(a_follower, a_class, a_st);   // batch L: the Thief line's Steal / Pick locks (first equip only)
        }   // else: no class default exists for the logistics table; the live one carries over
        EquipResult res = EquipResult::Kept;
        if (swap.combat) {
            CheckRestored(a_follower, *swap.combat, "combat");
            a_st.combat() = std::move(*swap.combat);
            res = EquipResult::Restored;
        } else if (ApplyDefaults(a_follower, a_class, a_st)) {
            res = EquipResult::Defaults;
        }
        spdlog::info("[classes] {:08X}: {} -> {} -- combat table {} ({} row(s)), logistics table {} ({} row(s))", a_follower, from, slug,
                     res == EquipResult::Restored ? "RESTORED from his last time in the class"
                         : res == EquipResult::Defaults ? "set to the class DEFAULTS (first equip)"
                                                        : "KEPT (no default set is shipped for this tier yet)",
                     a_st.combat().size(), swap.logistics ? "RESTORED" : "kept", a_st.logistics().size());
        Followers::RepublishActiveMirror();   // the C10 HOOK (Classes.h): the equip changed; WORKER
        return res;
    }

    bool UnequipClass(RE::FormID a_follower, FollowerState& a_st) {
        if (GetEquipped(a_follower) == kNoClass) return false;   // nothing to take off (and no empty record is made)
        auto swap = detail::SwapClass(a_follower, kNoClass, a_st.combat(), a_st.logistics());
        if (!swap.done) return false;
        if (swap.combat) {
            CheckRestored(a_follower, *swap.combat, "combat");
            a_st.combat() = std::move(*swap.combat);
        }
        if (swap.logistics) {
            CheckRestored(a_follower, *swap.logistics, "logistics");
            a_st.logistics() = std::move(*swap.logistics);
        }
        spdlog::info("[classes] {:08X}: {} -> classes OFF -- combat table {} ({} row(s)), logistics table {} ({} row(s))", a_follower,
                     Get(static_cast<Id>(swap.from)).slug, swap.combat ? "RESTORED to his pre-class one" : "kept",
                     a_st.combat().size(), swap.logistics ? "RESTORED to his pre-class one" : "kept", a_st.logistics().size());
        Followers::RepublishActiveMirror();
        return true;
    }

    bool ResetClassTables(RE::FormID a_follower, FollowerState& a_st) {
        if (!Writable()) return false;
        const std::uint8_t eq = GetEquipped(a_follower);
        if (eq == kNoClass || eq >= kClassCount) return false;
        if (!ApplyDefaults(a_follower, static_cast<Id>(eq), a_st)) {
            spdlog::info("[classes] {:08X} ({}): reset refused -- no default set is shipped for this tier yet", a_follower,
                         Get(static_cast<Id>(eq)).slug);
            return false;
        }
        return true;
    }

    void OnRankUp(RE::FormID a_follower, FollowerState& a_st, std::uint8_t a_before) {
        if (!Writable()) return;
        const std::uint8_t eq = GetEquipped(a_follower);
        if (eq == kNoClass || eq >= kClassCount) return;
        const auto defs = FullDefaults(static_cast<Id>(eq));
        if (!defs) return;
        const std::size_t had  = (std::min<std::size_t>)(SlotsForRank(a_before, Table::Combat), defs->size());
        const std::size_t have = (std::min<std::size_t>)(SlotsForRank(a_st.rank, Table::Combat), defs->size());
        auto& live = a_st.combat();
        if (have <= had || live.size() != had) return;   // nothing new fits, or the table is not the default prefix
        for (std::size_t i = 0; i < had; ++i)
            if (!SameDefault(live[i], (*defs)[i].row)) return;   // the player edited it: his table, left alone
        std::vector<Want> wants;
        LayDown(*defs, had, have, live, wants);
        spdlog::info("[classes] {:08X} ({}): rank {} -> {} opened combat slots -- default row(s) {}..{} of {} joined his table "
                     "(marth Q74), {} waiting for a spell pick", a_follower, Get(static_cast<Id>(eq)).slug, static_cast<int>(a_before),
                     static_cast<int>(a_st.rank), had + 1, have, defs->size(), wants.size());
        if (!wants.empty()) PostPick(a_follower, static_cast<Id>(eq), std::move(wants));
    }
}
