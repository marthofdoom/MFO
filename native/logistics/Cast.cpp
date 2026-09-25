// Logistics_Cast.cpp -- the CAST / MAGE-IDENTITY helpers of the logistics
// family (split mechanically out of Logistics.cpp, no logic change): the
// gambit-driven magic-user signals (TargetMagicSchool, HasCastGambit,
// IsCasterFollower, IsNecromancerFollower, TopTwoSchoolMask), the school
// name/keyword classifiers, and the auto tome-learn pass. NOTE: the OOC
// cast DISPATCH itself (pacing via g_logiCastUntil, concentration direct
// force, fire-and-forget) lives inside ServiceFollower in Logistics.cpp.
#include "Logistics_internal.h"

namespace MFO::Logistics {

        // ── MAGIC LOADOUT (v1.0.29): the mage's loot preferences ────────────
        // A follower is a MAGIC USER iff he has at least one ENABLED cast
        // gambit (act.cast_self / act.cast_target) in his combat table --
        // GAMBIT-DRIVEN like every other role decision in this file (marth:
        // role is what the table says, never a skill guess; a battlemage with
        // only attack/equip gambits is NOT a magic user however high his
        // Destruction). His TARGET SCHOOL is the most-represented school among
        // those gambits' spells -- each spell's school read from its costliest
        // effect's base MGEF "Magic Skill" field. Pure form-DATA reads (no
        // 3D/physics), so this is safe on the logistics worker. No cast
        // gambits -> a_castGambits stays 0 -> the whole feature is inert for
        // him (unchanged behavior).
        RE::ActorValue TargetMagicSchool(const FollowerState& a_state, int& a_castGambits) {
            constexpr int kNumSchools = 5;
            static constexpr RE::ActorValue kSchools[kNumSchools] = {
                RE::ActorValue::kAlteration, RE::ActorValue::kConjuration,
                RE::ActorValue::kDestruction, RE::ActorValue::kIllusion,
                RE::ActorValue::kRestoration,
            };
            a_castGambits = 0;
            int tally[kNumSchools] = {};
            for (const auto& g : a_state.combat()) {
                if (!g.enabled) continue;   // a toggled-OFF rule doesn't make a mage (same rule as TableHasAction)
                if (g.actionOpcode != Vocab::kActCastSelf &&
                    g.actionOpcode != Vocab::kActCastTarget &&
                    g.actionOpcode != Vocab::kActCastPlayer) continue;
                ++a_castGambits;   // counts even when the spell's school below is unreadable
                auto* spell = g.actionParamForm
                    ? RE::TESForm::LookupByID<RE::SpellItem>(g.actionParamForm) : nullptr;
                if (!spell) continue;
                const auto* eff  = spell->GetCostliestEffectItem();
                const auto* mgef = eff ? eff->baseEffect : nullptr;
                if (!mgef) continue;
                // data.associatedSkill IS the MGEF record's "Magic Skill" (the
                // school); non-school spells (kNone) simply don't tally.
                const auto school = mgef->data.associatedSkill;
                for (int i = 0; i < kNumSchools; ++i)
                    if (school == kSchools[i]) { ++tally[i]; break; }
            }
            // Most-represented school wins ("weighted by the gambits set" --
            // marth); a tie goes to the first-listed, which is stable across
            // ticks so the preference never flip-flops between two schools.
            int best = -1, bestN = 0;
            for (int i = 0; i < kNumSchools; ++i)
                if (tally[i] > bestN) { bestN = tally[i]; best = i; }
            return best >= 0 ? kSchools[best] : RE::ActorValue::kNone;
        }

        // ── #21 economy: caster signals (gambit-driven, MFO's existing "is a magic
        //    user" definition -- the same signal TargetMagicSchool/mageMode use). ──
        // Does this follower author ANY enabled cast gambit? The tome-buy + auto-
        // learn mage gate (criteria #2/#3 collapse to this one signal by reuse).
        bool HasCastGambit(const FollowerState& a_state) {
            for (const auto& g : a_state.combat()) {
                if (!g.enabled) continue;
                if (g.actionOpcode == Vocab::kActCastSelf ||
                    g.actionOpcode == Vocab::kActCastTarget ||
                    g.actionOpcode == Vocab::kActCastPlayer) return true;
            }
            return false;
        }

        // Is this a NECROMANCER, detected PRINCIPLED (never by name)? True when an
        // enabled cast gambit's spell carries a Reanimate effect (raise-dead /
        // summon-undead). Used only to let a necromancer follower buy the villain-
        // coded robes the general mage-apparel buy refuses.
        bool IsNecromancerFollower(const FollowerState& a_state) {
            for (const auto& g : a_state.combat()) {
                if (!g.enabled) continue;
                if (g.actionOpcode != Vocab::kActCastSelf &&
                    g.actionOpcode != Vocab::kActCastTarget &&
                    g.actionOpcode != Vocab::kActCastPlayer) continue;
                auto* spell = g.actionParamForm
                    ? RE::TESForm::LookupByID<RE::SpellItem>(g.actionParamForm) : nullptr;
                if (!spell) continue;
                for (const auto* e : spell->effects) {
                    const auto* mgef = e ? e->baseEffect : nullptr;
                    if (mgef && mgef->data.archetype ==
                                RE::EffectArchetypes::ArchetypeID::kReanimate) return true;
                }
            }
            return false;
        }

