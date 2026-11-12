// classes/Package.cpp -- class COMBAT PACKAGES, the data half (class update card C8). ClickUp 86e3dhbd3.
//
// WHAT A PACKAGE IS: three engine-facing facets of a class, each applied by its own subsystem through Harbinger or the
// CSTY write, never by an MFO-side engine action:
//   - the class's CSTY per stance  -> CombatStyle::Want / ApplyTick   (this file resolves them)
//   - the ch.18 attack mix         -> apmf/AttackMix.cpp              (ClassDef::powerShareMax / powerShareMin)
//   - the ch.23 leash length       -> Scheduler ServiceLeashWithReachHold (PackageLeashScale below)
// Packages never choose targets and never swap weapons (marth 2026-11-09). Tier 1 has NO package ("t1 behaves vanilla").
//
// THE CSTY LOOKUP (the C7 STYLE SLOTS contract, MFO_GenerateESP.py): manifest FLST entry[2] = MFOC_ClassStyles, 35 entries
// in class-Id order, each a per-class FLST whose entry[0] is a GLOB bitmask (1 melee single, 2 melee DUAL twin, 4 ranged, 8 cast;
// the GLOB's VALUE persists at runtime, its editor-id does not) and whose entries[1..] are that class's CSTYs in ASCENDING slot-bit
// order. Nothing here names a FormID: the DLL walks the lists. A shape that does not match (wrong count, a non-GLOB front,
// popcount(mask) != entries - 1, a non-CSTY entry) logs LOUDLY and leaves that class unresolved, so it keeps today's MFO_*Style.
//
// THREADING: ResolvePackageStyles runs once on the main thread at kDataLoaded; PackageStyle then reads immutable data (any thread,
// including the combat thread); PackageClass / PackageLeashScale take the State mutex (GetEquipped) so they are worker-only.
#include "Classes_internal.h"
#include "Config.h"

#include <bit>
#include <unordered_map>

namespace MFO::Classes {

    namespace {
        constexpr std::uint8_t kSlotMelee = 1, kSlotMeleeDual = 2, kSlotRanged = 4, kSlotCast = 8;

        struct ClassStyles {
            RE::TESCombatStyle* melee     = nullptr;
            RE::TESCombatStyle* meleeDual = nullptr;
            RE::TESCombatStyle* ranged    = nullptr;
            RE::TESCombatStyle* cast      = nullptr;
        };
        std::array<ClassStyles, kClassCount> g_styles{};   // written ONCE at kDataLoaded, read-only after
        // Zero-avoid twin of a class CSTY (heavy armor / Living Legend's held guard: apmf/StyleClaims.cpp). Key = the class style's
        // FormID, value = the twin's (the style itself when it has no Avoid Threat). Written ONCE at kDataLoaded, read-only after.
        std::unordered_map<RE::FormID, RE::FormID> g_noDodge;
    }

