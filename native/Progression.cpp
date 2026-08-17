#include "PCH.h"
#include "Progression.h"
#include "Config.h"
#include "Forms.h"   // §18.6: the addon-manifest sentinel keyword

// The production catalog reader. See Progression.h for the contract;
// Docs/FOLLOWER-PROGRESSION-ESL-DESIGN.md §1/§2/§3 for the design.
//
// The RE:: idioms here (AVIF perkTree BFS, BGSSkillPerkTreeNode edges,
// BGSPerk::perkEntries/perkConditions/nextPerk, entry GetType() dispatch) are
// the ones the ProgProbe field build proved on the real LoreRim/Requiem deck
// — this module is the same walk, promoted from "pick one perk to poke" to
// "emit every perk as a frozen value-only view".
//
// Every log line carries the [prog] tag. Log the ZERO cases too — "found
// none" and "never ran" must not look the same (INVARIANTS #46).

namespace MFO::Progression {

    namespace {

        // ── session state — written ONCE by Init() (kDataLoaded, main
        // thread), read-only forever after. No locks: freeze-then-publish.
        bool                  g_detected     = false;
        std::vector<AddonRef> g_addons;              // §18.6 enumerated manifests
        Catalog               g_catalog;

        // ── the 18 skill AVIFs (§2.1) ───────────────────────────────────────
        // The probe walked the 12 combat trees; the catalog walks ALL 18 on
        // purpose: (1) the §15 scarcity ratio needs the full PLAYER pool as
        // its denominator, which lives in the six non-combat trees too, and
        // (2) a Speech/Alchemy tree collapsing to ~100% filtered is exactly
        // the census evidence that proves the §3 classifier is aimed right.
        // The static names are the fallback when an AVIF has no full name.
        constexpr struct { RE::ActorValue av; const char* name; } kSkills[] = {
            { RE::ActorValue::kOneHanded,   "OneHanded" },
            { RE::ActorValue::kTwoHanded,   "TwoHanded" },
            { RE::ActorValue::kArchery,     "Archery" },
            { RE::ActorValue::kBlock,       "Block" },
            { RE::ActorValue::kHeavyArmor,  "HeavyArmor" },
            { RE::ActorValue::kLightArmor,  "LightArmor" },
            { RE::ActorValue::kDestruction, "Destruction" },
            { RE::ActorValue::kRestoration, "Restoration" },
            { RE::ActorValue::kConjuration, "Conjuration" },
            { RE::ActorValue::kAlteration,  "Alteration" },
            { RE::ActorValue::kIllusion,    "Illusion" },
            { RE::ActorValue::kSneak,       "Sneak" },
            { RE::ActorValue::kSmithing,    "Smithing" },
            { RE::ActorValue::kAlchemy,     "Alchemy" },
            { RE::ActorValue::kEnchanting,  "Enchanting" },
            { RE::ActorValue::kLockpicking, "Lockpicking" },
            { RE::ActorValue::kPickpocket,  "Pickpocket" },
            { RE::ActorValue::kSpeech,      "Speech" },
        };

        // ── the §3 entry-point classifier ───────────────────────────────────
        // One row per BGSEntryPoint::ENTRY_POINT value (92 total, engine-
        // frozen indices). The verdicts are the design's lists verbatim:
        // combat/defense = effective; lockpick/craft/commerce/player-UI =
        // dead; the named marginal set (+ judgment calls on values the design
        // left unnamed, biased toward marginal — an unknown must be FLAGGED,
        // never silently killed, so over-filtering stays diagnosable).
        struct EntryPointRow { Verdict verdict; const char* name; };

