// Logistics_Loot_Equipment.cpp -- the EQUIPMENT judge, split out of
// Logistics_Loot.cpp (2500-line hard rule, marth CLAUDE.md 2026-08-31; pure
// mechanical move, no logic change). Owns: the role/mage-mode context
// (EquipmentContext/BuildEquipmentContext), the container-scan judge
// (LootEquipment) and its route-2b loose-item twin (LooseEquipmentQualifies,
// which shares the SAME context + per-item predicate so a bare sword on the
// ground is judged identically to one in a corpse's pack -- see LootNearby,
// Logistics_Loot.cpp). Everything here reads/writes only through
// Logistics_internal.h's shared substrate (g_svc et al.) and the small
// cross-module helpers declared there (ArmorIsBetter, CarriesSlotArmorAtLeast,
// ComputeWeaponRoles, WornInLogicalSlot, IsCreatureWeapon/IsCreatureArmor,
// AcquireEquip) -- all still defined in Logistics_Loot.cpp.
#include "Logistics_internal.h"

namespace MFO::Logistics {

        // Equipment-loot judging context: the follower's combat role/mage-mode
        // gate plus the baselines any weapon/armor candidate must beat, all
        // computed ONCE from the follower's OWN gear (see the long
        // rationale inlined in LootEquipment below -- unchanged, just moved
        // here). Shared by LootEquipment's container scan AND
        // LooseEquipmentQualifies (route 2b, LootNearby) so a bare sword on
        // the ground is judged by the EXACT same rule as one in a corpse's
        // pack -- ONE decision path, two call sites.

