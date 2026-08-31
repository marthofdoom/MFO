// ProgAllocator_Manifest.cpp -- the §18.6 manifest reader side of the allocator
// (split mechanically out of ProgAllocator.cpp, no logic change): economy GLOB
// discovery + the live MCM INI overlay, class-def FLST parsing, Init (the one
// kDataLoaded parse pass), the v1.1 GENERIC add-on manifest model
// (BuildGenericManifests / Progression::Manifests()), and the PRGN
// class-identity resolvers (DeriveClassIdentity / LookupAddonForm) used by the
// co-save block that stays in ProgAllocator.cpp. Shared state (g_econ,
// g_econDefaults, g_classes, g_manifests, g_ready, g_devCmd, kSkillNames)
// lives in ProgAllocator_internal.h.
#include "ProgAllocator_internal.h"
#include <fstream>   // ApplyEconomyOverride reads the addon's MCM Settings INI

namespace MFO::ProgAllocator {

    namespace {

        // ── §18.6 manifest class parsing (Init helpers) ─────────────────────
        // NO fixed ids: classes are declared through the enumerated manifests.
        // A class-def is ONE BGSListForm whose entries dispatch by form type:
        // MESG (display name) | AVIF (a skill, order = weight) | PERK (auto-
        // pick priority) | GLOB suffixed "_Stance" (#65 combat-stance mirror).

        // True iff the global's runtime editor id ends with a_suffix. Globals
        // keep their editor ids at runtime (console-addressable), so the
        // stance knob is matched by suffix, never a fixed id.
        bool EdidEndsWith(RE::TESGlobal* a_glob, std::string_view a_suffix) {
            const char* e = a_glob->GetFormEditorID();
            if (!e) return false;
            const std::string_view id{ e };
            return id.size() >= a_suffix.size() &&
                   id.substr(id.size() - a_suffix.size()) == a_suffix;
        }

        // ── §18.6 Stage 3: economy is FULLY addon-declared ──────────────────
        // Each economy knob is a manifest GLOB matched by editor-id SUFFIX.
        // Order in this list == index order in AssignEconomyGlob's returns; it
        // exists only so Init can name the knobs that fell back to the default.
        constexpr std::string_view kEconKnobSuffixes[] = {
            "_LevelsPerPerkPoint", "_ManualSkillPointsPerLevel", "_SkillPointsPerLevel",
            "_SharedGrowthDivisor", "_RespecRapportCost", "_SkillCap",
        };
        constexpr int kEconKnobCount = 6;

        // Assign ONE manifest GLOB to its g_econ field by editor-id SUFFIX and
        // return its kEconKnobSuffixes index (0..5); 6 = the _DevCmd selector
        // (pointer stored, read LIVE); -1 = no economy/dev suffix matched.
        // Manual rate is tested BEFORE the auto rate so the longer id wins even
        // if a future suffix would otherwise be a tail of another. Same clamp
        // discipline the old fixed-id reads used (int knobs floored at 1/0).
        int AssignEconomyGlob(RE::TESGlobal* a_glob) {
            if (EdidEndsWith(a_glob, "_LevelsPerPerkPoint")) {
                g_econ.levelsPerPerkPoint = std::max(1, static_cast<int>(a_glob->value));
                return 0;
            }
            if (EdidEndsWith(a_glob, "_ManualSkillPointsPerLevel")) {
                g_econ.manualSkillPtsPerLevel = std::max(0, static_cast<int>(a_glob->value));
                return 1;
            }
            if (EdidEndsWith(a_glob, "_SkillPointsPerLevel")) {
                g_econ.skillPointsPerLevel = a_glob->value;
                return 2;
            }
            if (EdidEndsWith(a_glob, "_SharedGrowthDivisor")) {
                g_econ.sharedGrowthDivisor = std::max(1, static_cast<int>(a_glob->value));
                return 3;
            }
            if (EdidEndsWith(a_glob, "_RespecRapportCost")) {
                // Clamp ≥0: a negative respec cost would make Rapport::Spend a
                // GRANT (respec pays the follower).
                g_econ.respecRapportCost = std::max(0.0f, a_glob->value);
                return 4;
            }
            if (EdidEndsWith(a_glob, "_SkillCap")) {
                // Clamp ≥1: skillCap ≤ 0 neutralizes every auto-scale skill write.
                g_econ.skillCap = std::max(1.0f, a_glob->value);
                return 5;
            }
            if (EdidEndsWith(a_glob, "_DevCmd")) {
                g_devCmd = a_glob;   // pointer only — the ONE live-read knob
                return 6;
            }
            return -1;
        }