        constexpr EntryPointRow kEntryPoints[] = {
            /*  0 */ { Verdict::kEffective, "CalculateWeaponDamage" },
            /*  1 */ { Verdict::kEffective, "CalculateMyCriticalHitChance" },
            /*  2 */ { Verdict::kEffective, "CalculateMyCriticalHitDamage" },
            /*  3 */ { Verdict::kDead,      "CalculateMineExplodeChance" },   // FO leftover
            /*  4 */ { Verdict::kEffective, "AdjustLimbDamage" },
            /*  5 */ { Verdict::kDead,      "AdjustBookSkillPoints" },        // player UI
            /*  6 */ { Verdict::kEffective, "ModRecoveredHealth" },
            /*  7 */ { Verdict::kMarginal,  "GetShouldAttack" },              // AI decision — unproven on followers
            /*  8 */ { Verdict::kDead,      "ModBuyPrices" },                 // commerce
            /*  9 */ { Verdict::kDead,      "AddLeveledListOnDeath" },        // player loot economy
            /* 10 */ { Verdict::kMarginal,  "GetMaxCarryWeight" },            // §3 marginal
            /* 11 */ { Verdict::kDead,      "ModAddictionChance" },           // FO leftover
            /* 12 */ { Verdict::kDead,      "ModAddictionDuration" },         // FO leftover
            /* 13 */ { Verdict::kDead,      "ModPositiveChemDuration" },      // FO leftover
            /* 14 */ { Verdict::kDead,      "Activate" },                     // player activation UI
            /* 15 */ { Verdict::kEffective, "IgnoreRunningDuringDetection" }, // detection
            /* 16 */ { Verdict::kDead,      "IgnoreBrokenLock" },             // lockpicking
            /* 17 */ { Verdict::kEffective, "ModEnemyCriticalHitChance" },    // defense
            /* 18 */ { Verdict::kEffective, "ModSneakAttackMult" },
            /* 19 */ { Verdict::kDead,      "ModMaxPlaceableMines" },         // FO leftover
            /* 20 */ { Verdict::kDead,      "ModBowZoom" },                   // player camera
            /* 21 */ { Verdict::kMarginal,  "ModRecoverArrowChance" },        // §3 marginal
            /* 22 */ { Verdict::kDead,      "ModSkillUse" },                  // skill XP — NPCs earn none
            /* 23 */ { Verdict::kMarginal,  "ModTelekinesisDistance" },       // NPC use unproven
            /* 24 */ { Verdict::kMarginal,  "ModTelekinesisDamageMult" },
            /* 25 */ { Verdict::kMarginal,  "ModTelekinesisDamage" },
            /* 26 */ { Verdict::kEffective, "ModBashingDamage" },
            /* 27 */ { Verdict::kEffective, "ModPowerAttackStamina" },
            /* 28 */ { Verdict::kEffective, "ModPowerAttackDamage" },
            /* 29 */ { Verdict::kEffective, "ModSpellMagnitude" },
            /* 30 */ { Verdict::kEffective, "ModSpellDuration" },
            /* 31 */ { Verdict::kMarginal,  "ModSecondaryValueWeight" },      // unnamed in design — flag, don't kill
            /* 32 */ { Verdict::kMarginal,  "ModArmorWeight" },               // carry-weight family
            /* 33 */ { Verdict::kEffective, "ModIncomingStagger" },
            /* 34 */ { Verdict::kEffective, "ModTargetStagger" },
            /* 35 */ { Verdict::kEffective, "ModAttackDamage" },
            /* 36 */ { Verdict::kEffective, "ModIncomingDamage" },
            /* 37 */ { Verdict::kEffective, "ModTargetDamageResistance" },
            /* 38 */ { Verdict::kEffective, "ModSpellCost" },
            /* 39 */ { Verdict::kEffective, "ModPercentBlocked" },
            /* 40 */ { Verdict::kEffective, "ModShieldDeflectArrowChance" },
            /* 41 */ { Verdict::kEffective, "ModIncomingSpellMagnitude" },
            /* 42 */ { Verdict::kEffective, "ModIncomingSpellDuration" },
            /* 43 */ { Verdict::kDead,      "ModPlayerIntimidation" },        // social
            /* 44 */ { Verdict::kDead,      "ModPlayerReputation" },          // social
            /* 45 */ { Verdict::kDead,      "ModFavorPoints" },               // social
            /* 46 */ { Verdict::kDead,      "ModBribeAmount" },               // social
            /* 47 */ { Verdict::kEffective, "ModDetectionLight" },
            /* 48 */ { Verdict::kEffective, "ModDetectionMovement" },
            /* 49 */ { Verdict::kDead,      "ModSoulGemRecharge" },           // crafting
            /* 50 */ { Verdict::kEffective, "SetSweepAttack" },
            /* 51 */ { Verdict::kEffective, "ApplyCombatHitSpell" },
            /* 52 */ { Verdict::kEffective, "ApplyBashingSpell" },
            /* 53 */ { Verdict::kEffective, "ApplyReanimateSpell" },
            /* 54 */ { Verdict::kMarginal,  "SetBooleanGraphVariable" },      // anim var — may drive combat anims
            /* 55 */ { Verdict::kDead,      "ModSpellCastingSoundEvent" },    // cosmetic
            /* 56 */ { Verdict::kDead,      "ModPickpocketChance" },          // pickpocketing
            /* 57 */ { Verdict::kEffective, "ModDetectionSneakSkill" },
            /* 58 */ { Verdict::kEffective, "ModFallingDamage" },
            /* 59 */ { Verdict::kDead,      "ModLockpickSweetSpot" },         // lockpicking
            /* 60 */ { Verdict::kDead,      "ModSellPrices" },                // commerce
            /* 61 */ { Verdict::kDead,      "CanPickpocketEquippedItem" },    // pickpocketing
            /* 62 */ { Verdict::kDead,      "ModLockpickLevelAllowed" },      // lockpicking
            /* 63 */ { Verdict::kDead,      "SetLockpickStartingArc" },       // lockpicking
            /* 64 */ { Verdict::kDead,      "SetProgressionPicking" },        // lockpicking
            /* 65 */ { Verdict::kDead,      "MakeLockpicksUnbreakable" },     // lockpicking
            /* 66 */ { Verdict::kDead,      "ModAlchemyEffectiveness" },      // crafting
            /* 67 */ { Verdict::kEffective, "ApplyWeaponSwingSpell" },
            /* 68 */ { Verdict::kMarginal,  "ModCommandedActorLimit" },       // §3 marginal
            /* 69 */ { Verdict::kEffective, "ApplySneakingSpell" },
            /* 70 */ { Verdict::kDead,      "ModPlayerMagicSlowdown" },       // player UI
            /* 71 */ { Verdict::kEffective, "ModWardMagickaAbsorptionPct" },
            /* 72 */ { Verdict::kDead,      "ModInitialIngredientEffectsLearned" },  // crafting
            /* 73 */ { Verdict::kDead,      "PurifyAlchemyIngredients" },     // crafting
            /* 74 */ { Verdict::kDead,      "FilterActivation" },             // player activation UI
            /* 75 */ { Verdict::kMarginal,  "CanDualCastSpell" },             // §3 marginal
            /* 76 */ { Verdict::kDead,      "ModTemperingHealth" },           // crafting
            /* 77 */ { Verdict::kDead,      "ModEnchantmentPower" },          // crafting
            /* 78 */ { Verdict::kDead,      "ModSoulPctCapturedToWeapon" },   // crafting
            /* 79 */ { Verdict::kDead,      "ModSoulGemEnchanting" },         // crafting
            /* 80 */ { Verdict::kDead,      "ModNumberAppliedEnchantmentsAllowed" }, // crafting
            /* 81 */ { Verdict::kDead,      "SetActivateLabel" },             // player UI
            /* 82 */ { Verdict::kDead,      "ModShoutOK" },                   // player shouts
            /* 83 */ { Verdict::kDead,      "ModPoisonDoseCount" },           // player poison UI
            /* 84 */ { Verdict::kMarginal,  "ShouldApplyPlacedItem" },        // runes/traps — unproven
            /* 85 */ { Verdict::kEffective, "ModArmorRating" },
            /* 86 */ { Verdict::kDead,      "ModLockpickingCrimeChance" },    // lockpicking
            /* 87 */ { Verdict::kDead,      "ModIngredientsHarvested" },      // harvest
            /* 88 */ { Verdict::kMarginal,  "ModSpellRange_TargetLoc" },      // NPC use unproven
            /* 89 */ { Verdict::kDead,      "ModPotionsCreated" },            // crafting
            /* 90 */ { Verdict::kDead,      "ModLockpickingKeyRewardChance" },// lockpicking
            /* 91 */ { Verdict::kMarginal,  "AllowMountActor" },              // §3 marginal
        };
        static_assert(std::size(kEntryPoints) ==
                          static_cast<std::size_t>(RE::BGSEntryPoint::ENTRY_POINT::kTotal),
                      "entry-point table must cover every engine value");

