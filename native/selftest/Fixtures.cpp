// THE FIXTURE LEDGER (dev builds only). marth 2026-11-09: "If the follower doesnt have the equipment
// the gambit spawns it for the test then remove only what it ADDED".
//
// RULES (Docs/SELFTEST.md "Fixtures"):
//   * ADD only what is missing: an item is topped up to the test's `atLeast` (the follower's own copies
//     count toward it), a spell is taught only if the follower does not know it.
//   * RECORD exactly what was added (form, count added, the follower's own count before) in g_ledger,
//     in the SAME main-thread closure as the engine call, so a save (main thread too) can never see an
//     add without its ledger entry or a ledger entry without its add.
//   * REMOVE only what was added and never the follower's own: an item comes off as
//     min(added, now - before), so the count never drops below what he had before the test (whatever
//     the test consumed is gone already and is not taken out of his own stock). A taught spell is
//     deselected from a hand that holds it, then removed.
//   * The runner never equips anything itself; the gambits under test do, through MFO's own roads,
//     and those roads own their release. The one equip state the runner can leave behind is a taught
//     spell still selected in a hand, which the cleanup deselects (Actor::DeselectSpell, the call
//     Papyrus UnequipSpell makes; Loadout.cpp uses it for the same reason).
//   * The ledger is in the 'MFST' co-save, so a save taken mid-test carries it; the first tick after
//     the load removes those orphans (Runner.cpp). A crash never saved anything newer than the last
//     save, whose ledger matches its world.

#include "selftest/SelfTest_internal.h"
#include "MainThread.h"

namespace MFO::SelfTest {

    namespace {

        std::string Join(const std::vector<std::string>& a_parts) {
            std::string out;
            for (const auto& p : a_parts) out += (out.empty() ? "" : ", ") + p;
            return out.empty() ? std::string("nothing (the follower had everything)") : out;
        }

    }

