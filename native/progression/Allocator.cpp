// progression/Allocator.cpp -- the allocator CORE: its session state, the catalog
// index + ownership questions, the class table (FindClassDef), post-load /
// menu-close, and the whole PRGN co-save (CoSaveSave / CoSaveLoad / ClearAll).
// Was the head of native/ProgAllocator.cpp; the wave-1 subsystem-folder split
// (2026-09-24) moved the other concerns to their own files in progression/.
#include "PCH.h"
#include <cmath>     // std::floor / std::isfinite — not guaranteed via the PCH
#include "ProgAllocator.h"
#include "ProgAllocator_internal.h"   // the split family's shared substrate
#include "Progression.h"
#include "Forms.h"   // §18.6: the addon-manifest sentinel keyword
#include "Board.h"
#include "Config.h"
#include "Followers.h"
#include "Rapport.h"
#include "MainThread.h"
#include "Serialization.h"
#include "State.h"

// The allocator backend. See ProgAllocator.h for the contract;
// Docs/FOLLOWER-PROGRESSION-ESL-DESIGN.md §4/§5/§6/§8/§15 for the design.
//
// Every engine write here is one of the three ProgProbe-proven mechanisms —
// nothing in this file tries an unproven RE:: path:
//   perks  : TESNPC::AddPerk/RemovePerk + Actor::ApplyPerksFromBase (P1)
//   skills : GetBaseActorValue/SetBaseActorValue + the §4.2 reconcile (P2)
//   reapply: GetPerkIndex-guarded, ApplyPerksFromBase only on change (P3)
//
// Every log line carries the [prog] tag. Log the ZERO cases too — "found
// none" and "never ran" must not look the same (INVARIANTS #46).

namespace MFO::ProgAllocator {

        // ── the poll (level-with-player, §6/§15) ────────────────────────────
        // A MainThread::Post self-chain (the ProgProbe DelayedTick shape).
        // Generation-guarded: revert/reload bumps g_pollGen so a stale chain
        // drops itself even if MainThread::Clear missed it (belt + braces).
        int g_pollGen    = 0;
        int g_pollFrames = 0;

        // The one global economy anchor: the player level the last poll saw.
        // Serialized in the PRGN header so benched half-rate accrual survives
        // a save/load without re-observing (and never double-grants).
        std::uint16_t g_lastPlayerLevel = 0;

        // §HMS fixed-stat grant (v1.1 Phase 3): the running total of the PLAYER's
        // base H/M/S the last poll saw. On a player level-up the positive delta is
        // the LIVE catch-up rate granted to caught-up fixed-stat followers (never a
        // hardcoded number — "whatever the player actually gains"). Serialized in
        // the PRGN v6 header next to g_lastPlayerLevel; O(1), not per-follower.
        // 0 == unobserved (player base HMS is never 0) → the first observation
        // initializes it with a 0 grant, never a giant first-run catch-up.
        float g_playerHmsTotalLast = 0.0f;

    namespace {
        // ── co-save ingestion bounds (INVARIANTS #11) ───────────────────────
        constexpr std::uint32_t kMaxProgFollowers = 4096;
        constexpr std::uint16_t kMaxSkillAllocs   = 64;

        // ── catalog access (component 1 — consumed, never rewritten) ────────

        // O(1) node index over the FROZEN catalog. FindNode / OwnsAnyRank /
        // PerkAllocatableInCatalog were each a full skill-major scan; called per
        // parent per node inside BuildNodeViews (the ~500ms board publish) they
        // made it O(N^2) — a main-thread hitch. The catalog is frozen after
        // Progression::Init, so one map keyed by EVERY rank's perk FormID -> its
        // NodeRef replaces the scans. Rebuilt only when the catalog identity
        // changes (revert/reload), detected by an O(1) (trees-buffer, tree-count)
        // signature so the lookup path stays O(1) and never re-walks the catalog.
        std::unordered_map<RE::FormID, NodeRef> g_nodeIndex;
        const void* g_nodeIndexBase  = reinterpret_cast<const void*>(-1);   // != any real ptr
        std::size_t g_nodeIndexTrees = 0;

    }

        const std::unordered_map<RE::FormID, NodeRef>& NodeIndex() {
            const auto& skills = Progression::Get().skills;
            const void* base = skills.empty() ? nullptr : static_cast<const void*>(skills.data());
            if (base == g_nodeIndexBase && skills.size() == g_nodeIndexTrees)
                return g_nodeIndex;   // catalog unchanged — reuse
            g_nodeIndex.clear();
            std::size_t total = 0;
            for (const auto& t : skills) total += t.nodes.size();
            g_nodeIndex.reserve(total * 2 + 1);
            for (const auto& tree : skills)
                for (const auto& node : tree.nodes)
                    for (const auto& r : node.ranks)
                        g_nodeIndex.emplace(r.perkFormID, NodeRef{ &tree, &node });
            g_nodeIndexBase  = base;
            g_nodeIndexTrees = skills.size();
            return g_nodeIndex;
        }

        // Find a node by its identity (the rank-1 perk FormID). O(1) via the
        // index; preserves the old scan's rank-1-only semantics (the id must be
        // the node's IDENTITY, not merely one of its later ranks).
        NodeRef FindNode(RE::FormID a_nodePerkID) {
            const auto& idx = NodeIndex();
            auto it = idx.find(a_nodePerkID);
            if (it != idx.end() && it->second.node->perkFormID == a_nodePerkID)
                return it->second;
            return {};
        }

        // ── ownership questions ─────────────────────────────────────────────