        const EntryPointRow& RowFor(std::uint32_t a_ep) {
            static constexpr EntryPointRow kUnknown{ Verdict::kMarginal, "UnknownEntryPoint" };
            return a_ep < std::size(kEntryPoints) ? kEntryPoints[a_ep] : kUnknown;
        }

        // ── small helpers ───────────────────────────────────────────────────

        const char* NameOf(RE::TESForm* a_form) {
            if (!a_form) return "<none>";
            const char* n = a_form->GetName();
            return (n && *n) ? n : "<unnamed>";
        }

        // Append with ", " separator — reason strings for the dump.
        void Append(std::string& a_out, std::string_view a_piece) {
            if (!a_out.empty()) a_out += ", ";
            a_out += a_piece;
        }

        // The static (code-side) skill name — log-stable regardless of what
        // the load order renamed the AVIF to.
        const char* StaticName(RE::ActorValue a_av) {
            for (const auto& sk : kSkills)
                if (sk.av == a_av) return sk.name;
            return "?";
        }

        std::string AvName(RE::ActorValue a_av) {
            if (auto* avl = RE::ActorValueList::GetSingleton()) {
                if (auto* avi = avl->GetActorValue(a_av)) {
                    const char* n = avi->GetFullName();
                    if (n && *n) return n;
                }
            }
            for (const auto& sk : kSkills)
                if (sk.av == a_av) return sk.name;
            return std::format("AV{}", static_cast<int>(a_av));
        }