        // §18.6 Stage 3: read EVERY economy knob's RECORD DEFAULT from the
        // enumerated addon manifests into g_econ. **Main-thread only** — writes
        // g_econ (the §5 discipline). ***CALLED ONCE, AT INIT (kDataLoaded)
        // ONLY.*** Do NOT re-call it post-load or on menu close: a GLOB's
        // runtime value is SAVE-PERSISTED, so a re-read after a load feeds a
        // stale saved number back into the economy (the 2026-08-17 doubled-perk-
        // pool bug — a repurposed GLOB's old value read floor(level/1)). The
        // live MCM override rides a NON-save-persisted INI instead
        // (ApplyEconomyOverride), which is what post-load / menu-close call.
        // Classes are static
        // and are NOT re-parsed here. This is FULLY GENERIC: it walks
        // Progression::Addons() and matches by editor-id SUFFIX; it never names
        // any addon plugin, so the DLL stays ignorant of the addon it reads —
        // exactly as it would a third-party addon.
        void ReloadEconomy() {
            bool resolvedEcon[kEconKnobCount] = {};   // which knobs a manifest set
            for (const auto& addon : Progression::Addons()) {
                auto* manifest = RE::TESForm::LookupByID<RE::BGSListForm>(addon.manifestID);
                if (!manifest) continue;
                for (auto* form : manifest->forms) {
                    // Skip the self-declaration keyword (manifest entry[0]) generically.
                    if (!form || form->Is(RE::FormType::Keyword)) continue;
                    auto* glob = form->As<RE::TESGlobal>();
                    if (!glob) continue;   // FLSTs (classes) are Init's job, not economy
                    const int idx = AssignEconomyGlob(glob);
                    const char* edid = glob->GetFormEditorID();
                    if (idx < 0) {
                        spdlog::warn("[prog] manifest {:08X}: GLOB {:08X} \"{}\" matches no "
                                     "economy knob — ignored", addon.manifestID,
                                     glob->GetFormID(), edid ? edid : "");
                    } else {
                        if (idx < kEconKnobCount) resolvedEcon[idx] = true;
                        spdlog::info("[prog] economy: {} = {:g} (from {})",
                                     idx < kEconKnobCount ? kEconKnobSuffixes[idx] : "_DevCmd",
                                     glob->value, edid ? edid : "?");
                    }
                }
            }
            // Name every knob that fell back to the DLL default (diagnosable).
            for (int k = 0; k < kEconKnobCount; ++k)
                if (!resolvedEcon[k])
                    spdlog::info("[prog] economy: {} absent from all manifests — DLL default",
                                 kEconKnobSuffixes[k]);
        }

        // Case-insensitive "does key end with core". INI keys are the addon's
        // MCM ModSetting ids (e.g. "iLevelsPerPerkPoint") — a type char prefix,
        // no leading underscore, so we match the knob core, not the GLOB suffix.
        bool KeyEndsWith(const std::string& a_key, std::string_view a_core) {
            if (a_key.size() < a_core.size()) return false;
            return _stricmp(a_key.c_str() + (a_key.size() - a_core.size()),
                            std::string(a_core).c_str()) == 0;
        }

    }   // anonymous namespace