        // Does the follower own ANY rank of this node — through MFO's alloc,
        // or natively (Requiem/SPID granted)? The prereq gate (§5.1) needs
        // "owned at all"; the parent may itself be a FILTERED perk that never
        // reaches the board, so this must work from a bare FormID too.
        bool OwnsAnyRank(RE::Actor* a_actor, RE::TESNPC* a_base,
                         const ProgState& a_st, RE::FormID a_nodePerkID) {
            for (const auto& p : a_st.perks)
                if (p.nodePerkID == a_nodePerkID && p.rank > 0) return true;
            // Native ownership: check every rank form when the node is in the
            // catalog, else just the id we were handed.
            if (auto ref = FindNode(a_nodePerkID); ref.node) {
                for (const auto& r : ref.node->ranks) {
                    if (auto* perk = RE::TESForm::LookupByID<RE::BGSPerk>(r.perkFormID)) {
                        if (a_base->GetPerkIndex(perk).has_value() || a_actor->HasPerk(perk))
                            return true;
                    }
                }
                return false;
            }
            auto* perk = RE::TESForm::LookupByID<RE::BGSPerk>(a_nodePerkID);
            return perk && (a_base->GetPerkIndex(perk).has_value() || a_actor->HasPerk(perk));
        }

    const std::vector<ClassDef>& Classes() { return g_classes; }

    const ClassDef* FindClassDef(RE::FormID a_id) {
        if (a_id == 0) return nullptr;
        for (const auto& def : g_classes)
            if (def.id == a_id) return &def;
        return nullptr;
    }

    bool Active() { return g_ready; }

    void OnPostLoad() {
        if (!g_ready) {
            if (!g_prog.empty())
                spdlog::warn("[prog] {} progression record(s) loaded but the addon is absent — "
                             "inert this session (data preserved, nothing applied)", g_prog.size());
            return;
        }
        // §10 (perk-bug fix 2026-08-17): DO NOT re-read the economy from GLOB
        // values here. GLOB values are SAVE-PERSISTED — a save made before an
        // economy GLOB was repurposed/re-defaulted carries a STALE value (e.g.
        // 0x802 held 1 under the old MFOP_PerkPointsPerLevel; reading it post-
        // load gave floor(level/1) = double the perk pool). The economy stays
        // at the RECORD DEFAULTS latched at kDataLoaded. A live MCM override
        // rides a NON-save-persisted INI instead (ApplyEconomyOverride), never
        // the globals.
        ApplyEconomyOverride();   // re-overlay the addon MCM INI (defaults cached)
        // Fresh session: everything reapplies lazily (the poll retries until
        // each enrolled actor resolves — P3's guarded shape, never eager).
        // Fable F1 (4a62688): base perk edits do NOT survive a load (P3), so the
        // natives are back on every base — re-arm the strip too (idempotent;
        // strippedPerks unions), or every native node re-locks on the Board and
        // the reapply defers MFO's rank behind the returned native.
        for (auto& [id, st] : g_prog) { st.applied = false; st.nativeHeld = false; }

        ++g_pollGen;
        g_pollFrames = kPollFrames;
        const int gen = g_pollGen;
        MainThread::Post([gen]() { PollTick(gen); });
        // Seed the board views once so the Progression TAB exists on the very
        // first board open (the power's own PublishSnapshot copies the prog
        // pointer before the open-gated poll refresh would have run).
        PublishBoardViews();
        spdlog::info("[prog] post-load: {} progression record(s); level poll started (gen {})",
                     g_prog.size(), gen);
    }

    void OnMenuClose() {
        // (b) The MenuSink calls this on MCM/Journal close. A live MCM override
        // re-applies here from a NON-save-persisted INI (ApplyEconomyOverride),
        // never from the GLOB runtime values (those are save-persisted — the
        // 2026-08-17 perk-pool corruption). Marshals to the true main thread:
        // g_econ / g_ready are touched ONLY there.
        MainThread::Post([]() {
            if (!g_ready) return;   // addon absent — nothing to apply
            ApplyEconomyOverride();
        });
    }

    // ── co-save ('PRGN' v2, §8 + §16 + §17; v5/v6 §HMS) ─────────────────────
    //
    //   u16 lastPlayerLevel
    //   v6: f32 g_playerHmsTotalLast        (§HMS Phase 3 catch-up rate anchor)
    //   u32 followerCount, then per follower:
    //     u32 formID | u8 flags | u8 class | u16 progressionLevel
    //     u16 sharedGrowthRemainder
    //     … (v2/v3/v4/v5 fields) …
    //     v5 §HMS block (END of record): per pool {H,M,S}: f32 baseline,
    //         [v5 ONLY: f32 target — recomputed, not stored in v6], f32 skew,
    //         f32 cumulative; then u32 battlesSinceLevelUp, u32 battlesOffClass,
    //         u8 offClassPool, u8 hmsCaptured
    //     v6 §HMS additions (after hmsCaptured): u8 hmsZeroAwardStreak,
    //         f32 hmsGrantRemainder[3], f32 hmsAwardAccum; + flags bit 0x20 = fixedStat
    //     v7 (after the HMS block): u16 autoLevelsGranted, u8 freeRespec,
    //         u16 strippedCount + u32 strippedPerk × N
    //     v8 (after the v7 block): f32 hmsWithheld, f32 hmsParityCredit,
    //         f32 hmsHeld[3] {H,M,S} (what MFO last held; flags bit 0x40 retro-pending)
    //     v1 ONLY: f32 unspentPerk        (legacy stored pool — read + DISCARDED;
    //                                      §17 derives the pool instead)
    //     v2: u16 manualBaselineLevel | u16 manualPointsApplied
    //         u16 manualExcludedLevels    (§16 override accounting)
    //         u16 nativeTreePerksAtEnroll (§17 budget debit)
    //     u16 perkCount   { u32 nodePerkID, u8 rank }*
    //     u16 skillCount  { u32 av, f32 points, f32 lastWrittenBase,
    //                       v2: f32 manualPoints }*
    //     u16 baseCount   { u32 av, f32 value }*
    //
    // v2 never shipped in any deployed build, so its layout is defined here
    // once, cleanly — no in-between shape to migrate.
    //
    // FormIDs go through ResolveFormID on load (INVARIANTS #8): a gone
    // follower drops with its whole block consumed; a gone perk drops and
    // REFUNDS its rank-count in points. AV ids are engine enum ordinals, not
    // FormIDs — no resolution, bounds-checked only. Runtime (0xFF) ids are
    // never written (#9). New fields go behind `if (version >= N)` (#12).