        // ── condition introspection (§2.2/§2.3) ─────────────────────────────

        // The §3 player-gate backstop: an AND-required item hard-requiring
        // the player's identity (GetIsID Player / GetIsReference PlayerRef,
        // == 1) makes the thing it guards inert on a follower. Deliberately
        // NARROW — opcode must be ==, the comparison a literal 1, the item
        // not OR'd — so it only ever fires on the unambiguous shape; §5's
        // evaluate-on-the-follower catches everything subtler for free.
        bool ConditionPlayerGated(const RE::TESCondition& a_cond) {
            for (auto* it = a_cond.head; it; it = it->next) {
                const auto& d  = it->data;
                const auto  fn = d.functionData.function.get();
                if (fn != RE::FUNCTION_DATA::FunctionID::kGetIsID &&
                    fn != RE::FUNCTION_DATA::FunctionID::kGetIsReference)
                    continue;
                if (d.flags.isOR) continue;   // OR'd — not a hard requirement
                if (d.flags.opCode != RE::CONDITION_ITEM_DATA::OpCode::kEqualTo) continue;
                if (d.flags.global || d.comparisonValue.f != 1.0f) continue;
                // For these functions param 0 is a form pointer resolved at
                // load. Player base = 0x7, PlayerRef = 0x14.
                auto* form = static_cast<RE::TESForm*>(d.functionData.params[0]);
                if (form && (form->GetFormID() == 0x14 || form->GetFormID() == 0x7))
                    return true;
            }
            return false;
        }

        // §2.3 skill-req extraction, DISPLAY ONLY: the first perkConditions
        // item testing GetBaseActorValue on kSelf with >= (or >) gives the
        // human string. The FormID kept beside it is what §5 enforcement
        // re-evaluates in full — nothing downstream parses this string.
        std::string ExtractSkillReq(RE::BGSPerk* a_rank,
                                    RE::ActorValue* a_outAV = nullptr,
                                    float* a_outVal = nullptr) {
            using Op = RE::CONDITION_ITEM_DATA::OpCode;
            for (auto* it = a_rank->perkConditions.head; it; it = it->next) {
                const auto& d = it->data;
                if (d.functionData.function.get() != RE::FUNCTION_DATA::FunctionID::kGetBaseActorValue)
                    continue;
                if (d.object.get() != RE::CONDITIONITEMOBJECT::kSelf) continue;
                if (d.flags.opCode != Op::kGreaterThanOrEqualTo &&
                    d.flags.opCode != Op::kGreaterThan)
                    continue;
                // Integer param stuffed in the pointer slot — the AV index.
                const auto av = static_cast<RE::ActorValue>(
                    reinterpret_cast<std::uintptr_t>(d.functionData.params[0]));
                const float v = d.flags.global
                                    ? (d.comparisonValue.g ? d.comparisonValue.g->value : 0.0f)
                                    : d.comparisonValue.f;
                if (a_outAV)  *a_outAV  = av;
                if (a_outVal) *a_outVal = v;
                return std::format("{} {:g}", AvName(av), v);
            }
            return {};
        }

        // Condition-authored prereqs (deck round 4): the NARROW shape
        // "HasPerk X, ==1 (or >=1), AND-connected, literal" — the way some
        // overhauls encode a prerequisite as a condition instead of (or on
        // top of) a tree edge. Display/LAYOUT only: §5's IsTrue-on-the-
        // follower already ENFORCES these in full; extracting them lets the
        // tiered layout put such a node above its true prerequisite. The
        // exclusion shape "HasPerk X == 0" (mutually-exclusive perks) is
        // deliberately NOT matched — it is not a prerequisite.
        std::vector<RE::FormID> ExtractCondPrereqs(RE::BGSPerk* a_rank) {
            using Op = RE::CONDITION_ITEM_DATA::OpCode;
            std::vector<RE::FormID> out;
            for (auto* it = a_rank->perkConditions.head; it; it = it->next) {
                const auto& d = it->data;
                if (d.functionData.function.get() != RE::FUNCTION_DATA::FunctionID::kHasPerk)
                    continue;
                if (d.flags.isOR)  continue;   // OR'd — not a hard requirement
                if (d.flags.global) continue;  // global-compared — ambiguous, skip
                const float v = d.comparisonValue.f;
                const bool isReq =
                    (d.flags.opCode == Op::kEqualTo && v == 1.0f) ||
                    (d.flags.opCode == Op::kGreaterThanOrEqualTo && v > 0.0f && v <= 1.0f) ||
                    (d.flags.opCode == Op::kGreaterThan && v >= 0.0f && v < 1.0f);
                if (!isReq) continue;
                auto* form = static_cast<RE::TESForm*>(d.functionData.params[0]);
                if (form && std::find(out.begin(), out.end(), form->GetFormID()) == out.end())
                    out.push_back(form->GetFormID());
            }
            return out;
        }

