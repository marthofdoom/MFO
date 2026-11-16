#include "PCH.h"
#include "EngineOps.h"
#include "MainThread.h"
#include "cast/Actuation.h"   // CannotActReason / NoteRefusedApply (the drink's act gate, re-read on the main thread)

// EngineOps.h has the contract. This file: one in-place body per engine write, and the hop.
namespace MFO::EngineOps {

    namespace {
        std::atomic<std::uint64_t> g_inPlace{ 0 }, g_posted{ 0 }, g_skipped{ 0 };

        // A posted call that could not run. MAIN THREAD ONLY (only the posted bodies call it), so the map needs no lock.
        // Said once per (actor, form, what) per 10 s: a rule that re-fires every lap must not flood the log.
        void Skipped(RE::FormID a_actor, RE::FormID a_form, const char* a_what, const char* a_why) {
            g_skipped.fetch_add(1, std::memory_order_relaxed);
            using Clock = std::chrono::steady_clock;
            static std::unordered_map<std::uint64_t, Clock::time_point> s_said;
            const std::uint64_t key = ((static_cast<std::uint64_t>(a_actor) << 32) | a_form) ^ std::hash<std::string_view>{}(a_what);
            const auto now = Clock::now();
            if (s_said.size() > 512) s_said.clear();
            auto& at = s_said[key];
            if (at.time_since_epoch().count() != 0 && now - at < std::chrono::seconds(10)) return;
            at = now;
            spdlog::warn("[engine-op] {:08X} {} {:08X}: asked for on the worker, NOT run on the main thread: {} (nothing was done; said "
                         "once per 10 s)", a_actor, a_what, a_form, a_why);
        }

        // In place when this thread may write engine state, else posted. a_here runs with the caller's own pointers (the call
        // it replaces, unchanged); a_posted runs on the main thread a frame later and re-resolves everything from ids.
        template <class Here, class Posted>
        void Run(Here&& a_here, Posted&& a_posted) {
            if (MainThread::EngineWritesAllowed()) {
                g_inPlace.fetch_add(1, std::memory_order_relaxed);
                a_here();
                return;
            }
            g_posted.fetch_add(1, std::memory_order_relaxed);
            MainThread::Post(std::forward<Posted>(a_posted));
        }

        const RE::BGSEquipSlot* SlotOf(RE::FormID a_id) {
            return a_id ? RE::TESForm::LookupByID<RE::BGSEquipSlot>(a_id) : nullptr;
        }

        void EquipNow(RE::Actor* a_actor, RE::TESBoundObject* a_object, std::uint32_t a_count, const RE::BGSEquipSlot* a_slot,
                      bool a_queue, bool a_force, bool a_sounds, bool a_applyNow) {
            auto* mgr = RE::ActorEquipManager::GetSingleton();
            if (!mgr) return;
            MainThread::NoteEngineWrite("EngineOps::Equip");
            mgr->EquipObject(a_actor, a_object, nullptr, a_count, a_slot, a_queue, a_force, a_sounds, a_applyNow);
        }
        void UnequipNow(RE::Actor* a_actor, RE::TESBoundObject* a_object, std::uint32_t a_count, const RE::BGSEquipSlot* a_slot,
                        bool a_queue, bool a_force, bool a_sounds, bool a_applyNow) {
            auto* mgr = RE::ActorEquipManager::GetSingleton();
            if (!mgr) return;
            MainThread::NoteEngineWrite("EngineOps::Unequip");
            mgr->UnequipObject(a_actor, a_object, nullptr, a_count, a_slot, a_queue, a_force, a_sounds, a_applyNow);
        }
    }

    Stats GetStats() {
        return { g_inPlace.load(std::memory_order_relaxed), g_posted.load(std::memory_order_relaxed), g_skipped.load(std::memory_order_relaxed) };
    }

