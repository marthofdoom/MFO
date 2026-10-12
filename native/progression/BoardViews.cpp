// progression/BoardViews.cpp -- COMPONENT 3, the board views: the published
// snapshot and its focus/refresh state, EnrollBlocker, BuildNodeViews and the
// public Copy/Publish/SetBoardFocus entry points.
// Split out of the old native/ProgAllocator.cpp by the wave-1 subsystem-folder
// split (2026-09-24): a pure move, proven function by function with tools/splitcheck.
// The allocator's contract and design references are at the top of
// progression/Allocator.cpp.
#include "PCH.h"
#include <algorithm>
#include <cctype>
#include <string_view>
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
#include "i18n/Strings.h"   // Pk_* keys: the owner-perspective perk text

namespace MFO::ProgAllocator {

        // ── board views (component 3 — see ProgAllocator.h) ─────────────────
        // Written on the MAIN thread only; the mutex guards the shared_ptr
        // swap so any thread may take a refcounted copy. The snap itself is
        // immutable once published — the Snapshot discipline, by pointer.
        std::mutex g_viewMx;
        std::shared_ptr<const BoardProgSnap> g_boardSnap;
        std::atomic<RE::FormID> g_boardFocus{ 0 };   // written by the render thread
        RE::FormID g_lastPublishedFocus = 0;         // main-thread-only
        bool g_boardWasOpen = false;                 // main-thread-only
        int  g_viewFrames   = 0;

    namespace {

        // Read-only mirror of Enroll()'s refusals, for the board to explain
        // an ineligible follower BEFORE the player tries (same checks, same
        // order — keep in sync with Enroll).
        const char* EnrollBlocker(RE::Actor* a_actor, RE::TESNPC* a_base) {
            if (!a_base) return "no actor base";
            // ASCII only: these strings render in the board UI and the baked
            // fonts carry the default (Basic Latin) glyph ranges.
            if (!Followers::IsPersistableID(a_actor->GetFormID()))
                return "temporary (runtime) actor -- cannot persist across saves";
            if (!a_base->IsUnique())
                return "shared template -- not eligible (v1 is unique-base only)";
            if (!Followers::IsEligibleFollower(a_actor))
                return "not an eligible follower";
            return nullptr;
        }