        // ── the §3 rank classifier ──────────────────────────────────────────

        // kAbility is effective by default; "clearly player-gated" means
        // EVERY effect of the ability is behind the narrow player-pin above.
        // No effects at all is NOT clearly player-gated — stays effective
        // (conservative, per the design).
        bool AbilityPlayerGated(RE::SpellItem* a_spell) {
            if (a_spell->effects.empty()) return false;
            for (auto* eff : a_spell->effects)
                if (eff && !ConditionPlayerGated(eff->conditions))
                    return false;
            return true;
        }

        // A rank is effective iff >=1 entry is effective. a_outWhy is only
        // meaningful when the verdict is NOT effective — it names every
        // non-effective entry so the dump can explain the filter.
        Verdict ClassifyRank(RE::BGSPerk* a_rank, std::string& a_outWhy) {
            bool anyMarginal = false;
            std::string why;
            int n = 0;
            for (auto* entry : a_rank->perkEntries) {
                if (!entry) continue;
                ++n;
                // int compares hedge GetType()'s exact return type across
                // CommonLib revisions (the ProgProbe idiom, CI-proven).
                const int t = static_cast<int>(entry->GetType());
                if (t == static_cast<int>(RE::PERK_ENTRY_TYPE::kQuest)) {
                    Append(why, "quest");   // §3: dead, always
                } else if (t == static_cast<int>(RE::PERK_ENTRY_TYPE::kAbility)) {
                    auto* ab = static_cast<RE::BGSAbilityPerkEntry*>(entry)->ability;
                    if (!ab) {
                        Append(why, "ability:<no spell>");
                    } else if (AbilityPlayerGated(ab)) {
                        Append(why, std::format("ability:{}(player-gated)", NameOf(ab)));
                    } else {
                        return Verdict::kEffective;
                    }
                } else if (t == static_cast<int>(RE::PERK_ENTRY_TYPE::kEntryPoint)) {
                    const auto ep = static_cast<std::uint32_t>(
                        static_cast<RE::BGSEntryPointPerkEntry*>(entry)->entryData.entryPoint.get());
                    const auto& row = RowFor(ep);
                    switch (row.verdict) {
                    case Verdict::kEffective:
                        return Verdict::kEffective;
                    case Verdict::kMarginal:
                        anyMarginal = true;
                        Append(why, std::format("{}(marginal)", row.name));
                        break;
                    case Verdict::kDead:
                        Append(why, row.name);
                        break;
                    }
                } else {
                    // An entry type this build has never seen: FLAG it, never
                    // silently kill the perk carrying it.
                    anyMarginal = true;
                    Append(why, std::format("unknownEntryType{}(marginal)", t));
                }
            }
            if (n == 0) {
                // Entry-less perks: MARGINAL, not dead (round-4 acceptance
                // trace). Requiem implements real perk effects in scripts/DLL
                // keyed on HasPerk — its Cremation/Deep Freeze/Electrostatic
                // Discharge are entry-less yet gate the Mastery tier through
                // BOTH tree edges and HasPerk perkConditions. Filtering them
                // made that tier permanently unattainable (the conditions are
                // the authority and the marker was unallocatable). Marginal =
                // takeable, drawn as a dimmed passthrough when an effective
                // descendant needs it — the player-identical path, one point
                // each, exactly like the player pays.
                a_outWhy = "no entries (script-driven or marker)";
                return Verdict::kMarginal;
            }
            a_outWhy = why;
            return anyMarginal ? Verdict::kMarginal : Verdict::kDead;
        }

        // ── tree walk (§2.1) ────────────────────────────────────────────────

        // BFS from the AVIF root, collecting NODES (the probe collected bare
        // perks; the catalog needs grid/pos/edges too). Visited check is a
        // linear scan — trees are ~100 nodes, and it keeps extra headers out
        // of the TU (the v1.0.8 CI lesson).
        std::vector<RE::BGSSkillPerkTreeNode*> CollectNodes(RE::ActorValueInfo* a_avi) {
            std::vector<RE::BGSSkillPerkTreeNode*> queue;
            if (!a_avi || !a_avi->perkTree) return queue;
            queue.push_back(a_avi->perkTree);
            for (std::size_t qi = 0; qi < queue.size() && queue.size() < 512; ++qi) {
                auto* node = queue[qi];
                if (!node) continue;
                for (auto* child : node->children) {
                    if (!child) continue;
                    if (std::find(queue.begin(), queue.end(), child) == queue.end())
                        queue.push_back(child);
                }
            }
            return queue;
        }