        EquipmentContext BuildEquipmentContext(RE::Actor* a_follower) {
            EquipmentContext ctx;
            using WT = RE::WEAPON_TYPE;

            // THE ROLE WE LOOT/KEEP is a STABLE signal, never the momentarily
            // WIELDED weapon (#69, ComputeWeaponRoles above): a gambit-driven
            // role first, else whatever the follower actually CARRIES. Loot and
            // ShedOffRoleWeapon judge the SAME roles from the SAME helper, so
            // they can no longer disagree about what's "off-role" -- that
            // disagreement was the Gauldurbow bug (a custom follower's own
            // weapon, drawn only sometimes, read as off-role and got handed to
            // the player) and the hybrid-1h-gets-shed bug. WHAT gets force-
            // EQUIPPED over a drawn weapon is still decided by LootEquipment's
            // equipIt gate, UNCHANGED -- roles only decide what's looted/kept.
            const bool wantsRanged = g_svc && TableHasAction(g_svc->combat(), Vocab::kActEquipRanged);
            const bool wantsMelee  = g_svc && TableHasAction(g_svc->combat(), Vocab::kActEquipMelee);
            const WeaponRoles roles = g_svc ? ComputeWeaponRoles(a_follower, *g_svc) : WeaponRoles{};
            ctx.wantsRanged = wantsRanged;
            ctx.wantsMelee  = wantsMelee;
            // Base class (#65 combatClassOverride; 1=Melee 2=Ranged 3=Mage, 0=Auto)
            // read here, BEFORE mageMode, so mageMode can gate on it directly --
            // no circular dependency on the roles derived further down.
            const std::uint8_t baseClass = g_svc ? g_svc->combatClassOverride : 0;

            // ── MAGIC LOADOUT (v1.0.29) ─────────────────────────────────────
            // Gambit-driven magic-user detection. A magic user gets two loot
            // paths: (1) SCHOOL-SCORED APPAREL on the mage slots (bypasses
            // ArmorIsBetter's rating>0 gate, which can never judge a robe); and
            // (2) ONE one-handed melee BACKUP (daggers by default) his AI draws
            // at zero magicka. Detected BEFORE the role below, because a pure
            // caster must be kept OUT of the general weapon-upgrade role.
            int castGambits = 0;
            RE::ActorValue school = RE::ActorValue::kNone;
            if (Config::g_magicLoadout.load() && g_svc)
                school = TargetMagicSchool(*g_svc, castGambits);
            ctx.school      = school;
            ctx.castGambits = castGambits;
            // mageMode requires the follower be PRIMARILY a caster, not just
            // carrying a secondary cast gambit (marth: a Ranged/Melee follower
            // with a cast gambit must keep his class loadout, not flip to
            // school-scored mage apparel). The CLASS is the authority here
            // (matching meleeTargetClass's !baseWeaponUser gate just below):
            // a base Ranged/Melee follower (baseClass 1/2) is NEVER mageMode,
            // no matter what gambits she carries -- an act.attack-only Ranged
            // follower has no equip_ranged GAMBIT, so a gambit-composition
            // test alone (no equip-melee/equip-ranged gambit) would wrongly
            // call her mageMode. "Primarily a caster" = base class Mage (3),
            // OR -- for Auto/no explicit class (0) only -- no melee/ranged
            // attack gambit at all (a pure caster who never picked a class).
            const bool classIsMage          = baseClass == 3;
            const bool hasNoWeaponAttackRole = !wantsMelee && !wantsRanged;
            const bool mageMode    = castGambits > 0 &&
                (classIsMage || (baseClass == 0 && hasNoWeaponAttackRole));
            ctx.daggersOnly = Config::g_mageDaggersOnly.load();
            // #21 bMageWearRobes (default ON): the mage school-clothing dress-up gate.
            // When OFF, a magic user is treated like any other class for APPAREL and
            // falls through to the plain rating armor judge below (marth). It gates
            // ONLY apparel selection -- the mage still keeps the backup-weapon /
            // no-melee-role contract (mageMode) and still buys/learns tomes.
            ctx.useMageApparel = mageMode && Config::g_mageWearRobes.load();
            // #21 UNIFIED mage-apparel ranking (loot side; shared with the buy side).
            // Same MEO-aware model: value-primary when MEO carries gems, else school-
            // enchant primary; villain blacklist with a necromancer exception; all
            // clothing + jewelry slots (MageClothingSlot). See MageApparelBuyKey.
            ctx.mageTop2          = ctx.useMageApparel ? TopTwoSchoolMask(a_follower) : 0;
            ctx.mageSchoolPrimary = !MEOBridge::Available() || Config::g_mageApparelStrictSchool.load();
            ctx.mageAllowVillain  = ctx.useMageApparel && g_svc && IsNecromancerFollower(*g_svc);

            // The MELEE class we loot/upgrade, or Other = "no melee role at all".
            // #69: ComputeWeaponRoles hands a role to ANY carried melee/ranged
            // weapon, but a non-battlemage magic user must NOT take the general
            // weapon-upgrade path -- his melee is the ONE-sidearm backup contract
            // (#52/#54: daggers-only, never an armory), not a role, and he loots
            // no bow either. Only an explicit equip-melee/equip-ranged gambit (a
            // battlemage/spellbow) earns him the real role. Pre-#69 this fell out
            // for free -- the role was WIELD-based and a caster wields spells/
            // staff, never his carried sidearm -- so the stable carries-based
            // signal has to say it explicitly. (Shed still KEEPS his carried gear;
            // this only bars LOOTING new weapon upgrades for a pure caster.)
            // marth: the BASE CLASS decides the melee-loot contract, NOT the mere
            // presence of an equip-melee gambit. A base MAGE (Cast class, #65
            // combatClassOverride==3) keeps the daggers-only sidearm even when he
            // melees via a gambit -- "a base mage who melees", not a spellsword.
            // A base WARRIOR/ARCHER who casts (Melee/Ranged class == a spellsword)
            // earns the full weapon-upgrade role. Auto (0, no explicit class)
            // keeps the old gambit heuristic (mageMode && !wantsMelee). Ordinals
            // match CombatStyle::Stance by construction (State.h:82).
            // baseClass read earlier now (mageMode gates on it directly, above).
            const bool baseCaster          = baseClass == 3;
            const bool baseWeaponUser      = baseClass == 1 || baseClass == 2;
            ctx.meleeTargetClass =
                (mageMode && !baseWeaponUser && (baseCaster || !wantsMelee))
                    ? WepClass::Other : roles.melee;
            // Ranged is a primary if a gambit wants it OR they carry one.
            ctx.doRanged =
                (mageMode && !wantsRanged) ? false           : roles.doRanged;
            ctx.wantBackup  = mageMode && ctx.meleeTargetClass == WepClass::Other;

            // Baseline the MELEE/RANGED upgrade must beat: the best in-role
            // weapon anywhere in the follower's OWN INVENTORY (the equipped one
            // is part of it). Equipped-only was the residual thrash hole: a
            // follower HOLDING HIS BOW with an equip-melee gambit baselined
            // melee at 0, so every corpse's iron dagger "beat" the good sword
            // already in his pack -- looted a duplicate and force-equipped it
            // over the bow, corpse after corpse (the in-AND-out-of-combat half
            // of "Erik switches weapons for no reason": the combat gambit put
            // the bow back, the next corpse knocked it out again). BOW vs
            // CROSSBOW is decided by ComputeWeaponRoles now (#69, shared with
            // the shed side); this pass only baselines the follower's best
            // ALREADY-CARRIED weapon of that kind. Creature/excluded weapons
            // are unusable gear -- they never set a baseline.
            ctx.wantCrossbow = roles.wantCrossbow;
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* w = obj->As<RE::TESObjectWEAP>();
                if (!w || IsCreatureWeapon(w) ||
                    (!Config::g_lootSpecialItems.load() && Catalog::IsExcluded(obj->GetFormID()))) continue;
                if (ctx.meleeTargetClass != WepClass::Other &&
                    WeaponClassOf(w->GetWeaponType()) == ctx.meleeTargetClass)
                    ctx.baseDmg = std::max(ctx.baseDmg, w->GetAttackDamage());
                // ONE sidearm is the mage-backup contract: he restocks only
                // when he carries NONE, never accumulates an armory (creature/
                // excluded weapons already skipped above -- an unusable weapon
                // is not a backup).
                if (ctx.wantBackup && WeaponClassOf(w->GetWeaponType()) == WepClass::OneHand &&
                    (!ctx.daggersOnly || w->GetWeaponType() == WT::kOneHandDagger))
                    ctx.myBackupDmg = std::max(ctx.myBackupDmg, w->GetAttackDamage());
                if (ctx.doRanged && w->GetWeaponType() == (ctx.wantCrossbow ? WT::kCrossbow : WT::kBow))
                    ctx.myRangedDmg = std::max(ctx.myRangedDmg, w->GetAttackDamage());
            }
            return ctx;
        }

        // ROUTE 2b GENERALIZATION (marth: "act.loot_equipment should pick up a
        // loose weapon/armor too, same rule as a container"). Does a single
        // LOOSE weapon/armor item qualify under the EXACT rule LootEquipment
        // applies inside a container scan? For a lone candidate, the container
        // loop's "beats the running best" comparison collapses to "beats the
        // follower's own baseline" -- bestArmorRat/bestWeapDmg/bestRangedDmg/
        // bestBackupDmg all START at that baseline; a SECOND competing item in
        // the SAME container is what would raise the bar further, and that
        // does not apply to one item sitting alone on the floor. Mirrors
        // LootEquipment's armor/weapon branches verbatim, one candidate at a
        // time -- keep both in lockstep: a rule added there for a container
        // item needs the same rule here for a loose one, or "the SAME rule,
        // two sources" breaks quietly. NEVER-LOOT (quest/catalog) gating is
        // the CALLER's job (LooseSpecialItemBlocked, LootNearby) -- same split
        // LootEquipment itself uses (gate before the branch, not inside it).
        bool LooseEquipmentQualifies(RE::Actor* a_follower, RE::TESBoundObject* a_obj,
                                     const EquipmentContext& ctx) {
            using WT = RE::WEAPON_TYPE;
            if (auto* armo = a_obj->As<RE::TESObjectARMO>()) {
                if (IsCreatureArmor(armo)) return false;
                if (Config::g_dollsMode.load()) return false;   // #61 FASHIONRIM
                const bool isShield = (static_cast<std::uint32_t>(armo->GetSlotMask())
                    & static_cast<std::uint32_t>(RE::BGSBipedObjectForm::BipedObjectSlot::kShield)) != 0;
                const bool shieldUseless = isShield && !(ctx.meleeTargetClass == WepClass::OneHand && !ctx.doRanged);
                if (ctx.useMageApparel && !isShield) {
                    const int slot = MageClothingSlot(armo);
                    int cTier = 0; std::int32_t cMetric = 0;
                    if (slot >= 0 && slot <= 3 &&
                        MageApparelBuyKey(armo, ctx.mageTop2, ctx.mageSchoolPrimary, ctx.mageAllowVillain, cTier, cMetric)) {
                        int wTier = 0; std::int32_t wMetric = 0;
                        if (auto* worn = WornInLogicalSlot(a_follower, slot))
                            MageApparelBuyKey(worn, ctx.mageTop2, ctx.mageSchoolPrimary, /*allowVillain*/true, wTier, wMetric);
                        return cTier > wTier || (cTier == wTier && cMetric > wMetric);
                    }
                    return false;   // mage mode never falls through to plain rating
                }
                // The PLAIN rating path is for NON-magic users ONLY (see
                // LootEquipment's identical gate for the marth v1.0.31 story).
                return !ctx.useMageApparel && !shieldUseless && ArmorIsBetter(a_follower, armo) &&
                       armo->GetArmorRating() > 0.0f &&
                       !CarriesSlotArmorAtLeast(a_follower, armo);
            }
            if (auto* weap = a_obj->As<RE::TESObjectWEAP>()) {
                if (IsCreatureWeapon(weap)) return false;   // never equip automaton/creature gear
                const WepClass wc = WeaponClassOf(weap->GetWeaponType());
                if (ctx.meleeTargetClass != WepClass::Other && wc == ctx.meleeTargetClass &&
                    weap->GetAttackDamage() > ctx.baseDmg)
                    return true;
                if (ctx.doRanged) {
                    const auto wt = weap->GetWeaponType();
                    const bool kindMatch = ctx.wantCrossbow ? (wt == WT::kCrossbow) : (wt == WT::kBow);
                    if (kindMatch && weap->GetAttackDamage() > ctx.myRangedDmg) return true;
                }
                if (ctx.wantBackup && wc == WepClass::OneHand &&
                    (!ctx.daggersOnly || weap->GetWeaponType() == WT::kOneHandDagger) &&
                    weap->GetAttackDamage() > ctx.myBackupDmg)
                    return true;
                return false;
            }
            return false;
        }

        bool LootEquipment(RE::Actor* a_follower, RE::TESObjectREFR* a_src, bool a_peek) {
            // Generalized by CATEGORY, never by item (§4.8.2). One better piece
            // per CALL (one action per tick, §4.3; StripCorpse drains by calling
            // again -- each take raises the inventory baseline). We TRANSFER,
            // then EQUIP IN PLACE (see the equipIt gate below) -- a real person
            // who finds a better cuirass puts it on, they do not just
            // carry it (marth: the follower is a thinking person). Safe here
            // because logistics runs OUT of combat, where Loadout is not holding
            // a hand for a cast, and MFO has no equip-event sink to loop on;
            // armor slots are independent of the (left-hand) spell hand.
            auto* equippedWeap = a_follower->GetEquippedObject(false);
            auto* myWeap       = equippedWeap ? equippedWeap->As<RE::TESObjectWEAP>() : nullptr;
            using WT = RE::WEAPON_TYPE;

            // Context extraction (marth, route-2b generalization pass): the
            // role/mage-mode gate and the follower's-own-gear baselines used to
            // be computed inline here; they now live in BuildEquipmentContext
            // (shared with LooseEquipmentQualifies, above) so a loose item and a
            // container item are judged by the IDENTICAL rule. Pure extraction
            // -- every name below is aliased back to its old local so the rest
            // of this function (the container scan + best-pick + acquire +
            // diagnostics) is untouched.
            const EquipmentContext ctx = BuildEquipmentContext(a_follower);
            const WepClass       meleeTargetClass  = ctx.meleeTargetClass;
            const bool            doRanged         = ctx.doRanged;
            const bool            wantCrossbow     = ctx.wantCrossbow;
            const bool            wantBackup       = ctx.wantBackup;
            const bool            daggersOnly      = ctx.daggersOnly;
            const bool            useMageApparel   = ctx.useMageApparel;
            const std::uint8_t    mageTop2         = ctx.mageTop2;
            const bool            mageSchoolPrimary= ctx.mageSchoolPrimary;
            const bool            mageAllowVillain = ctx.mageAllowVillain;
            const std::uint16_t   baseDmg          = ctx.baseDmg;
            const std::uint16_t   myRangedDmg      = ctx.myRangedDmg;
            const std::uint16_t   myBackupDmg      = ctx.myBackupDmg;
            const bool            wantsMelee       = ctx.wantsMelee;
            const bool            wantsRanged      = ctx.wantsRanged;
            const RE::ActorValue  school           = ctx.school;
            const int             castGambits      = ctx.castGambits;

            RE::TESBoundObject* bestArmor     = nullptr;
            float               bestArmorRat  = 0.0f;   // best-first, like bestWeapDmg (marth's rule)
            RE::TESBoundObject* bestWeap      = nullptr;
            std::uint16_t       bestWeapDmg   = baseDmg;
            RE::TESBoundObject* bestRanged    = nullptr;
            std::uint16_t       bestRangedDmg = myRangedDmg;
            RE::TESBoundObject* bestMage      = nullptr;   // clothing/jewelry apparel (magic user) -- unified MEO-aware judge
            int                 bestMageTier  = 0;         // 2 top-2 school, 1 plain, 0 off-school (see MageApparelBuyKey)
            std::int32_t        bestMageMetric= 0;         // value (+fortify mag for a school piece); higher = fancier
            RE::TESBoundObject* bestBackup    = nullptr;   // the mage's melee sidearm (upgrade past best owned)
            std::uint16_t       bestBackupDmg = myBackupDmg;   // beat his best-OWNED sidearm, not the wielded hand

            for (auto& [obj, data] : a_src->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                // NEVER-LOOT: quest items stay engine-protected regardless of
                // bLootSpecialItems (per-instance alias check). The catalog
                // artifact/unique/no-drop exclusion below is what the toggle
                // governs; default ON lifts it -- leave the rest for the player.
                if (IsQuestObjectInstance(data.second.get())) continue;
                if (!Config::g_lootSpecialItems.load() && Catalog::IsExcluded(obj->GetFormID())) continue;

                if (auto* armo = obj->As<RE::TESObjectARMO>()) {
                    // Never loot a NON-PLAYABLE creature "skin" armor (MNC's
                    // BearBrownSoft et al.): armor-rated but the creature's own
                    // invisible body, useless/broken on a follower. Same flag as
                    // IsCreatureWeapon -- this is what BearBrownSoft slipped past.
                    if (IsCreatureArmor(armo)) continue;
                    // #61 FASHIONRIM: armor-only dolls mode -- skip EVERY armor
                    // candidate (plain-rated AND mage school-robe) so MFO never
                    // loots or swaps a follower's armor; the player dresses them
                    // by hand. Weapons fall to the else-branch below, untouched.
                    if (Config::g_dollsMode.load()) continue;
                    // A SHIELD needs a free off-hand: only a ONE-HAND melee role can
                    // use one. A 2H, ranged, or no-melee-role follower has no hand
                    // for it -- dead weight (marth: Farkas, a two-hander, picked up
                    // a shield).
                    const bool isShield = (static_cast<std::uint32_t>(armo->GetSlotMask())
                        & static_cast<std::uint32_t>(RE::BGSBipedObjectForm::BipedObjectSlot::kShield)) != 0;
                    const bool shieldUseless = isShield && !(meleeTargetClass == WepClass::OneHand && !doRanged);
                    // MAGE APPAREL + JEWELRY (#21 unified with the buy side): a magic
                    // user's dress-up is judged by the shared MEO-aware ranking
                    // (MageApparelBuyKey: value-primary with MEO, else school-enchant
                    // primary), across ALL clothing slots AND jewelry (ring/amulet),
                    // never by armor rating. A candidate must BEAT what he currently
                    // WEARS in that logical slot, then beat the running best pick.
                    // Shields are skipped -- rated armor, never dress-up.
                    if (useMageApparel && !isShield) {
                        LogMageApparelDiag(armo, school);   // one dump per form+school (deduped inside)
                        int cTier = 0; std::int32_t cMetric = 0;
                        const int slot = MageClothingSlot(armo);
                        // CLOTHING slots only here (0 head .. 3 feet). JEWELRY (ring/
                        // amulet, slots 4/5) is NOT acquired via the Equipment loot
                        // category -- it stays on the Valuables tier (LootJewelry, its
                        // stricter dibs preserved). EquipBestOwnedGear still WEARS the
                        // best owned ring/amulet (looted-as-valuable, bought, handed).
                        if (slot >= 0 && slot <= 3 &&
                            MageApparelBuyKey(armo, mageTop2, mageSchoolPrimary, mageAllowVillain, cTier, cMetric)) {
                            // Beats what he wears in this slot?
                            int wTier = 0; std::int32_t wMetric = 0;
                            if (auto* worn = WornInLogicalSlot(a_follower, slot))
                                MageApparelBuyKey(worn, mageTop2, mageSchoolPrimary, /*allowVillain*/true, wTier, wMetric);
                            const bool beatsWorn = cTier > wTier || (cTier == wTier && cMetric > wMetric);
                            const bool beatsBest = cTier > bestMageTier ||
                                                   (cTier == bestMageTier && cMetric > bestMageMetric);
                            if (beatsWorn && beatsBest) {
                                bestMageTier   = cTier;
                                bestMageMetric = cMetric;
                                bestMage       = obj;
                            }
                        }
                    }
                    // The PLAIN rating path is for NON-magic users ONLY
                    // (marth, v1.0.31: PURE CASTER). v1.0.29 ran it for mages
                    // too and let their "no school match but rated" pieces
                    // fall through to it -- which is exactly how Marcurio, a
                    // detected Destruction user, looted a Dwarven Heavy
                    // Cuirass and Chitin Heavy Boots by raw rating. Skipping
                    // the whole path means a magic user can never LOOT armor
                    // at all; armor he already wears stays on (nothing here
                    // strips gear) until a school robe displaces it on equip.
                    // This also retires the WouldStripSchoolGear guard: with
                    // no rating path for mages there is nothing left to
                    // thrash against their robes.
                    // Best-first: among the armour upgrades this body offers, keep the
                    // HIGHEST-rated (not the first enumerated), so a carry-weight cutoff
                    // can't strand the actually-best piece.
                    if (!useMageApparel &&
                        !shieldUseless && ArmorIsBetter(a_follower, armo) &&
                        armo->GetArmorRating() > bestArmorRat &&
                        !CarriesSlotArmorAtLeast(a_follower, armo)) {   // #3: don't re-take/equip a worse same-slot piece already in the pack (strip double-take)
                        bestArmorRat = armo->GetArmorRating();
                        bestArmor    = obj;
                    }
                } else if (auto* weap = obj->As<RE::TESObjectWEAP>()) {
                    if (IsCreatureWeapon(weap)) continue;   // never equip automaton/creature gear
                    const WepClass wc = WeaponClassOf(weap->GetWeaponType());
                    // MELEE upgrade: ONLY the target melee class -- the equip-melee
                    // gambit's best-skill class, or the class they already wield. Never
                    // cross-class, and never skill-forced onto a ranged user (that was
                    // the thrash bug). meleeTargetClass == Other means no melee role.
                    if (meleeTargetClass != WepClass::Other && wc == meleeTargetClass &&
                        weap->GetAttackDamage() > bestWeapDmg) {
                        bestWeapDmg = weap->GetAttackDamage();
                        bestWeap    = obj;
                    }
                    // Ranged pickup -- ONLY the follower's kind (bow XOR crossbow).
                    if (doRanged) {
                        const auto wt = weap->GetWeaponType();
                        const bool kindMatch = wantCrossbow ? (wt == WT::kCrossbow) : (wt == WT::kBow);
                        if (kindMatch && weap->GetAttackDamage() > bestRangedDmg) {
                            bestRangedDmg = weap->GetAttackDamage();
                            bestRanged    = obj;
                        }
                    }
                    // MAGE BACKUP (v1.0.29): the sidearm a caster's own AI draws
                    // when his magicka is gone. Daggers only by default
                    // (bMageDaggersOnly); the toggle opens it to the best of any
                    // one-hander. Baselined on his BEST-OWNED sidearm (bestBackupDmg
                    // = myBackupDmg), not the momentarily wielded hand: a caster
                    // dual-wielding SPELLS reads an empty weapon hand, so the old
                    // carries-none gate skipped every better dagger while he cast
                    // (marth field: "ignores better daggers while casting"). Now he
                    // restocks when he carries none (myBackupDmg==0) AND upgrades to
                    // a STRICTLY better sidearm -- one at a time (§4.3), and NEVER
                    // force-equipped (equipIt=false below), so it stays a stocked
                    // backup he switches to, not an armory or a per-corpse equip thrash.
                    if (wantBackup && wc == WepClass::OneHand &&
                        (!daggersOnly || weap->GetWeaponType() == WT::kOneHandDagger) &&
                        weap->GetAttackDamage() > bestBackupDmg) {
                        bestBackupDmg = weap->GetAttackDamage();
                        bestBackup    = obj;
                    }
                }
            }

            // Prefer the in-class weapon upgrade; then a ranged weapon they need
            // for their equip-ranged gambit; then the mage's missing sidearm
            // (safety before wardrobe); then school apparel over plain armor
            // (the point of the magic loadout). One item this tick (§4.3).
            RE::TESBoundObject* best = bestWeap   ? bestWeap
                                     : bestRanged ? bestRanged
                                     : bestBackup ? bestBackup
                                     : bestMage   ? bestMage
                                                  : bestArmor;
            // Peek: an upgrade exists AND the follower can actually carry it. Without
            // the weight gate an overencumbered follower walks a whole excursion leg,
            // takes nothing at arrival (the real take IS weight-gated below), and the
            // corpse gets marked DONE -- a wasted trip.
            if (a_peek) return best != nullptr && FitsCarryWeight(a_follower, best->GetWeight());
            if (!best) return false;
            if (!FitsCarryWeight(a_follower, best->GetWeight())) return false;

            // ACQUIRE + EQUIP through the shared v1.0.38 safe step: transfers from
            // a_src, captures + carries MEO gems, equips IN PLACE on the main thread
            // (MainThread::Post EquipObject, never DoReset3D -- #62), queues the gem
            // move. The mage BACKUP stays STOCK-ONLY (a caster's hand belongs to his
            // spells; his own AI draws the sidearm at zero magicka). The buy / owned-
            // upgrade pass calls the SAME AcquireEquip with a_src=nullptr.
            const bool equipped = AcquireEquip(a_follower, best, a_src, myWeap, best == bestBackup);

            // [equip] DIAGNOSTIC: log WHAT we put on, over WHAT, and the reasoning.
            if (auto* nw = best->As<RE::TESObjectWEAP>()) {
                spdlog::info("[equip] {:08X}: LOOT-{} weapon '{}' dmg={} class={} <- held '{}' "
                             "dmg={} class={} | meleeTgt={} wantsMelee={} wantsRanged={} baseDmg={}",
                             a_follower->GetFormID(), equipped ? "EQUIP" : "STOCK",
                             nw->GetFullName() ? nw->GetFullName() : "?", nw->GetAttackDamage(),
                             static_cast<int>(WeaponClassOf(nw->GetWeaponType())),
                             myWeap && myWeap->GetFullName() ? myWeap->GetFullName() : "(none)",
                             myWeap ? myWeap->GetAttackDamage() : 0,
                             myWeap ? static_cast<int>(WeaponClassOf(myWeap->GetWeaponType())) : -1,
                             static_cast<int>(meleeTargetClass), wantsMelee, wantsRanged, baseDmg);
            } else {
                spdlog::info("[equip] {:08X}: LOOT armor/apparel '{}' -> equip {}", a_follower->GetFormID(),
                             best->As<RE::TESFullName>() && best->As<RE::TESFullName>()->GetFullName()
                                 ? best->As<RE::TESFullName>()->GetFullName() : "?",
                             MainThread::IsInstalled() ? "queued to main thread" : "direct (VR/no-pump)");
            }

            // MAGIC-LOADOUT diagnostics: WHY the mage item won (logged on a TAKE only).
            if (best == bestMage || best == bestBackup) {
                spdlog::info("[loot] {:08X} '{}' magic-user: target school {} (from {} cast gambit(s))",
                             a_follower->GetFormID(),
                             a_follower->GetName() ? a_follower->GetName() : "?",
                             SchoolName(school), castGambits);
            }
            if (best == bestMage) {
                spdlog::info("[loot] apparel {:08X} '{}' tier={} metric={} (schoolPrimary={}) -> best",
                             best->GetFormID(), best->GetName() ? best->GetName() : "?",
                             bestMageTier, bestMageMetric, mageSchoolPrimary);
            }
            if (best == bestBackup) {
                auto* mw = best->As<RE::TESObjectWEAP>();
                spdlog::info("[loot] mage backup {} {:08X} '{}' dmg={} -- stocked; his own AI draws it when the magicka runs out",
                             daggersOnly ? "dagger" : "1h", best->GetFormID(),
                             best->GetName() ? best->GetName() : "?",
                             mw ? mw->GetAttackDamage() : 0);
            }
            return true;
        }

} // namespace MFO::Logistics
