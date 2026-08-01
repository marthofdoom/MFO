#include "PCH.h"
#include "Papyrus.h"

namespace MFO::Papyrus {

    namespace {

        std::atomic<std::uint32_t> g_dispatches{ 0 };
        std::atomic<std::uint32_t> g_failures{ 0 };

        RE::BSScript::Internal::VirtualMachine* VM() {
            return RE::BSScript::Internal::VirtualMachine::GetSingleton();
        }

        // A form's VM handle. The policy is what maps a TESForm to the identity
        // the VM knows it by; there is no shortcut that skips it.
        // Returns false when no VM identity exists. INVALID IS NOT ZERO --
        // GetHandleForObject returns the policy's own sentinel on failure, and
        // that sentinel is nonzero in Skyrim. Comparing against 0 lets a failed
        // handle through to dispatch and makes the failure counters lie about
        // where it died. po3's PapyrusExtender checks EmptyHandle(); so do we.
        bool HandleFor(RE::TESForm* a_form, RE::VMHandle& a_out) {
            auto* vm = VM();
            if (!vm || !a_form) return false;
            auto* policy = vm->GetObjectHandlePolicy();
            if (!policy) return false;
            const auto h = policy->GetHandleForObject(
                static_cast<RE::VMTypeID>(a_form->GetFormType()), a_form);
            if (h == policy->EmptyHandle()) return false;
            a_out = h;
            return true;
        }

    }

    bool Available() {
        auto* vm = VM();
        return vm && vm->GetObjectHandlePolicy() != nullptr;
    }

    bool DoCombatSpellApply(RE::Actor* a_caster, RE::SpellItem* a_spell,
                            RE::TESObjectREFR* a_target) {
        if (!a_caster || !a_spell || !a_target) {
            ++g_failures;
            return false;
        }

        auto* vm = VM();
        if (!vm) { ++g_failures; return false; }

        RE::VMHandle handle{};
        if (!HandleFor(a_caster, handle)) {
            // No VM identity for this actor. The caller must fall back rather
            // than silently do nothing.
            ++g_failures;
            return false;
        }

        // MakeFunctionArguments RETURNS A RAW `new` -- the VM packs the
        // arguments synchronously during dispatch and never takes ownership, so
        // a bare local leaks one allocation per call. At tick cadence that is a
        // slow leak, which is the worst kind to find later.
        std::unique_ptr<RE::BSScript::IFunctionArguments> args{
            RE::MakeFunctionArguments(std::move(a_spell), std::move(a_target)) };
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;

        // DispatchMethodCall2 is the HANDLE form -- the object form would need a
        // bound script instance, and Actor.psc's natives are reachable by class
        // name against the handle without one.
        const bool ok = vm->DispatchMethodCall2(handle, "Actor", "DoCombatSpellApply",
                                                args.get(), callback);
        if (ok) ++g_dispatches;
        else    ++g_failures;
        return ok;
    }

    bool DispatchActivate(RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_activator) {
        if (!a_ref || !a_activator) {
            ++g_failures;
            return false;
        }

        auto* vm = VM();
        if (!vm) { ++g_failures; return false; }

        // The handle is the REF's (the thing being activated); the follower
        // rides along as akActionRef. Same policy mapping as DoCombatSpellApply.
        RE::VMHandle handle{};
        if (!HandleFor(a_ref, handle)) {
            ++g_failures;
            return false;
        }

        // abDefaultProcessingOnly=false: run the FULL activation (an ammo/gold
        // pile's default processing IS the pickup; false also lets any
        // OnActivate blocks fire exactly as a real activation would).
        // unique_ptr for the same MakeFunctionArguments leak noted above.
        std::unique_ptr<RE::BSScript::IFunctionArguments> args{
            RE::MakeFunctionArguments(std::move(a_activator), false) };
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;

        // Handle form again: ObjectReference.psc's Activate is native and
        // reachable by class name against the handle, no bound script needed.
        const bool ok = vm->DispatchMethodCall2(handle, "ObjectReference", "Activate",
                                                args.get(), callback);
        if (ok) ++g_dispatches;
        else    ++g_failures;
        return ok;
    }

    bool DispatchTradeRun(RE::TESForm* a_quest, std::int32_t a_token) {
        if (!a_quest) { ++g_failures; return false; }
        auto* vm = VM();
        if (!vm) { ++g_failures; return false; }

        // The handle is the QUEST's; MFO_Trade is bound to it via VMAD, so the
        // custom-class method resolves against a real script instance (unlike the
        // vanilla-class calls above, which need no bound script).
        RE::VMHandle handle{};
        if (!HandleFor(a_quest, handle)) { ++g_failures; return false; }

        std::unique_ptr<RE::BSScript::IFunctionArguments> args{
            RE::MakeFunctionArguments(std::move(a_token)) };
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;

        const bool ok = vm->DispatchMethodCall2(handle, "MFO_Trade", "RunTrade",
                                                args.get(), callback);
        if (ok) ++g_dispatches;
        else    ++g_failures;
        return ok;
    }

    std::uint32_t Dispatches() { return g_dispatches.load(); }
    std::uint32_t Failures()   { return g_failures.load(); }

    void ClearTransientState() {
        g_dispatches = 0;
        g_failures   = 0;
    }

}