        void BuildSkill(RE::ActorValueInfo* a_avi, RE::ActorValue a_av,
                        const char* a_fallbackName, SkillTree& a_out) {
            a_out.av = a_av;
            a_out.skillName = a_fallbackName;
            if (a_avi) {
                const char* n = a_avi->GetFullName();
                if (n && *n) a_out.skillName = n;
            }

            const auto nodes = CollectNodes(a_avi);

            // Pass 1: classify + emit. Remember each kept view's raw node so
            // pass 2 can wire parent edges after every index is known (a
            // parent can be DISCOVERED later than its child when the child is
            // reachable by a shorter path — one pass would miss those edges).
            std::vector<RE::BGSSkillPerkTreeNode*> keptRaw;
            std::unordered_map<const RE::BGSSkillPerkTreeNode*, std::uint32_t> keptIndex;

            for (auto* node : nodes) {
                if (!node || !node->perk) continue;   // the root carries no perk

                PerkNodeView view;
                view.perkFormID = node->perk->GetFormID();
                view.name       = NameOf(node->perk);
                view.gridX      = node->perkGridX;
                view.gridY      = node->perkGridY;
                view.hpos       = node->horizontalPosition;
                view.vpos       = node->verticalPosition;
                {
                    // Description via the TESDescription component (copied
                    // out — value-only, no cached pointer).
                    RE::BSString buf;
                    node->perk->TESDescription::GetDescription(buf, node->perk);
                    view.description = buf.c_str() ? buf.c_str() : "";
                }
                // Rank 1 gates the first take — its condition-authored
                // prereqs are what the layout tiers by (round 4).
                view.condPrereqPerkIDs = ExtractCondPrereqs(node->perk);

                // Ranks: the nextPerk chain. Bounded + cycle-guarded against
                // a malformed overhaul record.
                Verdict     best = Verdict::kDead;
                std::string firstWhy;
                int         rankNo = 0;
                for (RE::BGSPerk* r = node->perk; r && rankNo < 16; r = r->nextPerk, ++rankNo) {
                    if (rankNo > 0 && r == node->perk) break;   // looped chain
                    RankView rank;
                    rank.perkFormID = r->GetFormID();
                    rank.skillReq   = ExtractSkillReq(r, &rank.skillReqAV, &rank.skillReqVal);
                    std::string why;
                    rank.verdict = ClassifyRank(r, why);
                    if (rankNo == 0) firstWhy = why;
                    if (rank.verdict < best) best = rank.verdict;   // kEffective < kMarginal < kDead
                    ++a_out.totalRanks;
                    if (rank.verdict == Verdict::kEffective) ++a_out.effectiveRanks;
                    view.ranks.push_back(std::move(rank));
                }
                view.verdict = best;

                if (best == Verdict::kDead) {
                    // Zero effective (or even marginal) ranks: never reaches
                    // the board. Rank 1's reason explains it in the dump.
                    a_out.filtered.push_back({ view.perkFormID, view.name,
                                               firstWhy.empty() ? "unclassified" : firstWhy });
                    continue;
                }

                keptIndex.emplace(node, static_cast<std::uint32_t>(a_out.nodes.size()));
                keptRaw.push_back(node);
                a_out.nodes.push_back(std::move(view));
            }

            // Pass 2: parent edges, BRIDGED THROUGH FILTERED NODES (deck
            // round 4 — the prereq-order fix). The old wiring recorded a
            // FILTERED direct parent (dead entry-less marker, player-only
            // perk) as the prereq truth: unallocatable through MFO, so its
            // whole subtree was permanently locked, and with no drawable
            // edge its children rendered as root-row orphans — the field
            // read "available order ignores prereqs". Now each parent LINE
            // is resolved through filtered nodes to the nearest KEPT
            // ancestors: those become both the drawable edges and the §5
            // reachability truth. A line that reaches the ROOT through only
            // filtered nodes makes the node root-reachable (parentPerkIDs
            // cleared — the vanilla any-line rule; the perk's own
            // conditions still apply in full at gate time).
            for (std::size_t i = 0; i < keptRaw.size(); ++i) {
                auto& view = a_out.nodes[i];
                bool rootLine = false;
                std::vector<const RE::BGSSkillPerkTreeNode*> stack;
                std::vector<const RE::BGSSkillPerkTreeNode*> seen;
                for (auto* parent : keptRaw[i]->parents) stack.push_back(parent);
                while (!stack.empty()) {
                    const auto* p = stack.back();
                    stack.pop_back();
                    if (!p) continue;
                    if (std::find(seen.begin(), seen.end(), p) != seen.end()) continue;
                    seen.push_back(p);
                    if (!p->perk) { rootLine = true; continue; }   // reached the root
                    if (auto it = keptIndex.find(p); it != keptIndex.end()) {
                        // Kept ancestor — a real, allocatable prereq and a
                        // drawable edge (deduped: several lines can bridge
                        // to the same ancestor).
                        const auto idx = it->second;
                        if (std::find(view.parentIndices.begin(), view.parentIndices.end(),
                                      idx) == view.parentIndices.end()) {
                            view.parentIndices.push_back(idx);
                            view.parentPerkIDs.push_back(a_out.nodes[idx].perkFormID);
                        }
                        continue;
                    }
                    // Filtered — bridge onward through ITS parents.
                    for (auto* gp : p->parents) stack.push_back(gp);
                }
                if (rootLine) view.parentPerkIDs.clear();   // reachable like tier 1
            }

            // Pass 2b: CONDITION-ENCODED prereqs become real graph edges too
            // (marth deck 2026-08-15: "if it's gated on a perk it needs to be
            // AFTER the perk in the tree"). A perk whose perkConditions carry
            // HasPerk <P> (the §2.3 condPrereqPerkIDs) had that dependency
            // enforced by §5.2 but invisible to the graph — so it drew with no
            // edge to P, P (often an entry-less Requiem "Mastery" perk kept as
            // MARGINAL) was never surfaced as the needed passthrough, and the
            // gated perk was not tiered below it. We now wire an edge to the
            // KEPT node that carries P at any rank; a FILTERED/absent P is left
            // to §5.2's bypass (it can never gate a follower). Re-populating
            // parentPerkIDs makes §5.1 require the prereq too — consistent with
            // §5.2, which is the final authority regardless.
            for (std::size_t i = 0; i < a_out.nodes.size(); ++i) {
                auto& view = a_out.nodes[i];
                for (const auto cpid : view.condPrereqPerkIDs) {
                    std::uint32_t owner = UINT32_MAX;
                    for (std::size_t j = 0; j < a_out.nodes.size() && owner == UINT32_MAX; ++j) {
                        if (j == i) continue;
                        for (const auto& r : a_out.nodes[j].ranks)
                            if (r.perkFormID == cpid) { owner = static_cast<std::uint32_t>(j); break; }
                    }
                    if (owner == UINT32_MAX) continue;   // filtered/absent — §5.2 bypass owns it
                    if (std::find(view.parentIndices.begin(), view.parentIndices.end(), owner)
                            == view.parentIndices.end()) {
                        view.parentIndices.push_back(owner);
                        view.parentPerkIDs.push_back(a_out.nodes[owner].perkFormID);
                    }
                }
            }
        }