        // §18.6 (perk-bug fix 2026-08-17): the LIVE MCM override. Reset g_econ to
        // the cached record DEFAULTS, then overlay each registered addon's MCM
        // Settings INI — Data/MCM/Settings/<addon-basename>.ini, which MCM Helper
        // ModSetting controls write and which is NOT save-persisted, so a slider
        // edit is live WITHOUT the stale-save-GLOB perk corruption. The path is
        // derived from the addon plugin name (AddonRef.plugin) — never hardcoded,
        // so the DLL stays addon-agnostic. Main-thread only (writes g_econ).
        // "Manual" is tested before the shorter "SkillPointsPerLevel" tail.
        void ApplyEconomyOverride() {
            g_econ = g_econDefaults;
            auto trim = [](std::string s) {
                s.erase(0, s.find_first_not_of(" \t\r\n"));
                if (auto p = s.find_last_not_of(" \t\r\n"); p != std::string::npos) s.erase(p + 1);
                else s.clear();
                return s;
            };
            for (const auto& addon : Progression::Addons()) {
                std::string mod = addon.plugin;
                if (auto d = mod.rfind('.'); d != std::string::npos) mod.erase(d);
                if (mod.empty()) continue;
                std::ifstream f("Data/MCM/Settings/" + mod + ".ini");
                if (!f) continue;
                std::string line;
                int applied = 0;
                while (std::getline(f, line)) {
                    const auto eq = line.find('=');
                    if (eq == std::string::npos) continue;
                    const std::string key = trim(line.substr(0, eq));
                    const std::string valS = trim(line.substr(eq + 1));
                    if (key.empty() || valS.empty() ||
                        key.front() == '[' || key.front() == ';' || key.front() == '#') continue;
                    float v;
                    try { v = std::stof(valS); } catch (...) { continue; }
                    if (KeyEndsWith(key, "LevelsPerPerkPoint"))
                        g_econ.levelsPerPerkPoint = std::max(1, static_cast<int>(v));
                    else if (KeyEndsWith(key, "ManualSkillPointsPerLevel"))
                        g_econ.manualSkillPtsPerLevel = std::max(0, static_cast<int>(v));
                    else if (KeyEndsWith(key, "SkillPointsPerLevel"))
                        g_econ.skillPointsPerLevel = v;
                    else if (KeyEndsWith(key, "SharedGrowthDivisor"))
                        g_econ.sharedGrowthDivisor = std::max(1, static_cast<int>(v));
                    else if (KeyEndsWith(key, "RespecRapportCost"))
                        g_econ.respecRapportCost = std::max(0.0f, v);   // ≥0: negative = a grant
                    else if (KeyEndsWith(key, "SkillCap"))
                        g_econ.skillCap = std::max(1.0f, v);            // ≥1: ≤0 kills skill writes
                    else if (KeyEndsWith(key, "CancelEngineAwards"))
                        g_econ.cancelEngineAwards = (v != 0.0f);        // §4.2 revert vs adopt
                    else if (KeyEndsWith(key, "SharedGrowth"))          // AFTER *Divisor above
                        g_econ.sharedGrowthEnabled = (v != 0.0f);       // §15 master toggle
                    else
                        continue;
                    ++applied;
                }
                if (applied)
                    spdlog::info("[prog] economy override from {}.ini: {} knob(s) — perk 1/{} lvl, "
                                 "skill/lvl {:g}, manual/lvl {}, sharedDiv {}, respec {:g}, cap {:g}, "
                                 "cancelEngineAwards {}",
                                 mod, applied, g_econ.levelsPerPerkPoint, g_econ.skillPointsPerLevel,
                                 g_econ.manualSkillPtsPerLevel, g_econ.sharedGrowthDivisor,
                                 g_econ.respecRapportCost, g_econ.skillCap, g_econ.cancelEngineAwards);
            }
        }

    namespace {

        // AVIF → ActorValue through the LIVE ActorValueList — no hardcoded
        // AVIF ids, so a load order that replaces the records still maps.
        RE::ActorValue MapSkillAvif(RE::TESForm* a_form) {
            auto* avl = RE::ActorValueList::GetSingleton();
            if (!avl) return RE::ActorValue::kNone;
            for (const auto& s : kSkillNames) {
                auto* avi = avl->GetActorValue(s.av);
                if (avi && avi->GetFormID() == a_form->GetFormID()) return s.av;
            }
            return RE::ActorValue::kNone;
        }