    namespace {
        // PRGN v4 class-identity codec. The class is written as a stable
        // plugin-qualified pair {u16 pluginLen, plugin bytes, u32 localFormID}
        // so it survives an addon-absent session (see Serialization.h v4 note).
        constexpr std::uint16_t kMaxPluginLen = 260;   // MAX_PATH; a plugin name can't exceed it

        void WritePluginName(SKSE::SerializationInterface* a_intfc, const std::string& a_s) {
            const auto len = static_cast<std::uint16_t>(
                std::min<std::size_t>(a_s.size(), kMaxPluginLen));
            a_intfc->WriteRecordData(len);
            if (len) a_intfc->WriteRecordData(a_s.data(), len);
        }
        // Returns false on short read / implausible length — caller aborts the
        // whole load (a desynced byte stream can't be trusted, INVARIANT #12).
        bool ReadPluginName(SKSE::SerializationInterface* a_intfc, std::string& a_out) {
            std::uint16_t len = 0;
            if (!a_intfc->ReadRecordData(len)) return false;
            if (len > kMaxPluginLen) {
                spdlog::error("[cosave] class plugin name length {} exceeds max {} -- aborting",
                              len, kMaxPluginLen);
                return false;
            }
            a_out.resize(len);
            if (len == 0) return true;
            return a_intfc->ReadRecordData(a_out.data(), len) == len;
        }
    }