        // ── owner-perspective perk text (perk owner remap P1) ───────────────
        // A perk's owner is whoever holds it, but the game's text and records
        // were written for the player. Built on the MAIN thread at publish and
        // cached in the BoardNodeView; the render thread only reads it.
        // Layer 1 (every language) is the composed targets line; Layer 2
        // (phrase rewrite) runs only while the language's table says so
        // (Pk_RwOn) and never on a rank part of which is player-only (the
        // Guard: the text there is genuinely about the player).
        std::vector<std::string> SplitBar(std::string_view a_s) {
            std::vector<std::string> out;
            std::size_t i = 0;
            while (i <= a_s.size()) {
                const auto j = a_s.find('|', i);
                const auto e = (j == std::string_view::npos) ? a_s.size() : j;
                if (e > i) out.emplace_back(a_s.substr(i, e - i));
                if (j == std::string_view::npos) break;
                i = j + 1;
            }
            return out;
        }
        bool IsWordCh(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }
        char LowerCh(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        // Does a_from (already lowercase, ASCII compare) sit at a_text[a_i] on word boundaries?
        bool WordAt(const std::string& a_text, std::size_t a_i, const std::string& a_from) {
            if (a_from.empty() || a_i + a_from.size() > a_text.size()) return false;
            if (a_i > 0 && IsWordCh(a_text[a_i - 1])) return false;
            for (std::size_t k = 0; k < a_from.size(); ++k)
                if (LowerCh(a_text[a_i + k]) != a_from[k]) return false;
            const auto e = a_i + a_from.size();
            return !(e < a_text.size() && IsWordCh(a_text[e]));
        }
        std::string Lower(std::string a_s) {
            for (auto& c : a_s) c = LowerCh(c);
            return a_s;
        }
        // Replace whole-word, case-insensitive matches; a capitalised match gets a capitalised result.
        void ReplaceWords(std::string& a_text, const std::string& a_from, const std::string& a_to) {
            const std::string from = Lower(a_from);
            for (std::size_t i = 0; i < a_text.size();) {
                if (WordAt(a_text, i, from)) {
                    std::string to = a_to;
                    if (!to.empty() && std::isupper(static_cast<unsigned char>(a_text[i])))
                        to[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(to[0])));
                    a_text.replace(i, from.size(), to);
                    i += to.size();
                } else ++i;
            }
        }
        // "you" -> "they" before a verb, "them" otherwise (after a preposition or conjunction,
        // or at the end of a clause). Singular they keeps the verb form, so no morphology.
        void ReplaceYou(std::string& a_text) {
            const auto nonVerb = SplitBar(Str::Get(Str::K::Pk_RwYouNonVerb));
            const std::string subj = Str::Get(Str::K::Pk_RwYouSubj), obj = Str::Get(Str::K::Pk_RwYouObj);
            for (std::size_t i = 0; i < a_text.size();) {
                if (!WordAt(a_text, i, "you")) { ++i; continue; }
                // The previous word, for the preposition test: "to you", "for you".
                std::size_t pe = i;
                while (pe > 0 && a_text[pe - 1] == ' ') --pe;
                std::size_t ps = pe;
                while (ps > 0 && IsWordCh(a_text[ps - 1])) --ps;
                const std::string prev = Lower(a_text.substr(ps, pe - ps));
                bool after = std::find(nonVerb.begin(), nonVerb.end(), prev) != nonVerb.end();
                // Contractions: you're / you'll / you've / you'd -> they're ... (the subject form).
                if (i + 3 < a_text.size() && a_text[i + 3] == '\'') {
                    std::size_t se = i + 4;
                    while (se < a_text.size() && IsWordCh(a_text[se])) ++se;
                    const std::string suf = Lower(a_text.substr(i + 4, se - (i + 4)));
                    if (suf == "re" || suf == "ll" || suf == "ve" || suf == "d") {
                        std::string to = subj;
                        if (!to.empty() && std::isupper(static_cast<unsigned char>(a_text[i])))
                            to[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(to[0])));
                        a_text.replace(i, 3, to);
                        i += to.size();
                        continue;
                    }
                }
                // Sentence-initial "You ..." is a subject ("You and ..." included).
                bool sentenceStart = (pe == 0);
                if (!sentenceStart) {
                    const char pc = a_text[pe - 1];
                    sentenceStart = (pc == '.' || pc == '!' || pc == '?' || pc == ':' || pc == '\n');
                }
                // Followed by a lowercase word that is not itself a preposition/conjunction: a verb.
                std::size_t n = i + 3;
                while (n < a_text.size() && a_text[n] == ' ') ++n;
                std::size_t ne = n;
                while (ne < a_text.size() && IsWordCh(a_text[ne])) ++ne;
                const std::string next = Lower(a_text.substr(n, ne - n));
                const bool verbNext = n > i + 3 && ne > n &&
                                      std::islower(static_cast<unsigned char>(a_text[n])) &&
                                      std::find(nonVerb.begin(), nonVerb.end(), next) == nonVerb.end();
                const bool andNext = (next == "and");
                std::string to = (!after && (verbNext || andNext || sentenceStart)) ? subj : obj;
                if (!to.empty() && std::isupper(static_cast<unsigned char>(a_text[i])))
                    to[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(to[0])));
                a_text.replace(i, 3, to);
                i += to.size();
            }
        }
        void RewriteForOwner(std::string& a_text, const std::string& a_owner) {
            // (a) the misnomer becomes the truth: "the player" -> the owner's name.
            if (const std::string p = Str::Get(Str::K::Pk_RwPlayer); !p.empty() && !a_owner.empty())
                ReplaceWords(a_text, p, a_owner);
            // (b) "your followers" -> "allies (you included)".
            const std::string to = Str::Get(Str::K::Pk_RwAlliesTo);
            for (const auto& ph : SplitBar(Str::Get(Str::K::Pk_RwAllies)))
                ReplaceWords(a_text, ph, to);
            // (c) the second person becomes the third.
            for (const auto& pr : SplitBar(Str::Get(Str::K::Pk_RwPairs))) {
                const auto arrow = pr.find("=>");
                if (arrow == std::string::npos) continue;
                ReplaceWords(a_text, pr.substr(0, arrow), pr.substr(arrow + 2));
            }
            ReplaceYou(a_text);
        }

        // The description the board shows for a follower who OWNS this node's perk.
        // Uses only the catalog's precomputed fields (no perk walk here).
        std::string OwnerDescription(const Progression::PerkNodeView& a_node, const std::string& a_owner) {
            Progression::PerkOwnerFacts u;
            for (const auto& r : a_node.ranks) u.Merge(r.owner);
            const bool playerOnlyPart = u.remap != Progression::OwnerRemap::kNone;

            std::string text = a_node.description.empty() ? a_node.effectsText : a_node.description;

            // Layer 2: only where the ACTIVE language's own table supplies the patterns
            // (English always does; another language must ship Pk_RwOn itself, or a missing
            // key would run English patterns over foreign text). Never under the Guard.
            if (!playerOnlyPart && Str::LanguageSupplies(Str::K::Pk_RwOn) &&
                std::string_view(Str::Get(Str::K::Pk_RwOn)) == "1")
                RewriteForOwner(text, a_owner);

            // Layer 1: the targets line. The Party line is written only for a cloak that is
            // not a damage aura (its MGEFs not flagged hostile, or gated on IsHostileToActor
            // with a null parameter) and carries no teammate test (which excludes the player
            // itself). Any other cloak says nothing rather than something false.
            std::string line;
            if (u.cloak) {
                if (!u.teammateTest && !playerOnlyPart && (!u.cloakHostile || u.hostileNullParam))
                    line = Str::Fmt(Str::K::Pk_TargetsParty, { a_owner });
            } else if (a_node.hasAbility && !playerOnlyPart) {
                line = Str::Fmt(Str::K::Pk_TargetsSelf, { a_owner });
            }
            if (!line.empty()) {
                if (!text.empty()) text += '\n';
                text += line;
            }
            if (playerOnlyPart) {
                if (!text.empty()) text += '\n';
                text += Str::Get(Str::K::Pk_PlayerOnlyPart);
            }
            return text;
        }

        // Rewritten text cache, main thread only (BuildNodeViews runs on it). Keyed by the focused
        // follower + name and the language table (Str::Get returns a pointer into the published
        // table, which a Reload replaces); rebuilt when any of them changes.
        struct DescCache {
            RE::FormID               id{ 0 };
            std::string              name;
            const char*              stamp{ nullptr };
            std::vector<std::string> text;
            std::vector<char>        have;
        };
        DescCache g_descCache;

        // One BoardNodeView per catalog node, catalog order (skill-major) —
        // the render thread indexes by prefix sums off the same frozen
        // catalog. Cost is bounded by the publish cadence (kViewFrames), not
        // per frame: GateNextRank's condition evaluation only runs for nodes
        // that survive the cheaper gates (its points check sits before the
        // prereq walk and perkConditions.IsTrue).
        void BuildNodeViews(RE::Actor* a_actor, RE::TESNPC* a_base, ProgState* a_st,
                            std::vector<BoardNodeView>& a_out) {
            static const ProgState kNoState{};
            const auto& cat = Progression::Get();
            std::size_t total = 0;
            for (const auto& t : cat.skills) total += t.nodes.size();
            a_out.clear();
            a_out.reserve(total);
            const bool gateable = a_st && a_st->enrolled && FindClassDef(a_st->clsId) != nullptr;
            const std::string ownerName = (a_actor && a_actor->GetName()) ? a_actor->GetName() : "";
            {
                const RE::FormID id = a_actor ? a_actor->GetFormID() : 0;
                const char* stamp = Str::Get(Str::K::Pk_RwPlayer);
                if (g_descCache.id != id || g_descCache.name != ownerName ||
                    g_descCache.stamp != stamp || g_descCache.have.size() != total) {
                    g_descCache.id = id; g_descCache.name = ownerName; g_descCache.stamp = stamp;
                    g_descCache.text.assign(total, std::string());
                    g_descCache.have.assign(total, 0);
                }
            }
            std::size_t flat = 0;
            for (const auto& tree : cat.skills) {
                for (const auto& node : tree.nodes) {
                    BoardNodeView v;
                    if (!g_descCache.have[flat]) {
                        g_descCache.text[flat] = OwnerDescription(node, ownerName);
                        g_descCache.have[flat] = 1;
                    }
                    v.description = g_descCache.text[flat++];
                    if (a_st)
                        if (auto* al = FindAlloc(*a_st, node.perkFormID))
                            v.ownedRank = al->rank;
                    if (v.ownedRank == 0 &&
                        OwnsAnyRank(a_actor, a_base, a_st ? *a_st : kNoState, node.perkFormID))
                        v.native = true;   // load-order-granted — MFO never builds on it (§4.1)
                    if (gateable && !v.native &&
                        v.ownedRank < static_cast<std::uint8_t>(node.ranks.size())) {
                        std::string why;
                        if (GateNextRank(a_actor, a_base, *a_st, node, why) > 0)
                            v.available = true;
                        else
                            v.whyNot = std::move(why);
                    }
                    a_out.push_back(std::move(v));
                }
            }
        }

    }

    // ── board views (component 3) ───────────────────────────────────────────

    void SetBoardFocus(RE::FormID a_id) { g_boardFocus.store(a_id); }

    std::shared_ptr<const BoardProgSnap> CopyBoardViews() {
        std::scoped_lock lk(g_viewMx);
        return g_boardSnap;
    }

    // v1.1 Phase 6b: wrap the published payload into the GENERIC hosted-tab list.
    // One BoardTabView per add-on that declared a board tab (frozen Manifests()),
    // carrying the add-on's own label + the shared payload; active iff the payload
    // is published+ready. Delete the add-on → Manifests() empty → no hosted tab.
    std::vector<BoardTabView> CopyBoardTabViews() {
        std::vector<BoardTabView> tabs;
        auto snap = CopyBoardViews();
        for (const auto& m : Progression::Manifests()) {
            if (!m.boardTab.declared) continue;
            BoardTabView v;
            v.label   = m.boardTab.label;
            v.content = snap;
            v.active  = (snap && snap->active);
            tabs.push_back(std::move(v));
        }
        return tabs;
    }

    void PublishBoardViews() {
        auto snap = std::make_shared<BoardProgSnap>();
        snap->active = g_ready && Progression::Get().built;
        if (snap->active) {
            snap->respecRapportCost     = g_econ.respecRapportCost;
            snap->skillCap              = g_econ.skillCap;
            snap->levelsPerPerkPoint    = std::max(1, g_econ.levelsPerPerkPoint);
            snap->manualSkillPtsPerLevel = g_econ.manualSkillPtsPerLevel;
            // §18.6: the declared classes, in declaration order — the board's
            // dynamic-N class prompt draws exactly these.
            snap->classes.reserve(g_classes.size());
            for (const auto& def : g_classes)
                snap->classes.emplace_back(def.id, def.name);

            const RE::FormID focus = g_boardFocus.load();
            // Immutable snapshot, not live g_active: Refresh reassigns it on the
            // worker while this poll runs on the MainThread::Post chain (SEV-1).
            auto activeSnap = Followers::ActiveSnapshot();
            for (RE::FormID snapId : *activeSnap) {
                auto* a = RE::TESForm::LookupByID<RE::Actor>(snapId);
                if (!a) continue;
                BoardFollowerView v;
                v.id   = a->GetFormID();
                v.name = a->GetName() ? a->GetName() : "?";
                auto* base = a->GetActorBase();
                const char* blk = EnrollBlocker(a, base);
                v.eligible = (blk == nullptr);
                if (blk) v.blocker = blk;
                ProgState* st = nullptr;
                if (auto it = g_prog.find(v.id); it != g_prog.end() && it->second.enrolled)
                    st = &it->second;
                if (st) {
                    v.enrolled       = true;
                    v.clsId          = st->clsId;
                    if (const auto* def = FindClassDef(st->clsId)) v.clsName = def->name;
                    v.level          = st->progressionLevel;
                    v.unspentPerk    = static_cast<float>(PerkPointsAvailable(*st));   // §17 derived
                    v.nativeAtEnroll = st->nativeTreePerksAtEnroll;
                    v.allocatedRanks = static_cast<std::uint16_t>(
                        std::min(AllocatedRanks(*st), 0xFFFF));
                    v.manualSkills   = st->manualSkills;           // §16
                    v.manualAvail    = ManualAvail(*st);
                    v.freeRespec     = st->freeRespec;             // one-time free respec pending
                }
                if (auto* avo = a->AsActorValueOwner()) {
                    v.skills.reserve(std::size(kSkillNames));
                    for (const auto& s : kSkillNames) {
                        BoardSkillLine line;
                        line.name = s.name;
                        line.av   = s.av;
                        line.base = avo->GetBaseActorValue(s.av);
                        if (st)
                            for (const auto& e : st->skills)
                                if (e.av == s.av) {
                                    line.alloc  = e.points;
                                    line.manual = e.manualPoints;   // §16
                                    break;
                                }
                        v.skills.push_back(std::move(line));
                    }
                }
                // The full per-node gate walk only for the follower the tab
                // is actually LOOKING at — one tree per publish, not four.
                if (base && v.id == focus) {
                    snap->treeFor = focus;
                    BuildNodeViews(a, base, st, snap->nodes);
                }
                snap->rows.push_back(std::move(v));
            }
            g_lastPublishedFocus = focus;
        }
        std::scoped_lock lk(g_viewMx);
        g_boardSnap = std::move(snap);
    }

}
