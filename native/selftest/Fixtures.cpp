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
#include "Runtime.h"

namespace MFO::SelfTest {

    namespace {

        // The same relocation row Probe.cpp uses (Probe.StartCombat, ids 38561 / 37608); the project's seat self-check refuses a
        // build that does not match. a_foe starts a fight with a_target. MAIN THREAD.
        bool StartCombatOn(RE::Actor* a_foe, RE::Actor* a_target) {
            if (!Runtime::Known()) return false;
            using func_t = bool(RE::Actor*, RE::Actor*, void*);
            static REL::Relocation<func_t> func{ REL::RelocationID(37608, 38561) };
            static const bool verified = Runtime::SeatVerified(func.address(), "Probe.StartCombat");
            if (!verified) return false;
            return func(a_foe, a_target, nullptr);
        }

        // MAIN THREAD. Places one hostile in front of the follower at ~700 units, records it in the ledger (same closure) and sets it on
        // him. Returns false + a_note on failure; whatever landed is in the ledger.
        bool SpawnHostile(RE::Actor* a_follower, const SpawnSpec& a_spec, std::uint32_t a_token, const std::string& a_testId,
                          std::string& a_note) {
            const RE::FormID baseId = VanillaForm(a_spec.baseLocal);
            auto*            base   = baseId ? RE::TESForm::LookupByID<RE::TESBoundObject>(baseId) : nullptr;
            if (!base) { a_note = std::format("Skyrim.esm base {:06X} does not resolve", a_spec.baseLocal); return false; }
            auto ref = a_follower->PlaceObjectAtMe(base, false);
            auto* foe = ref ? ref->As<RE::Actor>() : nullptr;
            if (!foe) { a_note = "PlaceObjectAtMe returned no actor"; return false; }
            {
                std::lock_guard lk(g_mx);   // ledger first: a failure below still gets the actor deleted
                g_ledger.push_back({ a_token, a_testId, a_follower->GetFormID(), FixtureKind::Spawn, foe->GetFormID(), 1, 0 });
                g_spawnRef[a_token] = foe->GetFormID();
            }
            auto pos = a_follower->GetPosition();
            const float ang = a_follower->GetAngleZ();
            pos.x += std::sin(ang) * 700.0f;
            pos.y += std::cos(ang) * 700.0f;
            foe->SetPosition(pos, true);
            const bool engaged = StartCombatOn(foe, a_follower);
            a_note = std::format("spawned {:08X} '{}' (Skyrim.esm base {:06X}) ~700 units from the follower; StartCombat {}",
                                 foe->GetFormID(), foe->GetName() ? foe->GetName() : "?", a_spec.baseLocal,
                                 engaged ? "issued" : "NOT issued (runtime/seat refused: it fights only if it aggroes on its own)");
            return true;
        }

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
                g_acks[a_token] = { false, std::format("follower {:08X} did not resolve", a_follower), false };
                return;
            }
            if (req.spawn.on) {   // SAFETY FIRST: nothing is added when a hostile may not be spawned here (RETRY, not an error)
                if (const char* why = SpawnUnsafeReason(actor)) {
                    std::lock_guard lk(g_mx);
                    g_acks[a_token] = { false, std::string("refused, ") + why, true };
                    return;
                }
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
            if (failed.empty() && req.spawn.hurtPct > 0.0f) {
                if (auto* avo = actor->AsActorValueOwner()) {
                    const float dmg = std::max(0.0f, avo->GetActorValue(RE::ActorValue::kHealth) * req.spawn.hurtPct / 100.0f);
                    if (dmg > 0.0f) {
                        avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, -dmg);
                        rec.push_back({ a_token, testId, a_follower, FixtureKind::Hurt, 0, static_cast<std::int32_t>(dmg * 10.0f + 0.5f), 0 });
                        did.push_back(std::format("hurt Health by {:.0f} ({:.0f}%)", dmg, req.spawn.hurtPct));
                    }
                }
            }
            if (failed.empty() && req.spawn.on) {
                std::string note;
                if (!SpawnHostile(actor, req.spawn, a_token, testId, note)) failed = "spawn failed: " + note;
                else did.push_back(note);
            }
            {
                // Whatever DID land is in the ledger even when a later part failed, so the cleanup that
                // follows the aborted run removes it.
                std::lock_guard lk(g_mx);
                for (auto& f : rec) g_ledger.push_back(std::move(f));
                g_acks[a_token] = { failed.empty(), failed.empty() ? Join(did) : failed + "; added so far: " + Join(did), false };
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
                } else if (f.kind == FixtureKind::Hurt) {
                    auto* avo = actor->AsActorValueOwner();
                    const float want = f.added / 10.0f;
                    const float dmg  = std::max(0.0f, -actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth));
                    const float heal = std::min(want, dmg);
                    if (avo && heal > 0.0f) avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, heal);
                    spdlog::info("[selftest] fixture removed ({}): {:08X} Health damage the test dealt: gave back {:.0f} of {:.0f} (damage now {:.0f})",
                                 why, f.follower, heal, want, dmg - heal);
                    doneIdx.push_back(i);
                } else if (f.kind == FixtureKind::Spawn) {
                    auto* foe = RE::TESForm::LookupByID<RE::Actor>(f.form);
                    if (!foe) {
                        spdlog::info("[selftest] fixture removed ({}): spawned hostile {:08X} is already gone (cell unloaded / deleted)", why, f.form);
                    } else {
                        const std::string nm = foe->GetName() ? foe->GetName() : "?";
                        foe->StopCombat();
                        foe->Disable();
                        foe->SetDelete(true);
                        spdlog::info("[selftest] fixture removed ({}): spawned hostile {:08X} '{}' stopped, disabled and marked for deletion "
                                     "(it was {})", why, f.form, nm, foe->IsDead() ? "dead" : "alive");
                    }
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

    void PostRespawn(std::uint32_t a_token, const std::string& a_testId, RE::FormID a_follower, const SpawnSpec& a_spec) {
        MainThread::Post([a_token, testId = a_testId, a_follower, spec = a_spec]() {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_follower);
            if (!actor) return;
            if (const char* why = SpawnUnsafeReason(actor)) {
                spdlog::info("[selftest] respawn for {} refused: {}", testId, why);
                return;
            }
            std::string note;
            if (SpawnHostile(actor, spec, a_token, testId, note)) spdlog::info("[selftest] respawn for {}: {}", testId, note);
            else spdlog::error("[selftest] respawn for {} FAILED: {}", testId, note);
        });
    }

}