        // One class-def FLST → a ClassDef. Type-dispatched, order-free except
        // within-type order (AVIF order = weight, PERK order = pick priority).
        // Robust: an unresolved/unknown entry is skipped with a warning, never
        // a crash. Returns false when the list declares no usable skills.
        bool ParseClassDef(RE::BGSListForm* a_list, ClassDef& a_out) {
            a_out.id = a_list->GetFormID();
            // GLOBs are collected in FLST ORDER and interpreted POSITIONALLY — the
            // engine DISCARDS editor-ids for TESGlobal at runtime (GetFormEditorID()
            // returns "", the Phase 2 root cause), so the old "_Stance" suffix match
            // never fired and stance always parsed 0. Order is the reliable carrier:
            //   glob[0] = combat stance (0-3)
            //   glob[1..3] = HMS weights H,M,S   (v1.1 class ratios as data)
            //   glob[4] = primary pool 0=H/1=M/2=S
            std::vector<RE::TESGlobal*> globs;
            for (auto* form : a_list->forms) {
                if (!form) continue;
                if (auto* msg = form->As<RE::BGSMessage>()) {
                    const char* full = msg->GetFullName();
                    if (full && *full && a_out.name.empty()) a_out.name = full;
                } else if (auto* glob = form->As<RE::TESGlobal>()) {
                    globs.push_back(glob);
                } else if (form->Is(RE::FormType::Perk)) {
                    a_out.perkPriority.push_back(form->GetFormID());
                } else if (const auto av = MapSkillAvif(form); av != RE::ActorValue::kNone) {
                    a_out.skills.push_back(av);
                } else {
                    spdlog::warn("[prog] class-def {:08X}: entry {:08X} is not a MESG/"
                                 "GLOB/PERK/skill-AVIF — skipped",
                                 a_list->GetFormID(), form->GetFormID());
                }
            }
            if (!globs.empty())
                a_out.stance = static_cast<std::uint8_t>(
                    std::clamp(static_cast<int>(globs[0]->value), 0, 3));
            if (globs.size() >= 4) {
                a_out.hmsWeights[0] = globs[1]->value;
                a_out.hmsWeights[1] = globs[2]->value;
                a_out.hmsWeights[2] = globs[3]->value;
                a_out.hmsWeightsSet  = true;
            }
            if (globs.size() >= 5)
                a_out.primaryPool = static_cast<std::uint8_t>(
                    std::clamp(static_cast<int>(globs[4]->value), 0, 2));
            if (a_out.name.empty())
                a_out.name = std::format("Class {:08X}", a_out.id);
            return !a_out.skills.empty();
        }

    }   // anonymous namespace

    // Forwarder so Progression::Manifests() (defined at the foot of this TU, in a
    // different namespace) can reach the anon-namespace store.
    const std::vector<Progression::AddonManifest>& ManifestsRef() { return g_manifests; }

    // v1.1: populate the GENERIC add-on manifest model (Progression::AddonManifest)
    // from the just-parsed progression data (g_classes + g_econ) — one manifest per
    // registered add-on. Add-on-agnostic: it copies DATA, no progression concept
    // leaks into a signature. PARSES only; nothing consumes g_manifests yet. Called
    // at the foot of Init, after classes + economy are parsed. Main-thread (§5).
    void BuildGenericManifests() {
        g_manifests.clear();
        for (const auto& addon : Progression::Addons()) {
            Progression::AddonManifest man;
            man.manifestID  = addon.manifestID;
            man.plugin      = addon.plugin;
            man.addonType   = addon.keywordEdid;   // self-declared type (join keyword)
            man.displayName = addon.plugin;        // TODO(Phase 5): richer header carrier
            // Economy + allocation mirror the live g_econ (whatever it resolved to).
            man.economy.levelsPerPerkPoint        = g_econ.levelsPerPerkPoint;
            man.economy.skillPointsPerLevel       = g_econ.skillPointsPerLevel;
            man.economy.manualSkillPointsPerLevel = g_econ.manualSkillPtsPerLevel;
            man.economy.sharedGrowthDivisor       = g_econ.sharedGrowthDivisor;
            man.economy.respecRapportCost         = g_econ.respecRapportCost;
            man.economy.skillCap                  = g_econ.skillCap;
            man.economy.cancelEngineAwards        = g_econ.cancelEngineAwards;
            man.economy.sharedGrowthEnabled       = g_econ.sharedGrowthEnabled;
            // Classes: re-walk this add-on's classes list, pulling the ALREADY-parsed
            // ClassDef (with hmsWeights) via FindClassDef — no re-parse, no duplication.
            std::string boardLabel;   // Phase 6c: self-declared board-tab title (MESG FULL)
            auto* manifest = RE::TESForm::LookupByID<RE::BGSListForm>(addon.manifestID);
            if (manifest) {
                for (auto* form : manifest->forms) {
                    // v1.1 Phase 6c: the add-on's board-tab label — a MESG whose
                    // FULL titles the hosted tab (no "Progression" literal in the
                    // DLL). Precedes the classes FLST in the manifest so it is
                    // seen before the break below.
                    if (auto* msg = form ? form->As<RE::BGSMessage>() : nullptr) {
                        if (const char* full = msg->GetFullName();
                            full && *full && boardLabel.empty())
                            boardLabel = full;
                        continue;
                    }
                    auto* classesList = form ? form->As<RE::BGSListForm>() : nullptr;
                    if (!classesList) continue;   // keyword / economy GLOBs
                    for (auto* cf : classesList->forms) {
                        auto* defList = cf ? cf->As<RE::BGSListForm>() : nullptr;
                        const ClassDef* def = defList ? FindClassDef(defList->GetFormID()) : nullptr;
                        if (!def) continue;
                        Progression::ManifestClass mc;
                        mc.id            = def->id;
                        mc.name          = def->name;
                        mc.stance        = def->stance;
                        mc.skills        = def->skills;
                        mc.perkPriority  = def->perkPriority;
                        mc.hmsWeights[0] = def->hmsWeights[0];
                        mc.hmsWeights[1] = def->hmsWeights[1];
                        mc.hmsWeights[2] = def->hmsWeights[2];
                        mc.primaryPool   = def->primaryPool;
                        mc.hmsWeightsSet = def->hmsWeightsSet;
                        man.classes.push_back(std::move(mc));
                    }
                    break;   // one classes list per manifest (Init warns on extras)
                }
            }
            // v1.1 Phase 6a: this add-on hosts a board (Field-Orders) tab. Board.cpp
            // iterates declared board tabs instead of hardcoding the progression tab
            // — delete the add-on and the board renders zero add-on tabs. label is
            // the add-on's own display name (no progression string in the DLL).
            man.boardTab.declared = true;
            // Phase 6c: title from the add-on's own MESG (self-declared); the
            // display name is only the fallback for an add-on that ships none.
            man.boardTab.label    = boardLabel.empty() ? man.displayName : boardLabel;
            spdlog::info("[prog] generic manifest {:08X} (\"{}\", type \"{}\"): {} class(es) "
                         "modeled, board tab declared [parsed, unused]", man.manifestID, man.plugin,
                         man.addonType, man.classes.size());
            g_manifests.push_back(std::move(man));
        }
    }