    void detail::ResolvePackageStyles() {
        g_styles = {};
        g_noDodge.clear();
        auto* manifest = detail::ManifestList();
        if (!manifest || manifest->forms.size() < 3 || !manifest->forms[2]) {
            spdlog::error("[classes] package styles: the manifest has no style lookup entry (entry[2]) -- every class keeps "
                          "today's MFO_*Style");
            return;
        }
        auto* all = manifest->forms[2]->As<RE::BGSListForm>();
        // 35 entries = the class lists; 70 = those plus the zero-avoid TWIN lists (entries 35..69, the same shape). A list from before
        // the twins has 35: every style then has no twin (a heavy-armored follower keeps the class Avoid Threat; said once below).
        if (!all || (all->forms.size() != kClassCount && all->forms.size() != 2 * kClassCount)) {
            spdlog::error("[classes] package styles: the style lookup {:08X} is not a list of {} class lists ({} entries) -- every "
                          "class keeps today's MFO_*Style", manifest->forms[2]->GetFormID(), kClassCount, all ? all->forms.size() : 0);
            return;
        }
        const bool hasTwins = all->forms.size() == 2 * kClassCount;
        std::size_t resolved = 0, styles = 0;
        for (std::size_t i = 0; i < kClassCount; ++i) {
            auto* cl = all->forms[i] ? all->forms[i]->As<RE::BGSListForm>() : nullptr;
            if (!cl || cl->forms.empty()) {
                spdlog::error("[classes] package styles: class {} has no style list -- it keeps today's styles", kTable[i].name);
                continue;
            }
            auto* mask = cl->forms.front() ? cl->forms.front()->As<RE::TESGlobal>() : nullptr;
            if (!mask) {
                spdlog::error("[classes] package styles: class {}'s list does not start with its slot mask GLOB -- it keeps today's "
                              "styles", kTable[i].name);
                continue;
            }
            const auto bits = static_cast<std::uint32_t>(mask->value + 0.5f);
            if (static_cast<std::size_t>(std::popcount(bits)) != cl->forms.size() - 1) {
                spdlog::error("[classes] package styles: class {}'s mask {} names {} slot(s) but the list holds {} style(s) -- it "
                              "keeps today's styles", kTable[i].name, bits, std::popcount(bits), cl->forms.size() - 1);
                continue;
            }
            ClassStyles cs{};
            std::size_t next = 1;
            bool ok = true;
            for (const std::uint8_t slot : { kSlotMelee, kSlotMeleeDual, kSlotRanged, kSlotCast }) {
                if (!(bits & slot)) continue;
                auto* csty = cl->forms[next] ? cl->forms[next]->As<RE::TESCombatStyle>() : nullptr;
                ++next;
                if (!csty) { ok = false; break; }
                switch (slot) {
                    case kSlotMelee:     cs.melee = csty; break;
                    case kSlotMeleeDual: cs.meleeDual = csty; break;
                    case kSlotRanged:    cs.ranged = csty; break;
                    default:             cs.cast = csty; break;
                }
            }
            if (!ok) {
                spdlog::error("[classes] package styles: class {} lists a non-CSTY form in a style slot -- it keeps today's styles",
                              kTable[i].name);
                continue;
            }
            g_styles[i] = cs;
            ++resolved;
            styles += cl->forms.size() - 1;
            if (!hasTwins) continue;
            // The twin list: same length, same order; every entry a CSTY. A mismatch drops this class's twins only (loud).
            auto* tl = all->forms[kClassCount + i] ? all->forms[kClassCount + i]->As<RE::BGSListForm>() : nullptr;
            bool tok = tl && tl->forms.size() == cl->forms.size();
            for (std::size_t k = 1; tok && k < cl->forms.size(); ++k)
                tok = cl->forms[k] && tl->forms[k] && tl->forms[k]->As<RE::TESCombatStyle>();
            if (!tok) {
                spdlog::error("[classes] package styles: class {}'s zero-avoid twin list does not match its style list -- no twins "
                              "for it", kTable[i].name);
                continue;
            }
            for (std::size_t k = 1; k < cl->forms.size(); ++k) g_noDodge[cl->forms[k]->GetFormID()] = tl->forms[k]->GetFormID();
        }
        if (!hasTwins)
            spdlog::warn("[classes] package styles: the class ESL has no zero-avoid twin lists -- heavy armor and Living Legend's "
                         "guard cannot remove Avoid Threat (regenerate MFO_Classes.esl)");
        spdlog::info("[classes] package styles resolved: {} of {} classes, {} CSTY record(s) (bClassStyles = {})", resolved,
                     kClassCount, styles, Config::g_classStyles.load() ? 1 : 0);
    }

    std::uint8_t PackageClass(RE::FormID a_follower) {
        if (!Active()) return kNoClass;
        std::uint8_t cls = GetEquipped(a_follower);
        if (cls == kNoClass || cls >= kClassCount) return kNoClass;
        if (static_cast<Id>(cls) == Id::Dragonborn) {
            // marth Q60: his combat behaviour is the player-picked T4; none picked = Dragonborn's own package.
            if (const std::uint8_t t4 = GetPickedT4(a_follower); t4 != kNoClass && t4 < kClassCount) cls = t4;
        }
        return kTable[cls].tier >= 2 ? cls : kNoClass;
    }

    RE::TESCombatStyle* PackageStyle(Id a_class, std::uint8_t a_stance, bool a_dual) {
        const auto i = static_cast<std::size_t>(a_class);
        if (i >= kClassCount || kTable[i].tier < 2) return nullptr;
        const auto& cs = g_styles[i];
        switch (a_stance) {
            case 1:  return (a_dual && cs.meleeDual) ? cs.meleeDual : cs.melee;   // no dual twin: the single style (never grants dual)
            case 2:  return cs.ranged;
            case 3:  return cs.cast;
            default: return nullptr;
        }
    }

    RE::FormID NoDodgeTwin(RE::FormID a_classStyle) {
        const auto it = g_noDodge.find(a_classStyle);
        return it != g_noDodge.end() ? it->second : a_classStyle;
    }

    float PackageLeashScale(RE::FormID a_follower) {
        const std::uint8_t cls = PackageClass(a_follower);
        if (cls == kNoClass) return 1.0f;
        if (!(kTable[cls].flags & (Flag::kShortLeash | Flag::kNearPlayer))) return 1.0f;
        return Config::g_classLeashMult.load(std::memory_order_relaxed);
    }
}