        void BuildCatalog() {
            auto* avl = RE::ActorValueList::GetSingleton();
            if (!avl) {
                spdlog::error("[prog] no ActorValueList singleton — catalog cannot build");
                return;
            }

            g_catalog.skills.reserve(std::size(kSkills));
            for (const auto& sk : kSkills) {
                SkillTree tree;
                BuildSkill(avl->GetActorValue(sk.av), sk.av, sk.name, tree);

                g_catalog.totalRanks     += tree.totalRanks;
                g_catalog.effectiveRanks += tree.effectiveRanks;
                g_catalog.keptPerks      += static_cast<int>(tree.nodes.size());
                g_catalog.filteredPerks  += static_cast<int>(tree.filtered.size());
                for (const auto& n : tree.nodes)
                    if (n.verdict == Verdict::kMarginal) ++g_catalog.marginalPerks;

                g_catalog.skills.push_back(std::move(tree));
            }
            // Publish LAST: everything above is complete before anyone can
            // observe built == true. After this line the catalog is frozen —
            // no code path writes it again this session.
            g_catalog.built = true;
        }

        // ── the [prog] census dump (bProgCatalogDump) ───────────────────────
        // Per skill: node/filter counts, EVERY filtered perk with its reason,
        // and a spot-check of the first few kept nodes (grid/dome coords +
        // per-rank skill-req strings) so the deck log proves the reader saw
        // the real merged trees.
        void DumpCatalog() {
            spdlog::info("[prog] ===== PROGRESSION CATALOG census =====");
            for (const auto& tree : g_catalog.skills) {
                int marginal = 0;
                for (const auto& n : tree.nodes)
                    if (n.verdict == Verdict::kMarginal) ++marginal;
                spdlog::info("[prog] tree {} (\"{}\"): {} kept ({} marginal) / {} filtered | "
                             "ranks {} player / {} follower-effective{}",
                             StaticName(tree.av), tree.skillName,
                             tree.nodes.size(), marginal, tree.filtered.size(),
                             tree.totalRanks, tree.effectiveRanks,
                             (tree.nodes.empty() && tree.filtered.empty()) ? "  [NO TREE on this AVIF]" : "");
                for (const auto& f : tree.filtered)
                    spdlog::info("[prog]   filtered: {} ({:08X}) — {}", f.name, f.perkFormID, f.reason);

                const std::size_t spot = std::min<std::size_t>(tree.nodes.size(), 3);
                for (std::size_t i = 0; i < spot; ++i) {
                    const auto& n = tree.nodes[i];
                    std::string ranks;
                    for (std::size_t r = 0; r < n.ranks.size(); ++r) {
                        Append(ranks, std::format("r{}[{}]\"{}\"", r + 1,
                                                  n.ranks[r].verdict == Verdict::kEffective ? "eff"
                                                  : n.ranks[r].verdict == Verdict::kMarginal ? "marg" : "dead",
                                                  n.ranks[r].skillReq));
                    }
                    // Round 4: parentPerkIDs mirrors the bridged
                    // parentIndices; empty = root-reachable (possibly via a
                    // bridged line). The old "off-board" delta is gone.
                    spdlog::info("[prog]   node[{}] {} ({:08X}){} grid=({},{}) dome=({:.2f},{:.2f}) "
                                 "parents={}{} condPrereqs={} ranks: {}",
                                 i, n.name, n.perkFormID,
                                 n.verdict == Verdict::kMarginal ? " [MARGINAL]" : "",
                                 n.gridX, n.gridY, n.hpos, n.vpos,
                                 n.parentIndices.size(),
                                 n.parentPerkIDs.empty() ? " (root-reachable)" : "",
                                 n.condPrereqPerkIDs.size(),
                                 ranks);
                }
            }
            const double ratio = g_catalog.totalRanks > 0
                                     ? static_cast<double>(g_catalog.effectiveRanks) / g_catalog.totalRanks
                                     : 0.0;
            spdlog::info("[prog] census: {} perks kept ({} marginal), {} filtered | ranks {} player / "
                         "{} follower-effective (§15 scarcity ratio {:.2f})",
                         g_catalog.keptPerks, g_catalog.marginalPerks, g_catalog.filteredPerks,
                         g_catalog.totalRanks, g_catalog.effectiveRanks, ratio);
        }

    }