    void Init() {
        if (!Progression::Detected()) {
            // One named line, never an error — the addon is optional (§1).
            spdlog::info("[prog] allocator inert (addon absent)");
            return;
        }
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            spdlog::error("[prog] TESDataHandler unavailable — allocator cannot initialize");
            return;
        }

        // §18.6 Stage 3: CLASSES **and** ECONOMY are declared through the
        // enumerated manifests — NO fixed-id GLOB reads survive. Per registered
        // manifest, the ONE non-sentinel FLST is the classes list (its entries
        // are class-def FLSTs, ParseClassDef); every GLOB entry is an economy
        // knob matched by editor-id SUFFIX (AssignEconomyGlob), last-writer-wins
        // across manifests in load order. g_econ's initializers stand as the
        // documented DEFAULT for any knob no manifest declares. Values are the
        // RECORD DEFAULTS (kDataLoaded is pre-save, §10); _DevCmd stores the
        // pointer (read LIVE). unused (unread) legacy GLOBs — perk rate 0x802 /
        // veteran mult 0x806 — are simply never in the manifest.
        // §18.6 Stage 3: the ECONOMY record-default read is factored into
        // ReloadEconomy(), called ONCE here (kDataLoaded is genuinely pre-save).
        // It is NEVER re-run post-load/menu-close — see ReloadEconomy's header:
        // GLOB runtime values are save-persisted (the doubled-perk-pool bug). The
        // loop below parses only the CLASSES (static — parsed once at Init).
        ReloadEconomy();
        g_econDefaults = g_econ;    // cache the RECORD DEFAULTS (§10) before any
        ApplyEconomyOverride();     // save loads; overlay the MCM INI (if present)
        for (const auto& addon : Progression::Addons()) {
            auto* manifest = RE::TESForm::LookupByID<RE::BGSListForm>(addon.manifestID);
            if (!manifest) continue;   // enumerated moments ago; belt and braces
            RE::BGSListForm* classesList = nullptr;
            for (auto* form : manifest->forms) {
                // Skip the self-declaration keyword (manifest entry[0]) generically.
                if (!form || form->Is(RE::FormType::Keyword)) continue;
                if (auto* flst = form->As<RE::BGSListForm>()) {
                    if (!classesList) classesList = flst;
                    else spdlog::warn("[prog] manifest {:08X}: extra FLST {:08X} ignored "
                                      "(one classes list per manifest)",
                                      addon.manifestID, flst->GetFormID());
                } else if (form->As<RE::TESGlobal>()) {
                    // economy GLOB — read by ReloadEconomy() above; nothing here.
                } else if (form->As<RE::BGSMessage>()) {
                    // v1.1 Phase 6c: the board-tab label MESG — its FULL is the
                    // hosted tab's title, captured by BuildGenericManifests into
                    // boardTab.label. Recognized here so it never warns.
                } else {
                    spdlog::warn("[prog] manifest {:08X}: entry {:08X} is neither an FLST "
                                 "(classes) nor a GLOB (economy) — ignored",
                                 addon.manifestID, form->GetFormID());
                }
            }
            int parsed = 0;
            if (classesList) {
                for (auto* form : classesList->forms) {
                    auto* defList = form ? form->As<RE::BGSListForm>() : nullptr;
                    if (!defList) {
                        spdlog::warn("[prog] classes list {:08X}: entry is not a class-def "
                                     "FLST — skipped", classesList->GetFormID());
                        continue;
                    }
                    if (FindClassDef(defList->GetFormID())) continue;   // dedupe across addons
                    ClassDef def;
                    if (ParseClassDef(defList, def)) {
                        spdlog::info("[prog] class \"{}\" ({:08X}): {} skill(s), {} perk "
                                     "priorit(ies), stance {}",
                                     def.name, def.id, def.skills.size(),
                                     def.perkPriority.size(), def.stance);
                        g_classes.push_back(std::move(def));
                        ++parsed;
                    } else {
                        spdlog::warn("[prog] class-def {:08X} (\"{}\") declares no usable "
                                     "skills — dropped", def.id, def.name);
                    }
                }
            }
            spdlog::info("[prog] addon \"{}\" ({:08X}): {} class(es) registered",
                         addon.plugin, addon.manifestID, parsed);
        }

