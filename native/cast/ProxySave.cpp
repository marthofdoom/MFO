// cast/ProxySave.cpp -- SAVE / LOAD INTEGRITY of MFO's ConcProxy forms (fix/save-integrity-mfo,
// 2026-11-15, INVARIANTS #82; public contract in cast/Actuation.h). The pool itself stays in
// cast/Direct.cpp (ConcProxy, file-local); this TU reads it through ConcProxyPool().
//
// WHY. ConcProxy mints dynamic 0xFF SpellItems (cast/Direct.cpp, IFormFactory::Create) and
// force-casts them with CastSpellImmediate on the follower's kInstant caster, which starts a real
// concentration channel. While a stream runs, two things the engine SAVES name the proxy:
//   * the owner's MagicCaster::currentSpell (+0x28). MagicCaster::SaveGame (AE 0x5BE8F0 /
//     SE 0x54F2D0 / 1.7.104 0x5CD440) writes its form id; LoadGame resolves the id and casts it.
//   * the target's ActiveEffect::spell (+0x40). The effect-list save (AE 0x5AD5A0 / 0x5AD7E0)
//     writes the spell's form id, then the index of ActiveEffect::effect (+0x48) in that spell's
//     own effect array (11339).
// On load an 0xFF id resolves to whatever LIVE runtime form holds that id at that moment, and
// when that form is destroyed the slot points at freed memory: the 11-14 Harbinger crash
// (Save373, a CastProxy in a hand), here in a caster or an effect. (RE: the Harbinger Fable
// round, agentlogs/apmf-proxysave.md; the same shape is Harbinger's core/CastProxy.cpp.)
//
// WHEN. SKSE64 dispatches kSaveGame synchronously BEFORE the engine writes anything
// (Hooks_SaveLoad.cpp SaveGame_Hook: Dispatch_Message, then the original); the co-save save
// callback runs from the Papyrus global-data hook, AFTER every changeform is in the buffer. So the
// sweep is at kSaveGame, and the co-save callback can only CHECK (and undo the sweep's re-key).
//
// WHAT THE SWEEP DOES (main thread, inside the save call):
//   1. Every caster (the four magicCasters of every process actor and the player) whose
//      currentSpell is a pool form: MagicCaster::InterruptCast(false), the same call
//      TargetCastEndActor makes at a stream's end. Disassembly, all three builds:
//      InterruptCast (AE 34408 0x5BBFA0 / SE 33630 0x54CB70 / 1.7.104 0x5CAAE0) returns at once
//      when currentSpell is null, else it ends with a tail jump to AE 34435 0x5BEB60 /
//      SE 33657 0x54F4F0 / 1.7.104 0x5CD6B0, every path of which stores currentSpell = 0 and
//      state = 0 (AE 0x5BED48, SE 0x54F6CB, 1.7.104 0x5CD898). The read-back below proves it.
//      The stream itself is not ended: its next beat re-arms the effect (or re-casts), exactly
//      as after any interrupted channel; MFO bills per beat, not through the channel.
//   2. Every active effect of a pool form is re-keyed to the REAL spell that owns its Effect*
//      (the slot's sources this session, newest first). A proxy shares its source's Effect* in
//      the source's order, so the save writes a real form and the same effect index. Done AFTER
//      the interrupts, so an effect the engine removed on an interrupt is never touched.
//   3. Hands, selected-spell slots and the spell list are only CHECKED: MFO never teaches or
//      equips a proxy. A pool form found there is an ERROR (the save will hold it), not repaired.
// The co-save callback puts each re-keyed effect back on its proxy (found again by address on its
// target's own list, and only while it still names the source this sweep wrote). A save whose
// co-save callback never comes (an aborted save) is keyed back on the next pump.
//
// THE LOAD REPAIR. A save written before this fix can carry a proxy id in a caster or an effect.
// For every process actor and the player it tests each caster's currentSpell and each effect's
// spell WITHOUT trusting the pointer: a guarded 4-byte read of the id it would hold (+0x14), then
// "does the form map hold THIS pointer under that id". TESForm's destructor (AE 0x1E2250) calls
// 14627 (0x1E06A0), which erases the form's own map entry, so a destroyed form can never pass
// (Fable RE, apmf-proxysave.md). A dead caster spell is cleared with the engine's own reset
// stores (currentSpell = 0, state = kNone; the vcalls of 34435 are skipped because they read the
// spell). A dead effect spell is reported (ERROR): its Effect* died with that form, and no engine
// path is safe on it.
#include "Direct_internal.h"
#include "Runtime.h"      // Known(): the exact builds the member offsets below were read on
#include "MainThread.h"   // Post: the aborted-save key-back, the first-pump load repair