    bool Detected() { return g_detected; }

    const std::vector<AddonRef>& Addons() { return g_addons; }

    const Catalog& Get() { return g_catalog; }

    void Init() {
        // ── §18.6 REGISTRATION: enumerate addon manifests ───────────────────
        // An addon = ONE BGSListForm whose FIRST entry is MFO.esp's sentinel
        // keyword (Forms::g_addonSentinel). Walking the data handler's FLST
        // array sees the merged load order, so N addons register without a
        // shared injection point (a common FormList would collide between
        // addons — each owns its manifest outright). Runs AFTER Forms::Resolve
        // (plugin.cpp order); sentinel unresolved = MFO.esp itself is broken.
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            spdlog::error("[prog] TESDataHandler unavailable — addon enumeration skipped");
        } else if (!Forms::g_addonSentinel) {
            spdlog::error("[prog] MFO_AddonManifest sentinel unresolved — addon enumeration skipped");
        } else {
            for (auto* list : dh->GetFormArray<RE::BGSListForm>()) {
                if (!list || list->forms.empty()) continue;
                if (list->forms.front() != Forms::g_addonSentinel) continue;
                AddonRef ref;
                ref.manifestID = list->GetFormID();
                if (auto* file = list->GetFile(0))
                    ref.plugin = std::string(file->GetFilename());
                spdlog::info("[prog] addon manifest {:08X} registered (from \"{}\", {} entr(ies))",
                             ref.manifestID, ref.plugin, list->forms.size());
                g_addons.push_back(std::move(ref));
            }
            g_detected = !g_addons.empty();
            if (!g_detected) {
                // Absent = feature off with ONE named line, never an error
                // (§1, the Forms failure doctrine — addons are optional).
                spdlog::info("[prog] no addon manifests found — progression off");
            }
        }

        // ── catalog build ───────────────────────────────────────────────────
        // The dump toggle also FORCES the build: the ESL does not exist yet,
        // so this is the only way to field-verify the reader on the deck
        // (dev-only, INI-only, default OFF — the bProgProbe precedent).
        const bool dump = Config::g_progCatalogDump.load();
        if (!g_detected && !dump) return;   // no consumer this session

        BuildCatalog();
        if (g_catalog.built)
            spdlog::info("[prog] catalog built: {} skill trees, {} perks kept ({} marginal), "
                         "{} filtered — frozen (lock-free reads from here on)",
                         g_catalog.skills.size(), g_catalog.keptPerks,
                         g_catalog.marginalPerks, g_catalog.filteredPerks);
        if (dump && g_catalog.built) DumpCatalog();
    }

}