    std::int32_t LiveCountOnMain(RE::TESObjectREFR* a_ref, RE::TESBoundObject* a_object) {
        if (!a_ref || !a_object) return 0;
        MainThread::NoteLiveRead(__FUNCTION__);   // INVARIANTS #84: a live inventory read names itself
        const auto inv = a_ref->GetInventory([a_object](RE::TESBoundObject& a_x) { return &a_x == a_object; });
        const auto it = inv.find(a_object);
        return it != inv.end() ? it->second.first : 0;
    }

    void Equip(RE::Actor* a_actor, RE::TESBoundObject* a_object, std::uint32_t a_count, const RE::BGSEquipSlot* a_slot,
               bool a_queueEquip, bool a_forceEquip, bool a_playSounds, bool a_applyNow) {
        if (!a_actor || !a_object) return;
        const RE::FormID aid = a_actor->GetFormID(), oid = a_object->GetFormID(), sid = a_slot ? a_slot->GetFormID() : 0;
        Run([&] { EquipNow(a_actor, a_object, a_count, a_slot, a_queueEquip, a_forceEquip, a_playSounds, a_applyNow); },
            [=] {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(aid);
                auto* obj   = RE::TESForm::LookupByID<RE::TESBoundObject>(oid);
                if (!actor || !obj) { Skipped(aid, oid, "equip", "the actor or the item no longer resolves"); return; }
                if (LiveCountOnMain(actor, obj) <= 0) { Skipped(aid, oid, "equip", "he no longer holds the item"); return; }
                EquipNow(actor, obj, a_count, SlotOf(sid), a_queueEquip, a_forceEquip, a_playSounds, a_applyNow);
            });
    }

    void Unequip(RE::Actor* a_actor, RE::TESBoundObject* a_object, std::uint32_t a_count, const RE::BGSEquipSlot* a_slot,
                 bool a_queueEquip, bool a_forceEquip, bool a_playSounds, bool a_applyNow) {
        if (!a_actor || !a_object) return;
        const RE::FormID aid = a_actor->GetFormID(), oid = a_object->GetFormID(), sid = a_slot ? a_slot->GetFormID() : 0;
        Run([&] { UnequipNow(a_actor, a_object, a_count, a_slot, a_queueEquip, a_forceEquip, a_playSounds, a_applyNow); },
            [=] {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(aid);
                auto* obj   = RE::TESForm::LookupByID<RE::TESBoundObject>(oid);
                if (!actor || !obj) { Skipped(aid, oid, "unequip", "the actor or the item no longer resolves"); return; }
                UnequipNow(actor, obj, a_count, SlotOf(sid), a_queueEquip, a_forceEquip, a_playSounds, a_applyNow);
            });
    }

    void EquipSpell(RE::Actor* a_actor, RE::SpellItem* a_spell, const RE::BGSEquipSlot* a_slot) {
        if (!a_actor || !a_spell) return;
        const auto now = [](RE::Actor* a_a, RE::SpellItem* a_s, const RE::BGSEquipSlot* a_sl) {
            auto* mgr = RE::ActorEquipManager::GetSingleton();
            if (!mgr) return;
            MainThread::NoteEngineWrite("EngineOps::EquipSpell");
            mgr->EquipSpell(a_a, a_s, a_sl);
        };
        const RE::FormID aid = a_actor->GetFormID(), spid = a_spell->GetFormID(), sid = a_slot ? a_slot->GetFormID() : 0;
        Run([&] { now(a_actor, a_spell, a_slot); },
            [=] {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(aid);
                auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(spid);
                if (!actor || !spell) { Skipped(aid, spid, "equip spell", "the actor or the spell no longer resolves"); return; }
                now(actor, spell, SlotOf(sid));
            });
    }