#include <unordered_set>

namespace MFO::Actuation {

    namespace {

        static_assert(offsetof(RE::TESForm, formID) == 0x14, "TESForm::formID moved -- the guarded id read uses +0x14");
        static_assert(offsetof(RE::MagicCaster, currentSpell) == 0x28 && offsetof(RE::MagicCaster, state) == 0x30,
                      "MagicCaster currentSpell / state moved -- InterruptCast's reset (AE 0x5BED48) stores +0x28 / +0x30");
        static_assert(offsetof(RE::ActiveEffect, spell) == 0x40 && offsetof(RE::ActiveEffect, effect) == 0x48,
                      "ActiveEffect spell / effect moved -- the effect-list save (AE 0x5AD5A0) reads +0x40 / +0x48");

        // An effect the sweep re-keyed from its proxy to the source spell for the length of the save.
        // `ae` is compared by address on the target's own list, never dereferenced on its own.
        struct Rekey {
            RE::FormID         target = 0;
            RE::ActiveEffect*  ae     = nullptr;
            RE::SpellItem*     proxy  = nullptr;
            RE::MagicItem*     source = nullptr;
        };
        std::vector<Rekey> g_rekeyed;        // MAIN THREAD
        bool               g_swept  = false; // kSaveGame ran since the last co-save callback
        std::uint32_t      g_gen    = 0;     // bumped per sweep and per revert: a stale posted key-back stops

        std::string Hex(RE::FormID a_id) { return fmt::format("{:08X}", a_id); }

        // Every actor with an AI process (the four process lists) plus the player, once each, held
        // for the length of the pass. MAIN THREAD.
        std::vector<RE::NiPointer<RE::Actor>> ProcessActors() {
            std::vector<RE::NiPointer<RE::Actor>> out;
            std::unordered_set<const RE::Actor*>  seen;
            auto add = [&](RE::NiPointer<RE::Actor> a_p) {
                if (a_p && seen.insert(a_p.get()).second) out.push_back(std::move(a_p));
            };
            if (auto* pl = RE::ProcessLists::GetSingleton()) {
                for (auto* arr : { &pl->highActorHandles, &pl->middleHighActorHandles, &pl->middleLowActorHandles,
                                   &pl->lowActorHandles })
                    for (auto& h : *arr) add(h.get());
            }
            if (auto* pc = RE::PlayerCharacter::GetSingleton()) add(RE::NiPointer<RE::Actor>(pc));
            return out;
        }

        const ConcProxyView* PoolOf(const std::vector<ConcProxyView>& a_pool, const RE::TESForm* a_f) {
            if (!a_f) return nullptr;
            for (const auto& v : a_pool)
                if (static_cast<const RE::TESForm*>(v.form) == a_f) return &v;
            return nullptr;
        }

        std::string PoolText(const std::vector<ConcProxyView>& a_pool) {
            std::string s;
            for (const auto& v : a_pool) s += fmt::format("{}{}", s.empty() ? "" : ", ", Hex(v.form->GetFormID()));
            return s;
        }

        // The real spell that owns a_effect: one of the sources this form carried this session,
        // newest first. nullptr = none of them.
        RE::MagicItem* SourceOf(const ConcProxyView& a_v, const RE::Effect* a_effect) {
            if (!a_effect) return nullptr;
            for (auto it = a_v.past.rbegin(); it != a_v.past.rend(); ++it) {
                auto* sp = *it ? RE::TESForm::LookupByID<RE::SpellItem>(*it) : nullptr;
                if (!sp) continue;
                for (auto* e : sp->effects)
                    if (e == a_effect) return sp;
            }
            return nullptr;
        }

        // Pool-form references a save would capture, other than active effects: casters (the
        // sweep's job) and the slots MFO never fills (hands, selected spells, spell list).
        struct Refs {
            std::uint32_t caster = 0, hand = 0, selected = 0, known = 0, effects = 0;
            std::uint32_t Slots() const { return caster + hand + selected + known; }
            std::string Text() const {
                std::string s;
                auto add = [&](std::uint32_t n, const char* what) {
                    if (n) s += fmt::format("{}{} {}", s.empty() ? "" : ", ", n, what);
                };
                add(caster, "caster current spell(s)");
                add(hand, "hand slot(s)");
                add(selected, "selected-spell slot(s)");
                add(known, "spell-list entr(y/ies)");
                add(effects, "active effect(s)");
                return s.empty() ? "nothing" : s;
            }
        };