        // Is this follower a MAGE-BUILD caster for gear/tome purposes? The gambit
        // signal, matching the loot judge's mageMode (magic loadout ON and >=1 cast
        // gambit). combatClassOverride is NOT consulted here (marth: it governs
        // WEAPON selection only; apparel is governed by bMageWearRobes). Shared by
        // the BUY apparel gate so loot and buy agree on "is a caster".
        bool IsCasterFollower(const FollowerState& a_state) {
            return Config::g_magicLoadout.load() && HasCastGambit(a_state);
        }

        // Bitmask of the follower's TOP 2 magic-school skills (criterion #4), in the
        // fixed 0=Alt 1=Conj 2=Dest 3=Illu 4=Rest bit order SpellSchoolBit returns.
        // A pure skill read -- worker-safe on a loaded follower.
        std::uint8_t TopTwoSchoolMask(RE::Actor* a_follower) {
            auto* avo = a_follower ? a_follower->AsActorValueOwner() : nullptr;
            if (!avo) return 0;
            static constexpr RE::ActorValue kBySchoolBit[5] = {
                RE::ActorValue::kAlteration, RE::ActorValue::kConjuration,
                RE::ActorValue::kDestruction, RE::ActorValue::kIllusion,
                RE::ActorValue::kRestoration,
            };
            float lv[5];
            for (int i = 0; i < 5; ++i) lv[i] = avo->GetActorValue(kBySchoolBit[i]);
            int hi = 0;
            for (int i = 1; i < 5; ++i) if (lv[i] > lv[hi]) hi = i;
            int lo = -1;
            for (int i = 0; i < 5; ++i) {
                if (i == hi) continue;
                if (lo < 0 || lv[i] > lv[lo]) lo = i;
            }
            std::uint8_t mask = static_cast<std::uint8_t>(1u << hi);
            if (lo >= 0) mask |= static_cast<std::uint8_t>(1u << lo);
            return mask;
        }

        // Learn (and consume) any spell tome the follower CARRIES whose spell it can
        // cast and does not yet know -- the copy of Board.cpp's teach primitive on
        // the worker (AddSpell/RemoveItem are edit-drain-safe, §0.32; NEVER
        // MainThread::Post). Also learns a tome the PLAYER hands the follower. One
        // tome per tick: the first AddSpell/RemoveItem mutates the live inventory,
        // so we return after it and catch the rest next tick. Caller gates on the
        // follower being a mage (HasCastGambit) -- only mages auto-consume tomes.
        void LearnCarriedTomes(RE::Actor* a_follower) {
            if (!a_follower) return;
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* book = obj->As<RE::TESObjectBOOK>();
                if (!book || !book->TeachesSpell()) continue;
                auto* sp = book->data.teaches.spell;
                if (!sp || !Vocab::IsCastableSpell(sp)) continue;
                if (Catalog::IsExcluded(book->GetFormID())) continue;
                if (a_follower->HasSpell(sp)) continue;
                a_follower->AddSpell(sp);
                a_follower->RemoveItem(book, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                spdlog::info("[econ] {:08X} learned spell {:08X} from carried tome {:08X}",
                             a_follower->GetFormID(), sp->GetFormID(), book->GetFormID());
                return;   // one per tick -- the mutation ends this inventory walk
            }
        }

        const char* SchoolName(RE::ActorValue a_school) {
            using AV = RE::ActorValue;
            switch (a_school) {
            case AV::kAlteration:  return "Alteration";
            case AV::kConjuration: return "Conjuration";
            case AV::kDestruction: return "Destruction";
            case AV::kIllusion:    return "Illusion";
            case AV::kRestoration: return "Restoration";
            default:               return "none";
            }
        }

        // Which SCHOOL does this actor value boost, if any? Vanilla apparel
        // does NOT fortify the skill AV itself: the "Fortify Destruction"
        // enchantment (spells cost -X%) modifies <School>Modifier, and the
        // "stronger spells" effects use <School>PowerModifier -- so all three
        // AVs map back to the school. Mods use any of the three; matching by
        // AV (never by name) is the §4.8.2 derived-vocabulary principle.
        RE::ActorValue SchoolOfBoostAV(RE::ActorValue a_av) {
            using AV = RE::ActorValue;
            switch (a_av) {
            case AV::kAlteration:  case AV::kAlterationModifier:  case AV::kAlterationPowerModifier:  return AV::kAlteration;
            case AV::kConjuration: case AV::kConjurationModifier: case AV::kConjurationPowerModifier: return AV::kConjuration;
            case AV::kDestruction: case AV::kDestructionModifier: case AV::kDestructionPowerModifier: return AV::kDestruction;
            case AV::kIllusion:    case AV::kIllusionModifier:    case AV::kIllusionPowerModifier:    return AV::kIllusion;
            case AV::kRestoration: case AV::kRestorationModifier: case AV::kRestorationPowerModifier: return AV::kRestoration;
            default:               return AV::kNone;
            }
        }