    void DeselectSpell(RE::Actor* a_actor, RE::SpellItem* a_spell) {
        if (!a_actor || !a_spell) return;
        const auto now = [](RE::Actor* a_a, RE::SpellItem* a_s) {
            MainThread::NoteEngineWrite("EngineOps::DeselectSpell");
            a_a->DeselectSpell(a_s);
        };
        const RE::FormID aid = a_actor->GetFormID(), spid = a_spell->GetFormID();
        Run([&] { now(a_actor, a_spell); },
            [=] {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(aid);
                auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(spid);
                if (!actor || !spell) { Skipped(aid, spid, "deselect spell", "the actor or the spell no longer resolves"); return; }
                now(actor, spell);
            });
    }

    void DrawHands(RE::Actor* a_actor, bool a_draw) {
        if (!a_actor) return;
        const auto now = [](RE::Actor* a_a, bool a_d) {
            MainThread::NoteEngineWrite("EngineOps::DrawHands");
            a_a->DrawWeaponMagicHands(a_d);
        };
        const RE::FormID aid = a_actor->GetFormID();
        Run([&] { now(a_actor, a_draw); },
            [=] {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(aid);
                if (!actor) { Skipped(aid, 0, "draw hands", "the actor no longer resolves"); return; }
                now(actor, a_draw);
            });
    }

    void EvaluatePackage(RE::Actor* a_actor) {
        if (!a_actor) return;
        const auto now = [](RE::Actor* a_a) {
            MainThread::NoteEngineWrite("EngineOps::EvaluatePackage");
            a_a->EvaluatePackage(true, false);   // never resetAI
        };
        const RE::FormID aid = a_actor->GetFormID();
        Run([&] { now(a_actor); },
            [=] {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(aid);
                if (!actor) { Skipped(aid, 0, "evaluate package", "the actor no longer resolves"); return; }
                now(actor);
            });
    }

    void InterruptCast(RE::Actor* a_actor, RE::MagicSystem::CastingSource a_source, bool a_refund) {
        if (!a_actor) return;
        const auto now = [](RE::Actor* a_a, RE::MagicSystem::CastingSource a_src, bool a_r) {
            if (auto* mc = a_a->GetMagicCaster(a_src)) {
                MainThread::NoteEngineWrite("EngineOps::InterruptCast");
                mc->InterruptCast(a_r);
            }
        };
        const RE::FormID aid = a_actor->GetFormID();
        Run([&] { now(a_actor, a_source, a_refund); },
            [=] {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(aid);
                if (!actor) { Skipped(aid, 0, "interrupt cast", "the actor no longer resolves"); return; }
                now(actor, a_source, a_refund);
            });
    }

    void Drink(RE::Actor* a_actor, RE::AlchemyItem* a_potion, const char* a_road) {
        if (!a_actor || !a_potion) return;
        const auto now = [](RE::Actor* a_a, RE::AlchemyItem* a_p) {
            auto* mgr = RE::ActorEquipManager::GetSingleton();
            if (!mgr) return;
            MainThread::NoteEngineWrite("EngineOps::Drink");
            mgr->EquipObject(a_a, a_p, nullptr, 1);   // one unit; the engine removes it from the pack
        };
        const RE::FormID aid = a_actor->GetFormID(), pid = a_potion->GetFormID();
        Run([&] { now(a_actor, a_potion); },
            [=] {
                auto* actor  = RE::TESForm::LookupByID<RE::Actor>(aid);
                auto* potion = RE::TESForm::LookupByID<RE::AlchemyItem>(pid);
                if (!actor || !potion) { Skipped(aid, pid, a_road, "the actor or the potion no longer resolves"); return; }
                // The worker's gate is a frame old: can he still act, and is the potion still his?
                if (const char* why = Actuation::CannotActReason(actor)) {
                    Actuation::NoteRefusedApply(aid, "drink", why, 0, aid);
                    Skipped(aid, pid, a_road, why);
                    return;
                }
                if (LiveCountOnMain(actor, potion) <= 0) { Skipped(aid, pid, a_road, "he no longer holds the potion"); return; }
                now(actor, potion);
            });
    }
}