    // GENERAL follower-allocation-state serializer (host machinery — Serialization.h
    // §PRGN). Writes the header + one blob per ENROLLED follower; with no add-on
    // manifest nothing enrolls, g_prog is empty, and only the header + count=0 go
    // out (Phase 9 acceptance test). Every field is general allocation-engine state;
    // the class is an OPAQUE plugin-qualified reference re-resolved on load, never
    // interpreted here (its meaning lives in the manifest form). Written even when
    // the add-on is ABSENT this session so a temporarily-disabled ESL's state is
    // echoed back verbatim, not destroyed (general "preserve the add-on's blob").
    void CoSaveSave(SKSE::SerializationInterface* a_intfc) {
        if (!a_intfc->OpenRecord(kRecProgression, kProgVersion)) {
            spdlog::error("[cosave] OpenRecord('{}') failed -- progression NOT saved", "PRGN");
            return;
        }
        a_intfc->WriteRecordData(g_lastPlayerLevel);
        // v6 GLOBAL HEADER: the player's running base-HMS total (§HMS Phase 3),
        // right after lastPlayerLevel and before the follower count. Read gated on
        // version>=6; a pre-v6 stream inits it from the live player on load.
        a_intfc->WriteRecordData(g_playerHmsTotalLast);

        std::uint32_t persistable = 0;
        for (const auto& [id, st] : g_prog)
            if (st.enrolled && Followers::IsPersistableID(id)) ++persistable;
        a_intfc->WriteRecordData(persistable);

        std::uint32_t written = 0, skippedRuntime = 0;
        for (const auto& [id, st] : g_prog) {
            if (!st.enrolled) continue;
            if (!Followers::IsPersistableID(id)) { ++skippedRuntime; continue; }
            a_intfc->WriteRecordData(id);
            const std::uint8_t flags =
                (st.enrolled ? 1u : 0u) | (st.autoSpend ? 2u : 0u) |
                (st.veteranConsumed ? 4u : 0u) | (st.wasInPotentialFollowerFaction ? 8u : 0u) |
                (st.manualSkills ? 16u : 0u) |   // v2 (§16) — spare bit in the same byte
                (st.fixedStat ? 32u : 0u) |      // v6 (§HMS Phase 3) — 0x20, free in v1–v5
                (st.hmsRetroPending ? 64u : 0u); // v8 (§HMS parity) — 0x40, never written by v1–v7
            a_intfc->WriteRecordData(flags);
            // v4 (SEV-2 class-wipe fix): the class is written as its STABLE
            // plugin-qualified identity {u16 pluginLen, plugin bytes, u32
            // localFormID}, NOT the runtime clsId (which v3 wrote and a session
            // without the addon could no longer resolve → cleared → 0 persisted
            // → class lost forever). Replaces the v3 4-byte FormID at the SAME
            // field position.
            //   clsId != 0 → resolved this session: re-derive the authoritative
            //     identity from the live form (also self-heals a v3→v4 upgrade).
            //   clsId == 0 but clsPlugin non-empty → failed to resolve THIS
            //     session (addon absent): echo the loaded identity VERBATIM so a
            //     save never persists a cleared class over one that merely
            //     failed to resolve.
            std::string clsPlugin = st.clsPlugin;
            RE::FormID  clsLocal  = st.clsLocal;
            if (st.clsId != 0) {
                std::string p; RE::FormID l = 0;
                if (DeriveClassIdentity(st.clsId, p, l)) { clsPlugin = p; clsLocal = l; }
            }
            WritePluginName(a_intfc, clsPlugin);
            a_intfc->WriteRecordData(clsLocal);
            a_intfc->WriteRecordData(st.progressionLevel);
            a_intfc->WriteRecordData(st.sharedGrowthRemainder);
            // v2: BASELINES only, never a pool value — both the §16 manual
            // pool and the §17 perk pool are recomputed from these, so a
            // save replays to the same numbers by construction.
            a_intfc->WriteRecordData(st.manualBaselineLevel);
            a_intfc->WriteRecordData(st.manualPointsApplied);
            a_intfc->WriteRecordData(st.manualExcludedLevels);   // §16 override accounting
            a_intfc->WriteRecordData(st.nativeTreePerksAtEnroll);

            const auto perkCount = static_cast<std::uint16_t>(
                std::min<std::size_t>(st.perks.size(), kMaxPerkAllocs));
            a_intfc->WriteRecordData(perkCount);
            for (std::uint16_t i = 0; i < perkCount; ++i) {
                a_intfc->WriteRecordData(st.perks[i].nodePerkID);
                a_intfc->WriteRecordData(st.perks[i].rank);
            }
            const auto skillCount = static_cast<std::uint16_t>(
                std::min<std::size_t>(st.skills.size(), kMaxSkillAllocs));
            a_intfc->WriteRecordData(skillCount);
            for (std::uint16_t i = 0; i < skillCount; ++i) {
                a_intfc->WriteRecordData(static_cast<std::uint32_t>(st.skills[i].av));
                a_intfc->WriteRecordData(st.skills[i].points);
                a_intfc->WriteRecordData(st.skills[i].lastWrittenBase);
                a_intfc->WriteRecordData(st.skills[i].manualPoints);   // v2 (§16)
                a_intfc->WriteRecordData(st.skills[i].autoPoints);     // v7 (A′ accumulator)
            }
            const auto baseCount = static_cast<std::uint16_t>(
                std::min<std::size_t>(st.baseline.size(), kMaxSkillAllocs));
            a_intfc->WriteRecordData(baseCount);
            for (std::uint16_t i = 0; i < baseCount; ++i) {
                a_intfc->WriteRecordData(static_cast<std::uint32_t>(st.baseline[i].av));
                a_intfc->WriteRecordData(st.baseline[i].value);
            }
            // ── v6 §HMS block — APPENDED at the very END of the record ───────
            // Fixed order, NO count prefix (always exactly 3 pools). Per pool in
            // fixed {Health, Magicka, Stamina} order: baseline, skew, cumulative
            // (f32 each — v6 DROPS hmsTarget, always max(baseline,baseline+cumul),
            // recomputed on load); then the 2 battle counters (u32), the off-class
            // pool (u8), the captured flag (u8), and the v6 Phase-3 additions:
            // hmsZeroAwardStreak (u8), hmsGrantRemainder (f32×3), hmsAwardAccum
            // (f32). fixedStat rides flags bit 0x20 above — NOT a byte here.
            // Read GATED on version
            // in CoSaveLoad (v6 layout / v5 keeps the old 4-f32/pool reader).
            // Nothing above this moved (byte-identical for v1–v4 saves).
            for (int p = 0; p < 3; ++p) {
                a_intfc->WriteRecordData(st.hmsBaseline[p]);
                a_intfc->WriteRecordData(st.hmsSkew[p]);
                a_intfc->WriteRecordData(st.hmsCumulative[p]);
            }
            a_intfc->WriteRecordData(st.battlesSinceLevelUp);
            a_intfc->WriteRecordData(st.battlesOffClass);
            a_intfc->WriteRecordData(st.offClassPool);
            a_intfc->WriteRecordData(static_cast<std::uint8_t>(st.hmsCaptured ? 1u : 0u));
            a_intfc->WriteRecordData(st.hmsZeroAwardStreak);            // v6
            for (int p = 0; p < 3; ++p)
                a_intfc->WriteRecordData(st.hmsGrantRemainder[p]);      // v6
            a_intfc->WriteRecordData(st.hmsAwardAccum);                 // v6 (detection tally, serialized)
            // ── v7 block — APPENDED after the HMS block: the auto-level grant
            // ledger (A′) and the native-perk strip record (B′). Read gated on
            // version >= 7; count-prefixed ids, each ResolveFormID'd on load.
            a_intfc->WriteRecordData(st.autoLevelsGranted);
            a_intfc->WriteRecordData(static_cast<std::uint8_t>(st.freeRespec ? 1u : 0u));   // v7
            // (nativeHeld is NOT written: base AddPerk/RemovePerk are runtime-only
            // — P3 — so the strip is per-session by nature and re-runs on load.)
            const auto strippedCount = static_cast<std::uint16_t>(
                std::min<std::size_t>(st.strippedPerks.size(), kMaxPerkAllocs));
            a_intfc->WriteRecordData(strippedCount);
            for (std::uint16_t i = 0; i < strippedCount; ++i)
                a_intfc->WriteRecordData(st.strippedPerks[i]);
            // ── v8 block — APPENDED after the v7 block: §HMS player-rate parity.
            a_intfc->WriteRecordData(st.hmsWithheld);       // v8 f32
            a_intfc->WriteRecordData(st.hmsParityCredit);   // v8 f32
            for (int p = 0; p < 3; ++p)
                a_intfc->WriteRecordData(st.hmsHeld[p]);    // v8 f32×3 {H,M,S}: last HELD
            ++written;
        }
        spdlog::info("[cosave] saved {} progression record(s), schema v{}{}", written, kProgVersion,
                     skippedRuntime ? std::format(" -- SKIPPED {} runtime (0xFF) record(s)", skippedRuntime)
                                    : std::string{});
    }