        g_ready = true;
        spdlog::info("[prog] allocator ready: perk = 1 per {} level(s), skill/lvl {:g}, "
                     "manual skill/lvl {}, sharedDiv {}, respec {:g} rapport, cap {:g}, "
                     "{} declared class(es), devCmd {}",
                     g_econ.levelsPerPerkPoint, g_econ.skillPointsPerLevel,
                     g_econ.manualSkillPtsPerLevel, g_econ.sharedGrowthDivisor,
                     g_econ.respecRapportCost, g_econ.skillCap, g_classes.size(),
                     g_devCmd ? "resolved" : "MISSING (harness cmds default to 0)");

        // v1.1: mirror everything just parsed into the GENERIC manifest model
        // (parsed, unused this phase — see BuildGenericManifests).
        BuildGenericManifests();
    }

        // Derive a class-def form's SOURCE plugin filename + local FormID (the
        // stable identity). False if the form is gone this session — then the
        // caller falls back to the already-loaded clsPlugin/clsLocal.
        bool DeriveClassIdentity(RE::FormID a_clsId, std::string& a_plugin, RE::FormID& a_local) {
            auto* form = RE::TESForm::LookupByID(a_clsId);
            if (!form) return false;
            auto* file = form->GetFile(0);
            if (!file) return false;
            a_plugin = std::string(file->GetFilename());
            a_local  = form->GetLocalFormID();
            return true;
        }

        // #21 addon .esl <-> .esp sibling resolve (READ-SIDE ONLY -- no co-save
        // format change). The progression addon ships BOTH an ESL and a Vortex-
        // compatible ESP variant (an ESL that masters a regular ESP is an
        // unbreakable Vortex load-order cycle). Both carry the SAME masters + local
        // ids, so the ONLY difference the DLL sees is the plugin NAME. A save written
        // under one name must still resolve its class under the other. Try the stored
        // name; if that misses and it is one of the two known addon names, retry the
        // sibling. Anything else resolves plainly (no sibling). The next save then
        // self-heals to the actually-loaded plugin (CoSaveSave's DeriveClassIdentity).
        RE::TESForm* LookupAddonForm(RE::FormID a_local, std::string_view a_plugin) {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return nullptr;
            if (auto* f = dh->LookupForm(a_local, a_plugin)) return f;
            constexpr std::string_view kEsl = "MFO_Progression.esl";
            constexpr std::string_view kEsp = "MFO_Progression.esp";
            std::string_view sib;
            if      (a_plugin == kEsl) sib = kEsp;
            else if (a_plugin == kEsp) sib = kEsl;
            else return nullptr;   // not an addon name -> no sibling
            return dh->LookupForm(a_local, sib);
        }

}

// v1.1: the GENERIC add-on manifest model is host-side (Progression namespace),
// but is populated in this TU where the progression data is parsed. Defined here
// (not in Progression.cpp) so it reads the store directly. Parsed, unused yet.
namespace MFO::Progression {
    const std::vector<AddonManifest>& Manifests() { return MFO::ProgAllocator::ManifestsRef(); }
}