    void PostFixtureAdd(std::uint32_t a_token, const std::string& a_testId, RE::FormID a_follower,
                        FixtureRequest a_req) {
        MainThread::Post([a_token, testId = a_testId, a_follower, req = std::move(a_req)]() {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_follower);
            if (!actor) {
                std::lock_guard lk(g_mx);
                g_acks[a_token] = { false, std::format("follower {:08X} did not resolve", a_follower) };
                return;
            }
            std::vector<std::string> did;
            std::vector<Fixture>     rec;
            std::string              failed;
            for (const auto& it : req.items) {
                auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(it.form);
                if (!obj) { failed = std::format("item {:08X} does not resolve", it.form); break; }
                const std::int32_t cur = CountItem(actor, it.form);
                if (cur >= it.atLeast) continue;
                const std::int32_t want = it.atLeast - cur;
                actor->AddObjectToContainer(obj, nullptr, want, nullptr);
                const std::int32_t after = CountItem(actor, it.form);
                const std::int32_t got   = after - cur;
                if (got > 0) {
                    rec.push_back({ a_token, testId, a_follower, FixtureKind::Item, it.form, got, cur });
                    did.push_back(std::format("{} x{} (had {})", FormName(it.form), got, cur));
                }
                if (got != want) {   // never mask: the add did not land as asked
                    failed = std::format("added {} of {} '{}' (count {} -> {})", got, want, FormName(it.form),
                                         cur, after);
                    break;
                }
            }
            if (failed.empty()) {
                for (const auto sid : req.spells) {
                    auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(sid);
                    if (!sp) { failed = std::format("spell {:08X} does not resolve", sid); break; }
                    if (actor->HasSpell(sp)) continue;
                    actor->AddSpell(sp);
                    if (!actor->HasSpell(sp)) {
                        failed = std::format("AddSpell '{}' did not take", FormName(sid));
                        break;
                    }
                    rec.push_back({ a_token, testId, a_follower, FixtureKind::Spell, sid, 1, 0 });
                    did.push_back(std::format("taught '{}'", FormName(sid)));
                }
            }
            {
                // Whatever DID land is in the ledger even when a later part failed, so the cleanup that
                // follows the aborted run removes it.
                std::lock_guard lk(g_mx);
                for (auto& f : rec) g_ledger.push_back(std::move(f));
                g_acks[a_token] = { failed.empty(), failed.empty() ? Join(did) : failed + "; added so far: " + Join(did) };
            }
        });
    }

    void PostFixtureCleanup(std::uint32_t a_token, const char* a_why) {
        MainThread::Post([a_token, why = std::string(a_why ? a_why : "")]() {
            std::vector<Fixture> mine;
            {
                std::lock_guard lk(g_mx);
                for (const auto& f : g_ledger)
                    if (f.token == a_token) mine.push_back(f);
                if (a_token == 0) g_orphanCleanupInFlight = false;
            }
            if (mine.empty()) return;

            std::vector<std::size_t> doneIdx;   // indices into `mine` that are settled (removed or moot)
            for (std::size_t i = 0; i < mine.size(); ++i) {
                const auto& f     = mine[i];
                auto*       actor = RE::TESForm::LookupByID<RE::Actor>(f.follower);
                if (!actor) {
                    spdlog::error("[selftest] fixture cleanup ({}): follower {:08X} does not resolve -- '{}' "
                                  "NOT removed yet, kept in the ledger for the next try",
                                  why, f.follower, FormName(f.form));
                    continue;
                }
                if (f.kind == FixtureKind::Item) {
                    auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(f.form);
                    if (!obj) {
                        spdlog::error("[selftest] fixture cleanup ({}): item {:08X} no longer resolves -- "
                                      "dropped from the ledger, nothing to remove", why, f.form);
                        doneIdx.push_back(i);
                        continue;
                    }
                    const std::int32_t cur    = CountItem(actor, f.form);
                    const std::int32_t remove = std::min(f.added, std::max(0, cur - f.before));
                    if (remove > 0)
                        actor->RemoveItem(obj, remove, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                    const std::int32_t after = CountItem(actor, f.form);
                    spdlog::info("[selftest] fixture removed ({}): {:08X} '{}' x{} of {} added (count {} -> {}, "
                                 "his own before the test {}){}",
                                 why, f.follower, FormName(f.form), remove, f.added, cur, after, f.before,
                                 remove < f.added ? std::format(" -- {} used up during the test", f.added - remove)
                                                  : std::string{});
                    if (after != cur - remove)
                        spdlog::error("[selftest] fixture cleanup ({}): '{}' count is {} after removing {} from {}",
                                      why, FormName(f.form), after, remove, cur);
                    doneIdx.push_back(i);
                } else {
                    auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(f.form);
                    if (!sp) { doneIdx.push_back(i); continue; }
                    for (const bool left : { true, false })
                        if (actor->GetEquippedObject(left) == sp) actor->DeselectSpell(sp);
                    actor->RemoveSpell(sp);
                    const bool still = actor->HasSpell(sp);
                    if (still)
                        spdlog::error("[selftest] fixture cleanup ({}): RemoveSpell '{}' on {:08X} did not take",
                                      why, FormName(f.form), f.follower);
                    else
                        spdlog::info("[selftest] fixture removed ({}): {:08X} unlearned '{}' (taught for {})",
                                     why, f.follower, FormName(f.form), f.testId);
                    doneIdx.push_back(i);
                }
            }

            std::lock_guard lk(g_mx);
            for (const auto i : doneIdx) {
                const auto& d = mine[i];
                for (auto it = g_ledger.begin(); it != g_ledger.end(); ++it) {
                    if (it->token == d.token && it->follower == d.follower && it->kind == d.kind &&
                        it->form == d.form && it->testId == d.testId) {
                        g_ledger.erase(it);
                        break;
                    }
                }
            }
        });
    }

}