    // GENERAL follower-allocation-state deserializer (host machinery). Keeps a
    // reader for EVERY shipped version forever (v1–v8, INVARIANT #12); re-resolves
    // follower + plugin-qualified class references without interpreting them. Runs
    // even when the add-on is absent (state preserved, inert — OnPostLoad gates
    // application on g_ready).
    void CoSaveLoad(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version) {
        // v1 saves lack the §16 manual fields — every v2 read below gates on
        // a_version >= 2 and defaults to "manual off, nothing applied".
        g_prog.clear();    // defence in depth — ClearAll ran at revert already

        std::uint16_t lastPl = 0;
        if (!a_intfc->ReadRecordData(lastPl)) {
            spdlog::error("[cosave] short read on progression header -- ABORTING progression load");
            return;
        }
        g_lastPlayerLevel = lastPl;

        // v6 GLOBAL HEADER: the player's running base-HMS total (§HMS Phase 3),
        // read right after lastPlayerLevel. A pre-v6 stream has no such field →
        // seed it from the LIVE player so the first post-load level-up grants no
        // spurious catch-up (belt-and-braces with PollWork's 0-init guard).
        if (a_version >= 6) {
            float ph = 0.0f;
            if (!a_intfc->ReadRecordData(ph)) {
                spdlog::error("[cosave] short read on progression header (playerHms) -- ABORTING");
                return;
            }
            g_playerHmsTotalLast = std::isfinite(ph) ? ph : 0.0f;
        } else {
            float t = 0.0f;
            if (auto* player = RE::PlayerCharacter::GetSingleton())
                for (int p = 0; p < 3; ++p) t += Followers::GetFollowerHMS(player, p);
            g_playerHmsTotalLast = t;
        }

        std::uint32_t count = 0;
        if (!a_intfc->ReadRecordData(count)) return;
        if (count > kMaxProgFollowers) {
            spdlog::error("[cosave] implausible progression follower count {} -- ABORTING", count);
            return;
        }

        const bool haveCatalog = Progression::Get().built;
        std::uint32_t loaded = 0, droppedActor = 0, droppedPerk = 0, droppedAv = 0;

        for (std::uint32_t i = 0; i < count; ++i) {
            RE::FormID rawID = 0;
            if (!a_intfc->ReadRecordData(rawID)) return;

            RE::FormID resolvedID = 0;
            const bool resolved = a_intfc->ResolveFormID(rawID, resolvedID);

            ProgState st{};
            std::uint8_t flags = 0, legacyClsRaw = 0;
            RE::FormID   rawClsId = 0;        // v3 only
            std::string  v4ClsPlugin;        // v4+ only
            RE::FormID   v4ClsLocal = 0;      // v4+ only
            if (!a_intfc->ReadRecordData(flags)) return;
            // The class field has migrated TWICE, always at the SAME position.
            // Consume EXACTLY the bytes the save's version wrote (INVARIANT #12
            // — a wrong byte count desyncs every field after this):
            //   v<3  : 1-byte ordinal        (migrated to the k-th declared class)
            //   v==3 : 4-byte runtime FormID (ResolveFormID'd — but a session
            //          without the addon cleared it: the SEV-2 wipe v4 closes)
            //   v>=4 : {u16 pluginLen, plugin bytes, u32 localFormID} — a stable
            //          plugin-qualified identity that survives an addon-absent
            //          session.
            if (a_version < 3) {
                // v1/v2 stored the fixed-3 ordinal (1=Melee 2=Ranged 3=Mage).
                if (!a_intfc->ReadRecordData(legacyClsRaw)) return;
            } else if (a_version < 4) {
                if (!a_intfc->ReadRecordData(rawClsId)) return;
            } else {
                if (!ReadPluginName(a_intfc, v4ClsPlugin)) return;
                if (!a_intfc->ReadRecordData(v4ClsLocal)) return;
            }
            if (!a_intfc->ReadRecordData(st.progressionLevel)) return;
            if (!a_intfc->ReadRecordData(st.sharedGrowthRemainder)) return;
            if (a_version < 2) {
                // v1 stored the (pre-§17) point pool — consume the bytes,
                // discard the value: the pool is derived now. A v1 follower
                // migrates with nativeTreePerksAtEnroll = 0 (the debit was
                // never captured; the derived pool is simply what §17 says).
                float legacyUnspent = 0.0f;
                if (!a_intfc->ReadRecordData(legacyUnspent)) return;
            }
            st.enrolled                       = (flags & 1u) != 0;
            st.autoSpend                      = (flags & 2u) != 0;
            st.veteranConsumed                = (flags & 4u) != 0;
            st.wasInPotentialFollowerFaction  = (flags & 8u) != 0;
            st.manualSkills                   = (flags & 16u) != 0;   // v2 (§16)
            st.fixedStat                      = (flags & 32u) != 0;   // v6 (§HMS Phase 3); 0 in v1–v5
            st.hmsRetroPending                = (flags & 64u) != 0;   // v8 (§HMS parity); 0 in v1–v7
            if (a_version < 3) {
                // MIGRATION (§18.6 PRGN discipline). Pre-v3 saves were ONLY ever
                // written by MFO_Progression.esl as the sole addon, at the FIXED
                // ordinals 1=Melee 2=Ranged 3=Mage. Resolve the ordinal against
                // THAT plugin's known class-def local ids (SEV-3: NOT an index
                // into the global g_classes, which a 2nd addon would shift), and
                // SYNTHESIZE the v4 plugin-qualified identity so the class is KEPT
                // even when the addon is absent this session (SEV-3: the old code
                // cleared clsId when g_classes was empty, then the v4 save wrote an
                // EMPTY identity → class lost forever). Mirrors the v4 reader:
                // keep clsPlugin/clsLocal always, resolve to a runtime clsId only
                // when the plugin is present.
                static constexpr const char* kProgPlugin = "MFO_Progression.esl";
                // Class-def FLST local ids — FROZEN contract with
                // MFO_GenerateESP.py (PGID_CLASSDEF_MELEE/RANGED/MAGE = 0x850/1/2).
                static constexpr RE::FormID kClassDefLocal[3] = { 0x850, 0x851, 0x852 };
                const int ord = static_cast<int>(legacyClsRaw);
                if (ord >= 1 && ord <= 3) {
                    st.clsPlugin = kProgPlugin;
                    st.clsLocal  = kClassDefLocal[ord - 1];
                    // #21 route through the .esl<->.esp sibling resolve so a .esp-only
                    // load still resolves the legacy ordinals (kProgPlugin literal kept).
                    RE::TESForm* form = LookupAddonForm(st.clsLocal, st.clsPlugin);
                    if (form && FindClassDef(form->GetFormID())) {
                        st.clsId = form->GetFormID();
                    } else {
                        spdlog::info("[cosave] legacy class ordinal {} migrated to {}|{:06X} — "
                                     "not resolvable this session (addon absent), identity KEPT "
                                     "(running class-less until it returns)", ord, kProgPlugin, st.clsLocal);
                    }
                } else if (ord > 3) {
                    // DON'T silently clamp a corrupt ordinal to 3 (Mage). Warn and
                    // leave class-less (nothing to keep).
                    spdlog::warn("[cosave] corrupt legacy class ordinal {} (>3) — left "
                                 "class-less (re-pick on the board), NOT mapped to Mage", ord);
                }
                // ord == 0: never had a class; leave class-less silently.
            } else if (a_version < 4) {
                // v3: bare runtime FormID. ResolveFormID + FindClassDef; on
                // FAILURE (addon absent) clsId stays 0 for the session — a v3
                // record carries NO plugin string, so recovering the identity
                // is impossible (the SEV-2 wipe is only fully closed for v4+
                // saves). A v3 record that DOES resolve self-heals: its next
                // save is written in the v4 plugin-qualified form.
                if (rawClsId != 0) {
                    RE::FormID resolvedCls = 0;
                    if (a_intfc->ResolveFormID(rawClsId, resolvedCls) &&
                        FindClassDef(resolvedCls)) {
                        st.clsId = resolvedCls;
                    } else {
                        spdlog::warn("[cosave] v3 class {:08X} unresolvable this session — "
                                     "running class-less (a v3 record carries no plugin "
                                     "string; re-pick on the board, or it self-heals to v4 "
                                     "once it resolves)", rawClsId);
                    }
                }
            } else {
                // v4: plugin-qualified identity. ALWAYS keep clsPlugin/clsLocal
                // (so a save this session — even one taken WITHOUT the addon —
                // echoes them back verbatim rather than persisting a cleared
                // class). Resolve to a runtime clsId only when the plugin is
                // present; absent/removed → clsId 0, all gates already handle
                // "no class", and the next save re-writes the real identity once
                // the addon returns.
                st.clsPlugin = v4ClsPlugin;
                st.clsLocal  = v4ClsLocal;
                if (!v4ClsPlugin.empty()) {
                    // #21 .esl<->.esp sibling resolve (cross-variant save loads either way).
                    RE::TESForm* form = LookupAddonForm(v4ClsLocal, v4ClsPlugin);
                    if (form && FindClassDef(form->GetFormID())) {
                        st.clsId = form->GetFormID();
                    } else {
                        spdlog::info("[cosave] class {}|{:06X} not resolvable this session "
                                     "(addon absent or class removed) — identity KEPT, "
                                     "running class-less until it returns", v4ClsPlugin, v4ClsLocal);
                    }
                }
            }
            if (a_version >= 2) {
                if (!a_intfc->ReadRecordData(st.manualBaselineLevel)) return;
                if (!a_intfc->ReadRecordData(st.manualPointsApplied)) return;
                if (!a_intfc->ReadRecordData(st.manualExcludedLevels)) return;   // §16
                if (!a_intfc->ReadRecordData(st.nativeTreePerksAtEnroll)) return;
            }
            // Coherence guard: manual ON without a latched baseline cannot
            // happen through the verbs — a hand-edited/corrupt record reads
            // as OFF rather than as an infinite level-1 pool.
            if (st.manualSkills && st.manualBaselineLevel == 0) st.manualSkills = false;

            std::uint16_t perkCount = 0;
            if (!a_intfc->ReadRecordData(perkCount)) return;
            if (perkCount > kMaxPerkAllocs) {
                spdlog::error("[cosave] implausible perk alloc count {} -- ABORTING progression load", perkCount);
                return;
            }
            for (std::uint16_t p = 0; p < perkCount; ++p) {
                RE::FormID rawPerk = 0;
                std::uint8_t rank = 0;
                // NOTE: read unconditionally even for a dropped follower —
                // bailing early would desync every byte after this block.
                if (!a_intfc->ReadRecordData(rawPerk)) return;
                if (!a_intfc->ReadRecordData(rank)) return;
                if (!resolved) continue;
                RE::FormID resolvedPerk = 0;
                if (!a_intfc->ResolveFormID(rawPerk, resolvedPerk)) {
                    // INVARIANTS #8 + §8: DROP the alloc — the load order lost
                    // this perk. The "refund" is automatic under §17: a dropped
                    // alloc no longer debits the derived pool.
                    ++droppedPerk;
                    continue;
                }
                if (haveCatalog) {
                    // The catalog is this session's truth: a node the current
                    // trees no longer carry (overhaul swap mid-playthrough)
                    // also drops, with the same counter (same automatic refund).
                    auto ref = FindNode(resolvedPerk);
                    if (!ref.node || rank > ref.node->ranks.size()) {
                        ++droppedPerk;
                        continue;
                    }
                }
                st.perks.push_back({ resolvedPerk, rank });
            }

            std::uint16_t skillCount = 0;
            if (!a_intfc->ReadRecordData(skillCount)) return;
            if (skillCount > kMaxSkillAllocs) {
                spdlog::error("[cosave] implausible skill alloc count {} -- ABORTING progression load", skillCount);
                return;
            }
            for (std::uint16_t s = 0; s < skillCount; ++s) {
                std::uint32_t av = 0;
                float points = 0.0f, lastBase = -1.0f, manual = 0.0f;
                if (!a_intfc->ReadRecordData(av)) return;
                if (!a_intfc->ReadRecordData(points)) return;
                if (!a_intfc->ReadRecordData(lastBase)) return;
                if (a_version >= 2 && !a_intfc->ReadRecordData(manual)) return;   // §16
                float autoPts = 0.0f;
                if (a_version >= 7 && !a_intfc->ReadRecordData(autoPts)) return;   // A′
                if (!resolved) continue;
                if (!IsKnownSkillAv(av)) { ++droppedAv; continue; }   // L2: value, not just count
                if (!std::isfinite(manual) || manual < 0.0f) manual = 0.0f;   // L2 for v2
                if (!std::isfinite(autoPts) || autoPts < 0.0f) autoPts = 0.0f;
                // v6 MIGRATION (A′): the auto share is whatever is APPLIED today
                // minus the manual points — frozen as placed, never re-split.
                // Under cap saturation `points` is the CLAMPED applied delta, so
                // auto points already wasted into skillCap are not carried into
                // the ledger (a v7 grant retains them). Visible value identical;
                // matters only if skillCap is later raised (Fable F5, MFO-B11).
                if (a_version < 7) autoPts = std::max(0.0f, points - manual);
                st.skills.push_back({ static_cast<RE::ActorValue>(av), points, lastBase, manual, autoPts });
            }

            std::uint16_t baseCount = 0;
            if (!a_intfc->ReadRecordData(baseCount)) return;
            if (baseCount > kMaxSkillAllocs) {
                spdlog::error("[cosave] implausible baseline count {} -- ABORTING progression load", baseCount);
                return;
            }
            for (std::uint16_t b = 0; b < baseCount; ++b) {
                std::uint32_t av = 0;
                float value = 0.0f;
                if (!a_intfc->ReadRecordData(av)) return;
                if (!a_intfc->ReadRecordData(value)) return;
                if (!resolved) continue;
                if (!IsKnownSkillAv(av)) { ++droppedAv; continue; }   // L2: value, not just count
                st.baseline.push_back({ static_cast<RE::ActorValue>(av), value });
            }

            // ── §HMS block (read ONLY when a_version >= 5) ───────────────────
            // MUST be read UNCONDITIONALLY (even for a dropped/unresolved
            // follower) or every byte after this record desyncs. Fixed order,
            // no count prefix — mirror the write exactly. Finite/NaN-guard every
            // float (a corrupt read must never poison SetBaseActorValue).
            //   v6: per pool baseline,skew,cumulative (3 f32; target DROPPED,
            //       recomputed); then counters+captured; then hmsZeroAwardStreak
            //       (u8) + hmsGrantRemainder (f32×3) + hmsAwardAccum (f32).
            //   v5: per pool baseline,TARGET,skew,cumulative (4 f32) — the stored
            //       target is READ to stay byte-aligned then DISCARDED + recomputed;
            //       the v6 fields default (fixedStat already read false via flags,
            //       streak=0, remainder={0,0,0}).
            if (a_version >= 5) {
                const bool v6 = (a_version >= 6);
                bool hmsOk = true;
                for (int p = 0; p < 3; ++p) {
                    float b = 0.0f, t = 0.0f, sk = 0.0f, c = 0.0f;
                    if (!a_intfc->ReadRecordData(b))  return;
                    if (!v6 && !a_intfc->ReadRecordData(t)) return;   // v5 ONLY: target, discarded below
                    if (!a_intfc->ReadRecordData(sk)) return;
                    if (!a_intfc->ReadRecordData(c))  return;
                    // v5's stored target is NOT trusted (it is always derivable);
                    // exclude it from the finite check — a poisoned target must not
                    // force a re-adopt when baseline/cumulative are sound.
                    if (!std::isfinite(b) || !std::isfinite(sk) || !std::isfinite(c)) hmsOk = false;
                    st.hmsBaseline[p]   = std::isfinite(b)  ? b  : 0.0f;
                    st.hmsSkew[p]       = std::isfinite(sk) ? sk : 0.0f;
                    st.hmsCumulative[p] = std::isfinite(c)  ? c  : 0.0f;
                }
                // RECOMPUTE the held target (never read) — the invariant every
                // write site maintains: target == max(baseline, baseline+cumul).
                for (int p = 0; p < 3; ++p) {
                    const float tgt = st.hmsBaseline[p] + st.hmsCumulative[p];
                    st.hmsTarget[p] = (tgt < st.hmsBaseline[p]) ? st.hmsBaseline[p] : tgt;
                }
                std::uint32_t bSince = 0, bOff = 0;
                std::uint8_t  offPool = 0, captured = 0;
                if (!a_intfc->ReadRecordData(bSince))   return;
                if (!a_intfc->ReadRecordData(bOff))     return;
                if (!a_intfc->ReadRecordData(offPool))  return;
                if (!a_intfc->ReadRecordData(captured)) return;
                st.battlesSinceLevelUp = bSince;
                st.battlesOffClass     = std::min(bOff, bSince);   // ratio ≤ 1 by construction
                st.offClassPool        = (offPool <= 3) ? offPool : 0;   // clamp [0,3]
                // A corrupt (non-finite) HMS payload → force re-ADOPT on the next
                // RecomputeHMS rather than trusting a poisoned baseline.
                st.hmsCaptured = (captured != 0) && hmsOk;
                if (v6) {
                    // v6 Phase-3 additions.
                    std::uint8_t streak = 0;
                    if (!a_intfc->ReadRecordData(streak)) return;
                    st.hmsZeroAwardStreak = std::min<std::uint8_t>(streak, 2);   // clamp 0..2
                    for (int p = 0; p < 3; ++p) {
                        float r = 0.0f;
                        if (!a_intfc->ReadRecordData(r)) return;
                        // contract is 0 <= frac < 1 — reject finite-but-out-of-range
                        // corruption (a huge value would slam SetBaseActorValue).
                        st.hmsGrantRemainder[p] =
                            (std::isfinite(r) && r >= 0.0f && r < 1.0f) ? r : 0.0f;
                    }
                    float acc = 0.0f;   // detection tally (serialized in v6)
                    if (!a_intfc->ReadRecordData(acc)) return;
                    st.hmsAwardAccum = (std::isfinite(acc) && acc >= 0.0f) ? acc : 0.0f;
                }
                // A poisoned HMS payload forces a re-ADOPT (hmsCaptured=false); it
                // must ALSO restart detection so a stale flags-bit 0x20 (fixedStat)
                // or serialized streak/tally can't linger against a freshly
                // re-adopted baseline. Applied AFTER the v6 reads so it wins.
                if (!hmsOk) {
                    st.fixedStat          = false;
                    st.hmsZeroAwardStreak = 0;
                    st.hmsAwardAccum      = 0.0f;
                }
                // a_version == 5: fixedStat=false (flags), streak=0, remainder={0}
                // (struct defaults) — a v5 follower begins fresh 0-award detection.
            }
            // a_version < 5: no HMS block on disk → hmsCaptured stays false
            // (struct default) → the first RecomputeHMS adopts the live base.

            // ── v7 block (A′ ledger + B′ strip record), read UNCONDITIONALLY for
            // stream alignment; applied only when resolved. ─────────────────────
            if (a_version >= 7) {
                std::uint16_t granted = 0, strippedCount = 0;
                std::uint8_t  freeRespec = 0;
                if (!a_intfc->ReadRecordData(granted))       return;
                if (!a_intfc->ReadRecordData(freeRespec))    return;
                if (!a_intfc->ReadRecordData(strippedCount)) return;
                if (strippedCount > kMaxPerkAllocs) {
                    spdlog::error("[cosave] implausible stripped-perk count {} -- ABORTING progression load", strippedCount);
                    return;
                }
                st.autoLevelsGranted = granted;
                st.freeRespec        = (freeRespec != 0);
                for (std::uint16_t i = 0; i < strippedCount; ++i) {
                    RE::FormID raw = 0, res = 0;
                    if (!a_intfc->ReadRecordData(raw)) return;
                    if (resolved && a_intfc->ResolveFormID(raw, res) && res)
                        st.strippedPerks.push_back(res);   // unresolvable: the perk left the load order
                }
            } else {
                // v6 MIGRATION (A′): every auto level up to today's partition counts
                // as GRANTED — nothing pending, so the first RecomputeSkills holds
                // today's applied values and re-splits nothing. B′: nothing was ever
                // stripped; the PollWork active-edge strips an active follower on
                // the first poll (logged per perk).
                const int effAutoLvl = std::max(0,
                    (st.manualSkills ? static_cast<int>(st.manualBaselineLevel)
                                     : static_cast<int>(st.progressionLevel)) -
                    static_cast<int>(st.manualExcludedLevels));
                st.autoLevelsGranted = static_cast<std::uint16_t>(std::max(0, effAutoLvl - 1));
                // ONE FREE RESPEC (marth 2026-09-14): these applied values came
                // from the OLD drifting split and are now frozen as they stood —
                // the escape hatch. Only a v6-born record earns it.
                st.freeRespec = true;
            }

            // ── v8 block (§HMS player-rate parity), read UNCONDITIONALLY for
            // alignment. v<8: the ONE-TIME retro takes back the NPC-rate excess.
            if (a_version >= 8) {
                float w = 0.0f, c = 0.0f;
                if (!a_intfc->ReadRecordData(w)) return;
                if (!a_intfc->ReadRecordData(c)) return;
                st.hmsWithheld     = (std::isfinite(w) && w >= 0.0f) ? w : 0.0f;
                st.hmsParityCredit = (std::isfinite(c) && c >= 0.0f) ? c : 0.0f;
                for (int p = 0; p < 3; ++p) {
                    float h = 0.0f;
                    if (!a_intfc->ReadRecordData(h)) return;
                    st.hmsHeld[p] = (std::isfinite(h) && h >= 0.0f) ? h : st.hmsTarget[p];
                }
            } else {
                // v<8: v7 HELD baseline + cumulative (== the recomputed target),
                // captured BEFORE the retro scales it, so the retro's drop heals.
                for (int p = 0; p < 3; ++p) st.hmsHeld[p] = st.hmsTarget[p];
                if (resolved) HmsRetroParity(resolvedID, st);   // ProgAllocator_Hms.cpp
            }

            if (!resolved) { ++droppedActor; continue; }
            g_prog[resolvedID] = std::move(st);
            ++loaded;
        }
        // Split counters by reason (INVARIANTS #47).
        spdlog::info("[cosave] loaded {} progression record(s); dropped {} unresolvable actor(s), "
                     "{} unresolvable/off-catalog perk alloc(s) (§17 auto-refund), "
                     "{} unknown-skill AV entr(ies); lastPlayerLevel {}",
                     loaded, droppedActor, droppedPerk, droppedAv, g_lastPlayerLevel);
    }

    void ClearAll() {
        g_prog.clear();
        g_lastPlayerLevel = 0;
        g_playerHmsTotalLast = 0.0f;   // §HMS Phase 3: re-seeded on the next load/observe
        ++g_pollGen;   // orphan any in-flight poll chain (MainThread::Clear
                       // drops the queued closure too — belt and braces)
        // §HMS: drop the off-class fire mirror (runtime-only, save-scoped).
        // Own lock, taken + released BEFORE g_viewMx — never nested.
        { std::scoped_lock fl(g_hmsFireMx); g_hmsFiredMask.clear(); }
        // Drop the published board views: a view built from the old save must
        // never be drawn over a freshly loaded one (the ClearPendingEdits rule
        // applied to reads). OnPostLoad reseeds it.
        std::scoped_lock lk(g_viewMx);
        g_boardSnap.reset();
    }

    int PollGeneration() { return g_pollGen; }
}