        Refs CountRefs(RE::Actor* a_actor, const std::vector<ConcProxyView>& a_pool) {
            Refs  r;
            auto& rd = a_actor->GetActorRuntimeData();
            for (auto* mc : rd.magicCasters)
                if (mc && PoolOf(a_pool, mc->currentSpell)) ++r.caster;
            for (const bool left : { true, false })
                if (PoolOf(a_pool, a_actor->GetEquippedObject(left))) ++r.hand;
            for (auto* sel : rd.selectedSpells)
                if (PoolOf(a_pool, sel)) ++r.selected;
            for (auto* sp : rd.addedSpells)
                if (PoolOf(a_pool, sp)) ++r.known;
            if (auto* list = a_actor->AsMagicTarget()->GetActiveEffectList())
                for (auto* ae : *list)
                    if (ae && PoolOf(a_pool, ae->spell)) ++r.effects;
            return r;
        }

        // Key every still-recorded effect back to its proxy. Each is found again on its target's
        // own list by address and must still name the source the sweep wrote. MAIN THREAD.
        void KeyBack(const char* a_when) {
            if (g_rekeyed.empty()) return;
            const auto    pool = ConcProxyPool();
            std::uint32_t back = 0, gone = 0;
            for (const auto& r : g_rekeyed) {
                bool done = false;
                if (auto* t = RE::TESForm::LookupByID<RE::Actor>(r.target); t && PoolOf(pool, r.proxy)) {
                    if (auto* list = t->AsMagicTarget()->GetActiveEffectList()) {
                        for (auto* ae : *list) {
                            if (ae != r.ae) continue;
                            if (ae->spell == r.source) {
                                ae->spell = r.proxy;
                                done      = true;
                            }
                            break;
                        }
                    }
                }
                done ? ++back : ++gone;
            }
            g_rekeyed.clear();
            spdlog::info("[proxysave] {}: {} active effect(s) keyed back to their proxy{}.", a_when, back,
                         gone ? fmt::format(" ({} no longer on their target or no longer on the source: left as they are)", gone)
                              : std::string());
        }

        // ---- load repair ----
        enum class Life : std::uint8_t { kLive, kLiveRuntime, kDead, kUnverifiable };

        // The 4 bytes where a form keeps its id, read through a pointer that may be dead. Freed
        // memory can be unmapped, so the read is guarded. (No C++ object with a destructor lives in
        // this function, which __try requires.)
        bool ReadFormIdGuarded(const void* a_p, RE::FormID& a_out) noexcept {
            __try {
                a_out = *reinterpret_cast<const volatile RE::FormID*>(reinterpret_cast<const std::uint8_t*>(a_p) + 0x14);
                return true;
            } __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
                a_out = 0;
                return false;
            }
        }