        // Case-insensitive ASCII substring -- keyword editorIDs are plain
        // ASCII ("MagicSchool_Destruction", "MAG_DestructionRobes", ...). No
        // locale, no allocation.
        bool ContainsNoCase(const char* a_hay, const char* a_needle) {
            if (!a_hay || !a_needle || !*a_needle) return false;
            for (const char* h = a_hay; *h; ++h) {
                const char* a = h;
                const char* b = a_needle;
                while (*a && *b &&
                       std::tolower(static_cast<unsigned char>(*a)) ==
                       std::tolower(static_cast<unsigned char>(*b))) { ++a; ++b; }
                if (!*b) return true;
            }
            return false;
        }

        // LAST-DITCH school read: a keyword whose editorID NAMES a school.
        // Some mods tag their robes/effects ("MagicSchool_Destruction",
        // "DestructionRobes") instead of -- or as well as -- wiring school
        // AVs, and the school names are distinctive enough that a substring
        // match is safe (nothing vanilla or common collides with e.g.
        // "conjuration" outside actual school tagging). Keyword arrays are
        // static form DATA -> worker-safe. This bends §4.8.2's never-by-name
        // rule deliberately and LAST: every AV read gets first refusal, and a
        // robe we'd otherwise misjudge as junk is worse than a name match.
        RE::ActorValue SchoolFromKeywords(const RE::BGSKeywordForm* a_kwf) {
            if (!a_kwf || !a_kwf->keywords) return RE::ActorValue::kNone;
            static constexpr std::pair<const char*, RE::ActorValue> kNames[] = {
                { "alteration",  RE::ActorValue::kAlteration  },
                { "conjuration", RE::ActorValue::kConjuration },
                { "destruction", RE::ActorValue::kDestruction },
                { "illusion",    RE::ActorValue::kIllusion    },
                { "restoration", RE::ActorValue::kRestoration },
            };
            for (std::uint32_t i = 0; i < a_kwf->numKeywords; ++i) {
                const auto* kw = a_kwf->keywords[i];
                const char* ed = kw ? kw->GetFormEditorID() : nullptr;
                if (!ed) continue;
                for (const auto& [name, school] : kNames)
                    if (ContainsNoCase(ed, name)) return school;
            }
            return RE::ActorValue::kNone;
        }

        // Comma-join a form's keyword editorIDs for the diagnostic dump below.
        std::string KeywordCsv(const RE::BGSKeywordForm* a_kwf) {
            std::string out;
            if (!a_kwf || !a_kwf->keywords) return out;
            for (std::uint32_t i = 0; i < a_kwf->numKeywords; ++i) {
                const auto* kw = a_kwf->keywords[i];
                const char* ed = kw ? kw->GetFormEditorID() : nullptr;
                if (!ed || !*ed) continue;
                if (!out.empty()) out += ',';
                out += ed;
            }
            return out;
        }

        // The school ONE beneficial effect boosts, or kNone -- the v1.0.31
        // detection chain. v1.0.29 read ONLY data.primaryAV, which is where
        // VANILLA fortify-school enchantments put the AV -- but marth's
        // modlist (LoreRim) re-authors robe enchantments, and the deck log
        // showed Marcurio's valid Destruction robe scoring 0 (never preferred)
        // while a plain circlet won as "best". The school is now the FIRST of
        // these that resolves (all pure form-DATA reads, worker-safe):
        //   (a) primaryAV                -- the vanilla wiring (unchanged);
        //   (b) associatedSkill          -- the MGEF "Magic Skill" field, the
        //       SAME read TargetMagicSchool already trusts on the spell side;
        //   (c) secondaryAV, only when the archetype really modifies values
        //       (Value/DualValue/PeakValue Modifier -- there the second AV is
        //       a genuine target, elsewhere it is noise);
        //   (d) an MGEF keyword naming the school (SchoolFromKeywords above).
        RE::ActorValue EffectBoostSchool(const RE::EffectSetting* a_mgef) {
            if (!a_mgef) return RE::ActorValue::kNone;
            if (auto s = SchoolOfBoostAV(a_mgef->data.primaryAV); s != RE::ActorValue::kNone) return s;
            if (auto s = SchoolOfBoostAV(a_mgef->data.associatedSkill); s != RE::ActorValue::kNone) return s;
            using Arch = RE::EffectArchetypes::ArchetypeID;
            const auto arch = a_mgef->data.archetype;
            if (arch == Arch::kValueModifier || arch == Arch::kDualValueModifier ||
                arch == Arch::kPeakValueModifier)
                if (auto s = SchoolOfBoostAV(a_mgef->data.secondaryAV); s != RE::ActorValue::kNone) return s;
            return SchoolFromKeywords(a_mgef);
        }
}