        // kUnverifiable: the memory reads id 0, which the form map never holds; a form outside the
        // map cannot be told from garbage, so it is left alone and said.
        Life Classify(const RE::TESForm* a_p, RE::FormID& a_id) {
            if (!ReadFormIdGuarded(a_p, a_id)) return Life::kDead;
            if (a_id == 0) return Life::kUnverifiable;
            if (RE::TESForm::LookupByID(a_id) != a_p) return Life::kDead;
            return a_id >= 0xFF000000 ? Life::kLiveRuntime : Life::kLive;
        }

    }

    void ResetProxySaveState() {
        g_rekeyed.clear();
        g_swept = false;
        ++g_gen;
    }

    void ConcProxyPreSaveSweep() {
        if (!g_rekeyed.empty()) {
            // An earlier sweep whose co-save callback never came and whose posted key-back has not
            // run yet: key those back first, so this sweep starts from the live state.
            KeyBack("kSaveGame (a previous save's re-keys)");
        }
        g_swept = true;
        const auto gen = ++g_gen;
        const auto pool = ConcProxyPool();
        if (pool.empty()) return;   // nothing minted this session: nothing a save could capture
        if (!Runtime::Known()) {
            spdlog::error("[proxysave] kSaveGame: {} ConcProxy form(s) [{}] are minted, but the sweep is REFUSED on this "
                          "runtime (member offsets proven on 1.6.1170 / 1.5.97 / 1.7.104 only) -- THE SAVE MAY HOLD A "
                          "RUNTIME FORM.",
                          pool.size(), PoolText(pool));
            return;
        }
        std::uint32_t actors = 0, interrupted = 0, leftCasters = 0, rekeyed = 0, unresolved = 0, foreign = 0;
        for (auto& ap : ProcessActors()) {
            auto* actor = ap.get();
            ++actors;
            const char* name = actor->GetName();
            // 1. Casters channeling a pool form.
            auto& rd = actor->GetActorRuntimeData();
            for (std::size_t i = 0; i < std::size(rd.magicCasters); ++i) {
                auto* mc = rd.magicCasters[i];
                if (!mc) continue;
                const auto* v = PoolOf(pool, mc->currentSpell);
                if (!v) continue;
                const auto form = v->form->GetFormID();
                const auto st   = static_cast<int>(mc->state.get());
                mc->InterruptCast(false);
                if (mc->currentSpell == nullptr) {
                    ++interrupted;
                    spdlog::info("[proxysave] kSaveGame: {} \"{}\" caster {} was channeling ConcProxy {} (state {}) -- "
                                 "interrupted, its current spell is clear for the save.",
                                 Hex(actor->GetFormID()), name ? name : "", i, Hex(form), st);
                } else {
                    ++leftCasters;
                    spdlog::error("[proxysave] kSaveGame: {} \"{}\" caster {} STILL holds {} after InterruptCast (now "
                                  "{}) -- THE SAVE WILL HOLD A RUNTIME FORM in that caster.",
                                  Hex(actor->GetFormID()), name ? name : "", i, Hex(form),
                                  mc->currentSpell ? Hex(mc->currentSpell->GetFormID()) : std::string("null"));
                }
            }
            // 2. Active effects of a pool form: key them to their real source spell for the save.
            if (auto* list = actor->AsMagicTarget()->GetActiveEffectList()) {
                for (auto* ae : *list) {
                    if (!ae) continue;
                    const auto* v = PoolOf(pool, ae->spell);
                    if (!v) continue;
                    auto* src = SourceOf(*v, ae->effect);
                    if (!src) {
                        ++unresolved;
                        spdlog::error("[proxysave] kSaveGame: {} \"{}\" carries an active effect of ConcProxy {} whose "
                                      "Effect belongs to none of the spells that form carried this session -- NOT "
                                      "re-keyed: THE SAVE WILL HOLD A RUNTIME FORM in that effect.",
                                      Hex(actor->GetFormID()), name ? name : "", Hex(v->form->GetFormID()));
                        continue;
                    }
                    g_rekeyed.push_back({ actor->GetFormID(), ae, v->form, src });
                    ae->spell = src;
                    ++rekeyed;
                    spdlog::info("[proxysave] kSaveGame: {} \"{}\" active effect of ConcProxy {} keyed to its source "
                                 "spell {} for the save.",
                                 Hex(actor->GetFormID()), name ? name : "", Hex(v->form->GetFormID()), Hex(src->GetFormID()));
                }
            }
            // 3. Slots MFO never fills: check only.
            if (const Refs r = CountRefs(actor, pool); r.hand + r.selected + r.known != 0) {
                foreign += r.hand + r.selected + r.known;
                spdlog::error("[proxysave] kSaveGame: {} \"{}\" holds a ConcProxy form in {} -- MFO never puts one "
                              "there and does not clear it: THE SAVE WILL HOLD A RUNTIME FORM.",
                              Hex(actor->GetFormID()), name ? name : "", r.Text());
            }
        }
        spdlog::info("[proxysave] kSaveGame sweep (before the engine writes): {} actor(s) checked against {} ConcProxy "
                     "form(s) [{}] -- {} caster(s) interrupted, {} active effect(s) keyed to their source for the save; "
                     "{} caster(s), {} effect(s) and {} other slot(s) could NOT be cleared.",
                     actors, pool.size(), PoolText(pool), interrupted, rekeyed, leftCasters, unresolved, foreign);
        if (!g_rekeyed.empty()) {
            // The co-save callback keys them back inside this save call. If it never comes (the save
            // did not complete), this does it on the next pump; it finds nothing when it did.
            MainThread::Post([gen] {
                if (gen != g_gen) return;   // a later sweep or a revert owns the state now
                KeyBack("next pump after a save whose co-save callback did not run");
            });
        }
    }

    void ConcProxySaveWrittenCheck() {
        const bool swept = g_swept;
        g_swept          = false;
        const auto pool  = ConcProxyPool();
        if (pool.empty()) {
            g_rekeyed.clear();   // a re-key needs a minted form
            return;
        }
        if (!Runtime::Known()) return;   // refused at kSaveGame, said there
        if (!swept)
            spdlog::warn("[proxysave] co-save callback ran with NO kSaveGame message before it -- the ConcProxy sweep did "
                         "not run for this save, and the records are already written.");
        Refs        total;
        std::string who;
        std::uint32_t actors = 0;
        for (auto& ap : ProcessActors()) {
            auto* actor = ap.get();
            ++actors;
            const Refs r = CountRefs(actor, pool);
            if (r.Slots() + r.effects == 0) continue;
            // An effect the sweep re-keyed names its source now, so it is not counted here.
            total.caster += r.caster;
            total.hand += r.hand;
            total.selected += r.selected;
            total.known += r.known;
            total.effects += r.effects;
            who += fmt::format("{}{} ({})", who.empty() ? "" : "; ", Hex(actor->GetFormID()), r.Text());
        }
        if (total.Slots() + total.effects != 0)
            spdlog::warn("[proxysave] co-save check (after the records): {} still named a ConcProxy form [{}] when the "
                         "engine wrote the actors: {} -- THE .ESS MAY HOLD A RUNTIME FORM (INVARIANTS #82).",
                         total.Text(), PoolText(pool), who);
        else
            spdlog::info("[proxysave] co-save check (after the records): {} actor(s), no caster, slot or active effect "
                         "named any of the {} ConcProxy form(s) [{}] -- CLEAN.",
                         actors, pool.size(), PoolText(pool));
        KeyBack("co-save callback");
    }

    void ConcProxyLoadRepair(const char* a_when) {
        if (!Runtime::Known()) {
            static bool s_said = false;   // main thread
            if (!s_said) {
                s_said = true;
                spdlog::warn("[proxysave] load repair REFUSED on this runtime: the caster / effect member offsets are "
                             "proven on 1.6.1170, 1.5.97 and 1.7.104 only.");
            }
            return;
        }
        std::unordered_map<const RE::TESForm*, Life> memo;
        auto classify = [&](const RE::TESForm* a_p, RE::FormID& a_id) {
            if (auto it = memo.find(a_p); it != memo.end() && it->second == Life::kLive) return Life::kLive;
            const Life l = Classify(a_p, a_id);
            memo[a_p]    = l;
            return l;
        };
        std::uint32_t actors = 0, cleared = 0, deadEffects = 0, liveRuntime = 0, unverifiable = 0;
        for (auto& ap : ProcessActors()) {
            auto* actor = ap.get();
            ++actors;
            const char* name = actor->GetName();
            auto&       rd   = actor->GetActorRuntimeData();
            for (std::size_t i = 0; i < std::size(rd.magicCasters); ++i) {
                auto* mc = rd.magicCasters[i];
                if (!mc || !mc->currentSpell) continue;
                const auto* p  = static_cast<const RE::TESForm*>(mc->currentSpell);
                RE::FormID  id = 0;
                switch (classify(p, id)) {
                case Life::kDead:
                    // The engine's own reset stores (34435's tail), without its vcalls: they read the spell.
                    mc->currentSpell = nullptr;
                    mc->state        = RE::MagicCaster::State::kNone;
                    ++cleared;
                    spdlog::warn("[proxysave] LOAD REPAIR ({}): {} \"{}\" caster {} held a pointer to a DESTROYED form "
                                 "({}, its memory reads id {}) -- current spell cleared, caster state reset. (A save "
                                 "written with a runtime form in that caster, INVARIANTS #82; its next reader would "
                                 "have crashed.)",
                                 a_when, Hex(actor->GetFormID()), name ? name : "", i, fmt::ptr(p), Hex(id));
                    break;
                case Life::kLiveRuntime: ++liveRuntime; break;
                case Life::kUnverifiable: ++unverifiable; break;
                default: break;
                }
            }
            if (auto* list = actor->AsMagicTarget()->GetActiveEffectList()) {
                std::uint32_t dead = 0;
                for (auto* ae : *list) {
                    if (!ae || !ae->spell) continue;
                    RE::FormID id = 0;
                    switch (classify(static_cast<const RE::TESForm*>(ae->spell), id)) {
                    case Life::kDead: ++dead; break;
                    case Life::kLiveRuntime: ++liveRuntime; break;
                    case Life::kUnverifiable: ++unverifiable; break;
                    default: break;
                    }
                }
                if (dead) {
                    deadEffects += dead;
                    spdlog::error("[proxysave] load check ({}): {} \"{}\" carries {} active effect(s) whose spell pointer "
                                  "is a DESTROYED form -- NOT repaired (the effect's Effect* died with that form; no "
                                  "engine path is safe on it). The game may crash when that effect updates.",
                                  a_when, Hex(actor->GetFormID()), name ? name : "", dead);
                }
            }
        }
        spdlog::info("[proxysave] load repair ({}): {} actor(s) checked -- {} caster(s) holding a destroyed form cleared, "
                     "{} active effect(s) with a destroyed spell reported; {} live runtime (0xFF) spell(s) in a caster or "
                     "effect (not MFO's: its pool is empty after a load; left alone), {} unverifiable.",
                     a_when, actors, cleared, deadEffects, liveRuntime, unverifiable);
    }

}
