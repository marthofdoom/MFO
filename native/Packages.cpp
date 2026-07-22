#include "PCH.h"
#include "Packages.h"
#include "Config.h"
#include "Forms.h"

namespace MFO::Packages {

    namespace {

        // ── LAYOUT: the two package-data classes CommonLib does NOT define ──
        //
        // MFO_CastPackage's Spell and Target inputs are instances of
        // BGSPackageDataTargetSelector and BGSPackageDataRef. NEITHER CLASS
        // EXISTS at the pinned rev c4ab853d -- only their RTTI ids do. So the
        // offset of the PackageTarget* they carry had to be recovered rather
        // than read. Getting it wrong is a silent memory stomp, so the whole
        // derivation is written down here.
        //
        // Recovered from the game binary's MSVC RTTI ClassHierarchyDescriptor
        // + BaseClassArray (.rdata is plaintext even though .text is Steam-DRM
        // encrypted, so the hierarchy is readable and the function bodies are
        // not). The two classes DO NOT have the same shape:
        //
        //   BGSPackageDataTargetSelector   COL attributes=0 (SINGLE inheritance)
        //     BGSPackageDataPointerTemplate<IPackageData, PackageTarget, ...>
        //     BGSNamedPackageData<IPackageData>
        //     IPackageData                                    <- mdisp 0
        //   object: [IPackageData vptr @00][data @08][pointer @10]  sizeof 0x18
        //
        //   BGSPackageDataRef              COL attributes=1 (MULTIPLE inheritance)
        //     BGSPackageDataPointerTemplate<IAITarget, PackageTarget, ...>
        //     BGSNamedPackageData<IAITarget>
        //     IAITarget / IPackageDataAIWorldLocationHandle /
        //     IAIWorldLocationHandle                          <- mdisp 0
        //     IPackageData                                    <- mdisp 8, 2nd vptr
        //   object: [prim vptr @00][IPackageData vptr @08][data @10][pointer @18]
        //                                                            sizeof 0x20
        //
        // SO THE MEMBER OFFSETS DIFFER -- 0x10 vs 0x18 -- AND YET ONE CONSTANT
        // IS CORRECT FOR BOTH. That is not luck and it is the subtle part:
        //
        // the array we read these out of is typed `IPackageData** data`, so
        // what is stored is a pointer to the IPackageData SUBOBJECT, already
        // base-adjusted by the compiler that stored it. (It has to be: every
        // use is a virtual call through IPackageData's vtable, which would go
        // through the wrong vptr otherwise. Our own GetTypeName() guard below
        // is such a call, and it returns the right string.) For the multiple-
        // inheritance case that pointer is object+0x08, exactly the 0x08 by
        // which the members are pushed down. THE ADJUSTMENT CANCELS THE EXTRA
        // BASE, because in both layouts `data` immediately follows the
        // IPackageData vptr and `pointer` immediately follows `data`:
        //
        //     TargetSelector:  IPackageData* = obj+0x00, pointer = obj+0x10
        //     Ref:             IPackageData* = obj+0x08, pointer = obj+0x18
        //                                   => pointer = IPackageData* + 0x10 in BOTH
        //
        // CROSS-CHECKED against the two layouts CommonLib DOES assert at the
        // pin, which agree exactly:
        //   RE/B/BGSPackageDataBool.h      offsetof(data)==0x08, sizeof==0x10
        //                                  (the single-inheritance shape)
        //   RE/B/BGSPackageDataLocation.h  sizeof==0x20, parent
        //                                  IPackageDataAIWorldLocationHandle
        //                                  asserted 0x10 -- the multiple-
        //                                  inheritance shape, pointer @0x18,
        //                                  i.e. IPackageData* + 0x10 again.
        constexpr std::size_t kPointerOffFromIPackageData = 0x10;

        // The type-name strings IPackageData::GetTypeName() returns for the two
        // classes we write through. These are what the ESP's ANAM subrecords
        // carry for the Spell and Target value slots respectively.
        //
        // THIS GUARD IS A MEMORY-SAFETY GUARD, not a nicety. The offset above
        // is only meaningful for a BGSPackageDataPointerTemplate; a
        // BGSPackageDataBool is 0x10 bytes TOTAL, so reading +0x10 off one
        // would be a read one past the end of the object. An input that does
        // not name itself as one of these two is not a PackageTarget carrier
        // and must never be read through, let alone written.
        constexpr std::string_view kTypeTargetSelector = "TargetSelector"sv;
        constexpr std::string_view kTypeSingleRef      = "SingleRef"sv;

        // The template input NAMES we mutate. BY NAME, NEVER BY INDEX: the
        // value list is positional on disk, but UNAM[i] labels which template
        // input each slot fills, so the name is the stable key and the index
        // is not. UseMagic 000504F5 declares 13 inputs and MFO supplies 11 of
        // them; hardcoding "slot 1" would silently follow the wrong input the
        // day the generator emits one more.
        constexpr std::string_view kInputSpell  = "Spell"sv;
        constexpr std::string_view kInputTarget = "Target"sv;

        // PTDA targType values, established by resolving what vanilla actually
        // put in the target slot for each (not from format docs):
        //   0 Specific Reference, 1 Object ID, 2 Object Type,
        //   3 Linked Reference,   4 Reference Alias, 6 Self
        constexpr std::int8_t kTargTypeObjectID = 1;   // the Spell slot
        constexpr std::int8_t kTargTypeAlias    = 4;   // the Target slot, self route
        constexpr std::int8_t kTargTypeRef      = 0;   // the Target slot, foe route

        // Alias 0 is the FOLLOWER -- the carrier AND, for a self-cast, the
        // target. ALPC delivers the package BY the actor being in the alias,
        // so the follower cannot be in alias 1; that inversion is the single
        // most important structural fact in this system.
        constexpr std::uint32_t kAliasCommandActor = 0;

        // §4.5c: an action is BOUNDED. Both halves -- a completion condition
        // and a hard timeout -- or a stuck package is a follower MFO has
        // quietly taken away from the player forever.
        constexpr float kFillTimeout = 3.0f;    // Requested -> Filled
        constexpr float kRunTimeout  = 20.0f;   // Filled/Running -> Done

        using Clock = std::chrono::steady_clock;

        // ONE holder, and that is forced by the records, not chosen: there is
        // one MFO_CastPackage and one alias 0, and TESPackage instances are
        // SHARED (refCount at +0xDC), so mutating the Spell input while a
        // second follower was mid-cast would change the spell under them.
        struct Holder {
            Phase             phase      = Phase::Idle;
            RE::FormID        actorID    = 0;
            RE::FormID        spellID    = 0;
            Clock::time_point phaseAt{};
            Clock::time_point startAt{};
        };
        Holder g_holder;

        std::atomic<std::uint32_t> g_requests{ 0 };
        std::atomic<std::uint32_t> g_completions{ 0 };
        std::atomic<std::uint32_t> g_declines{ 0 };
        std::atomic<std::uint32_t> g_timeouts{ 0 };

        float Since(Clock::time_point a_t) {
            if (a_t.time_since_epoch().count() == 0) return 0.0f;
            return std::chrono::duration<float>(Clock::now() - a_t).count();
        }

        void SetPhase(Phase a_next, const char* a_why) {
            if (g_holder.phase == a_next) return;
            spdlog::info("[pkg] {:08X}: {} -> {} ({}, {:.2f}s in phase)",
                         g_holder.actorID, PhaseName(g_holder.phase),
                         PhaseName(a_next), a_why, Since(g_holder.phaseAt));
            g_holder.phase   = a_next;
            g_holder.phaseAt = Clock::now();
        }

        RE::BSScript::Internal::VirtualMachine* VM() {
            return RE::BSScript::Internal::VirtualMachine::GetSingleton();
        }

        // A VM handle for an ALIAS, which is NOT a TESForm.
        //
        // Papyrus.cpp's HandleFor() derives the VMTypeID from
        // TESForm::GetFormType() and therefore cannot do this -- a BGSRefAlias
        // has no form type. Its VM identity is a fixed id instead:
        // BGSRefAlias::VMTYPEID = 140, BGSBaseAlias::VMTYPEID = 139 (both
        // asserted in the headers at the pinned rev, and independently
        // confirmed against SKSE64's own GameForms.h enum, where
        // kFormType_Alias=139 and kFormType_ReferenceAlias=140).
        //
        // GetHandleForObject's first overload takes `const void*`, not
        // `TESForm*`, so passing an alias is in-contract and not a cast trick.
        //
        // PRECEDENT, because "can I mint a handle for a non-form" deserved a
        // shipped answer rather than a plausible one: SKSE itself does it.
        // PapyrusAlias.cpp registers 23 natives with base type BGSBaseAlias on
        // script class "Alias", PapyrusReferenceAlias.cpp registers
        // NativeFunction2<BGSRefAlias, ...>("ForceRefToWornItem",
        // "ReferenceAlias", ...), and PapyrusEvents.h routes alias event
        // registrations through GetHandleForObject(typeID, obj) in exactly
        // this shape.
        bool HandleForAlias(RE::BGSRefAlias* a_alias, RE::VMHandle& a_out) {
            auto* vm = VM();
            if (!vm || !a_alias) return false;
            auto* policy = vm->GetObjectHandlePolicy();
            if (!policy) return false;
            const auto h = policy->GetHandleForObject(RE::BGSRefAlias::VMTYPEID, a_alias);
            // INVALID IS NOT ZERO. The policy returns its own sentinel, and in
            // Skyrim that sentinel is nonzero -- comparing against 0 lets a
            // dead handle through to dispatch and makes the counters lie about
            // where it died. Same rule as Papyrus.cpp.
            if (h == policy->EmptyHandle()) return false;
            a_out = h;
            return true;
        }

        RE::BGSRefAlias* CommandAlias() {
            auto* quest = Forms::g_commandQuest;
            if (!quest) return nullptr;
            for (auto* base : quest->aliases) {
                if (!base || base->aliasID != kAliasCommandActor) continue;
                return skyrim_cast<RE::BGSRefAlias*>(base);
            }
            return nullptr;
        }

        // Dispatch ReferenceAlias.ForceRefTo / .Clear.
        //
        // WHY THE VM AND NOT A NATIVE CALL: SKSE's own PapyrusReferenceAlias
        // reaches this through CALL_MEMBER_FN(alias->owner, ForceRefTo), i.e.
        // TESQuest::ForceRefTo(aliasID, refr) -- which would be synchronous and
        // strictly nicer. But that function is NOT WRAPPED IN COMMONLIB AT THE
        // PINNED REV (grep: zero hits across include/ and src/), so using it
        // means inventing a REL::Relocation id. This project's standing rule is
        // "prefer 'I could not verify this at the pinned rev' over a plausible
        // API name" -- names taken from current-master docs have failed CI here
        // repeatedly, and StartCombat's hand-sourced offset ALREADY DID NOT
        // WORK IN THE FIELD (ENGINE_NOTES §0.12). So: the VM, whose only cost
        // is asynchrony, and asynchrony is something the state machine below
        // already has to handle anyway.
        bool DispatchAlias(const char* a_fn, RE::TESObjectREFR* a_arg) {
            auto* vm = VM();
            auto* alias = CommandAlias();
            if (!vm || !alias) return false;

            RE::VMHandle handle{};
            if (!HandleForAlias(alias, handle)) {
                spdlog::error("[pkg] no VM handle for alias {} -- the policy refused a "
                              "non-form object; {} is unreachable", kAliasCommandActor, a_fn);
                return false;
            }

            // MakeFunctionArguments RETURNS A RAW `new`. The VM packs the
            // arguments synchronously during dispatch and never takes
            // ownership, so a bare local leaks one allocation per call -- at
            // tick cadence that is a slow leak, the worst kind to find later.
            std::unique_ptr<RE::BSScript::IFunctionArguments> args{
                a_arg ? RE::MakeFunctionArguments(std::move(a_arg))
                      : RE::MakeFunctionArguments() };
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;

            return vm->DispatchMethodCall2(handle, "ReferenceAlias", a_fn,
                                           args.get(), callback);
        }

        // ── INPUT MUTATION, by name ─────────────────────────────────────────

        RE::TESCustomPackageData* CustomData(RE::TESPackage* a_pkg) {
            if (!a_pkg || !a_pkg->data) return nullptr;
            return skyrim_cast<RE::TESCustomPackageData*>(a_pkg->data);
        }

        // Resolve a template input NAME to the live IPackageData* that fills
        // it. Two indirections, both of which are the engine's own:
        //
        //   nameMap : BNAM name  -> UNAM uid   (the TEMPLATE's declaration)
        //   uids[i] : slot i     -> UNAM uid   (which input THIS instance fills)
        //
        // so name -> uid -> slot -> data[slot]. The instance emits no
        // declarations of its own (it inherits them through PKCU.template), so
        // the name map may live on the template's data instead -- hence the
        // fallback.
        RE::IPackageData* FindInput(RE::TESPackage* a_pkg, std::string_view a_name) {
            auto* cpd = CustomData(a_pkg);
            if (!cpd) return nullptr;

            // Find the uid the template declared for this name.
            std::int8_t uid  = -1;
            auto*       maps = cpd->nameMap.get();
            if (!maps && cpd->templateParent) {
                if (auto* tpl = CustomData(cpd->templateParent)) {
                    maps = tpl->nameMap.get();
                }
            }
            if (!maps) {
                spdlog::error("[pkg] package {:08X} has no name map on the instance OR its "
                              "template -- inputs cannot be resolved by name",
                              a_pkg->GetFormID());
                return nullptr;
            }
            for (const auto& nm : maps->nameMap) {
                if (!nm.name.empty() && a_name == std::string_view(nm.name.c_str())) {
                    uid = nm.uid;
                    break;
                }
            }
            if (uid < 0) {
                spdlog::error("[pkg] template input '{}' is not declared on package {:08X}",
                              a_name, a_pkg->GetFormID());
                return nullptr;
            }

            // Find which of OUR value slots carries that uid.
            if (!cpd->data.data || !cpd->data.uids) return nullptr;
            for (std::uint16_t i = 0; i < cpd->data.dataSize; ++i) {
                if (cpd->data.uids[i] != uid) continue;
                return cpd->data.data[i];
            }
            spdlog::error("[pkg] input '{}' (uid {}) is declared but this instance supplies "
                          "no value for it", a_name, uid);
            return nullptr;
        }

        // Recover the PackageTarget* a data input carries, guarded by its own
        // reported type name, and VALIDATED against the targType the ESP
        // authored. The validation is not belt-and-braces -- it is what makes
        // the recovered offsets above safe to use at all (see the residual
        // uncertainty note there). A mismatch means our layout model is wrong,
        // and the correct response is to decline loudly, never to write.
        RE::PackageTarget* ReadTarget(RE::IPackageData* a_pd, std::int8_t a_expectTargType,
                                      std::string_view a_what) {
            if (!a_pd) return nullptr;

            const auto& tn = a_pd->GetTypeName();
            const std::string_view name = tn.empty() ? std::string_view{} : std::string_view(tn.c_str());

            if (name != kTypeTargetSelector && name != kTypeSingleRef) {
                spdlog::error("[pkg] {} input reports type '{}' -- not a PackageTarget carrier; "
                              "refusing to read through it", a_what,
                              name.empty() ? "<unnamed>"sv : name);
                return nullptr;
            }
            constexpr std::size_t off = kPointerOffFromIPackageData;

            auto* pt = *reinterpret_cast<RE::PackageTarget* const*>(
                           reinterpret_cast<const unsigned char*>(a_pd) + off);
            if (!pt) {
                spdlog::error("[pkg] {} input '{}' has a null PackageTarget at +0x{:X}",
                              a_what, name, off);
                return nullptr;
            }
            if (pt->targType != a_expectTargType) {
                // Either the ESP does not say what the generator thinks it
                // says, or the offset model is wrong. Both are our bug, and
                // both are silent if we write anyway.
                spdlog::error("[pkg] {} input '{}' at +0x{:X}: targType is {}, expected {} -- "
                              "LAYOUT MODEL REJECTED, not writing",
                              a_what, name, off,
                              static_cast<int>(pt->targType), static_cast<int>(a_expectTargType));
                return nullptr;
            }
            return pt;
        }

        // Point the package at a spell and a target. Returns false without
        // having written ANYTHING if either input fails its guard -- a
        // half-mutated package would cast the right spell at the wrong thing.
        bool SetInputs(RE::SpellItem* a_spell, RE::TESObjectREFR* a_ref) {
            auto* pkg = Forms::g_castPackage;
            if (!pkg) return false;

            auto* spellPd  = FindInput(pkg, kInputSpell);
            auto* targetPd = FindInput(pkg, kInputTarget);
            if (!spellPd || !targetPd) return false;

            const std::int8_t wantTarg = a_ref ? kTargTypeRef : kTargTypeAlias;

            auto* spellTarget  = ReadTarget(spellPd,  kTargTypeObjectID, "Spell");
            auto* targetTarget = ReadTarget(targetPd, wantTarg,          "Target");
            if (!spellTarget || !targetTarget) return false;

            // BOTH guards passed -- only now do we write.
            spellTarget->targType      = kTargTypeObjectID;
            spellTarget->target.object = a_spell;
            spellTarget->value         = 0;

            if (a_ref) {
                // UNVERIFIED ROUTE (Q8). ObjectRefHandle is a first-class
                // member of PackageTarget::Target at the pin and targType 0 is
                // the only union member it can correspond to -- but on disk
                // targType 0 stores a FormID, so the engine converts
                // form->handle at load and that conversion was never observed.
                targetTarget->targType      = kTargTypeRef;
                targetTarget->target.handle = a_ref->CreateRefHandle();
                targetTarget->value         = 0;
            } else {
                targetTarget->targType       = kTargTypeAlias;
                targetTarget->target.aliasID = kAliasCommandActor;
                targetTarget->value          = 0;
            }
            return true;
        }

        // ── CONTENTION ──────────────────────────────────────────────────────

        // Does another quest already own the alias-package layer on this actor
        // at a priority we would not beat?
        //
        // THE SUBTLETY THAT MAKES THIS NOT A BLANKET DECLINE: every vanilla
        // follower is ALREADY in DialogueFollower's alias, which carries ALPC
        // packages. Declining on "any other quest present" would decline
        // always, and M9 would be a no-op that looked like caution. The
        // designed behaviour (§4.6 layer 2) is that the ENGINE arbitrates by
        // quest priority -- MFO is 60, DialogueFollower is 50, vanilla's mode
        // is 30 -- so a lower-priority owner is not contention at all, it is
        // the arbitration working.
        //
        // What IS contention is an owner at >= our priority: we would lose, or
        // tie unpredictably. Then we DECLINE and name them. We never raise our
        // own number to win -- that is the escalation §4.6 forbids, and it is
        // the move that turns a compatibility story into an arms race.
        bool ForeignOwnerBlocks(RE::Actor* a_actor, std::string& a_who) {
            auto* mine = Forms::g_commandQuest;
            if (!a_actor || !mine) return false;

            auto* arr = a_actor->extraList.GetByType<RE::ExtraAliasInstanceArray>();
            if (!arr) return false;   // no alias packages at all -- clear road

            const std::int8_t myPriority = mine->data.priority;

            // The array has its own lock and the engine writes it from the
            // alias-fill path; walking it unlocked is a torn read waiting for a
            // cell transition.
            RE::BSReadLockGuard guard(arr->lock);
            for (auto* inst : arr->aliases) {
                if (!inst || !inst->quest || inst->quest == mine) continue;
                if (!inst->instancedPackages || inst->instancedPackages->empty()) continue;

                const std::int8_t theirs = inst->quest->data.priority;
                if (theirs < myPriority) continue;   // we win -- arbitration, not contention

                // Name the package's OWNER QUEST, not just the alias quest:
                // those can differ, and the owner is what QNAM scopes the
                // package's alias targets against.
                const auto* pkg   = inst->instancedPackages->front();
                const auto* owner = pkg ? pkg->ownerQuest : nullptr;
                a_who = std::format(
                    "quest {:08X} (priority {}) alias {} pkg {:08X} ownerQuest {:08X}",
                    inst->quest->GetFormID(), static_cast<int>(theirs),
                    inst->alias ? inst->alias->aliasID : 0u,
                    pkg ? pkg->GetFormID() : 0u,
                    owner ? owner->GetFormID() : 0u);
                return true;
            }
            return false;
        }

        // ── OBSERVATION ─────────────────────────────────────────────────────

        // Is alias 0 currently filled with the holder? This is the ONLY honest
        // answer to "did the fill land", because the dispatch that requested it
        // returned long before the VM ran it.
        //
        // Read through CreateRefHandleByAliasID -- the engine's own reader --
        // rather than by indexing refAliasMap. We do NOT take aliasAccessLock
        // around it: the reader takes it internally, and BSReadWriteLock is not
        // recursive, so wrapping it would deadlock.
        bool FillObserved(RE::FormID a_actorID) {
            auto* quest = Forms::g_commandQuest;
            if (!quest || !a_actorID) return false;
            RE::ObjectRefHandle handle{};
            quest->CreateRefHandleByAliasID(handle, kAliasCommandActor);
            auto ref = handle.get();
            return ref && ref->GetFormID() == a_actorID;
        }

        RE::Actor* HolderActor() {
            if (!g_holder.actorID) return nullptr;
            auto* form = RE::TESForm::LookupByID(g_holder.actorID);
            return form ? form->As<RE::Actor>() : nullptr;
        }

        // Is MFO's package the one actually driving this actor?
        //
        // Two readings, deliberately: CheckForCurrentAliasPackage (vfunc 0x049)
        // is the engine's own "does an alias hand me a package?" and is the
        // function the whole ALPC route runs through; GetCurrentPackage is what
        // is actually RUNNING (runOncePackage ?: currentPackage). They can
        // disagree -- the alias can offer a package the actor has not switched
        // to yet -- and that gap is exactly the Filled->Running edge.
        bool PackageOffered(RE::Actor* a_actor) {
            if (!a_actor || !Forms::g_castPackage) return false;
            return a_actor->CheckForCurrentAliasPackage() == Forms::g_castPackage;
        }

        bool PackageRunning(RE::Actor* a_actor) {
            if (!a_actor || !Forms::g_castPackage) return false;
            return a_actor->GetCurrentPackage() == Forms::g_castPackage;
        }

        void ResetHolder() {
            g_holder = Holder{};
        }

        // Clear the alias. Safe to call in any phase.
        void ClearAlias(const char* a_why) {
            const auto id = g_holder.actorID;
            if (!DispatchAlias("Clear", nullptr)) {
                // The fill is SERIALIZED INTO THE SAVE. A clear we could not
                // dispatch is not a cosmetic miss -- it is a follower who comes
                // back latched on every subsequent load.
                spdlog::error("[pkg] {:08X}: Clear() DISPATCH FAILED ({}) -- the alias fill "
                              "persists in the save; the follower may reload latched", id, a_why);
            } else {
                spdlog::info("[pkg] {:08X}: released ({})", id, a_why);
            }
            ResetHolder();
        }

        Decline Begin(RE::Actor* a_follower, RE::SpellItem* a_spell,
                      RE::TESObjectREFR* a_target) {
            if (!Config::g_usePackages.load()) return Decline::Disabled;
            if (!a_follower || !a_spell)       return Decline::NoRecord;

            auto* quest = Forms::g_commandQuest;
            auto* pkg   = Forms::g_castPackage;
            if (!quest || !pkg)                return Decline::NoRecord;
            if (!VM() || !CommandAlias())      return Decline::NoVM;

            // A start-game-enabled, non-run-once quest that is not in the SEQ
            // NEVER STARTS on an existing save, and the whole mechanism is then
            // silently absent. Say so by name rather than failing to fill.
            if (!quest->IsRunning()) {
                spdlog::error("[pkg] MFO_CommandQuest {:08X} is NOT RUNNING -- Data/SEQ/MFO.seq "
                              "missing or stale? No alias can be filled.", quest->GetFormID());
                return Decline::QuestStopped;
            }

            const auto id = a_follower->GetFormID();

            // One alias, one shared PACK record -- see the header. A second
            // requester is refused, not queued.
            //
            // NOTE THIS REFUSES THE SAME FOLLOWER TOO, not just a different
            // one. Re-requesting mid-action would rewrite the Spell input of a
            // package that is CURRENTLY RUNNING on them -- the shared-instance
            // hazard (TESPackage::refCount at +0xDC) reaching us through time
            // rather than through a second actor. Only Idle and Done are safe
            // moments to re-point the record.
            if (g_holder.phase != Phase::Idle && g_holder.phase != Phase::Done) {
                return Decline::Busy;
            }

            // BEFORE acting, not after.
            std::string who;
            if (ForeignOwnerBlocks(a_follower, who)) {
                spdlog::info("[pkg] {:08X}: DECLINED -- alias layer owned by {}. MFO is priority "
                             "{}; not escalating (§4.6).", id, who,
                             static_cast<int>(quest->data.priority));
                return Decline::Contention;
            }

            if (!SetInputs(a_spell, a_target)) return Decline::BadInputs;

            if (!DispatchAlias("ForceRefTo", a_follower)) {
                spdlog::error("[pkg] {:08X}: ForceRefTo dispatch failed", id);
                return Decline::NoVM;
            }

            ResetHolder();
            g_holder.actorID = id;
            g_holder.spellID = a_spell->GetFormID();
            g_holder.startAt = Clock::now();
            g_holder.phaseAt = Clock::now();
            g_holder.phase   = Phase::Requested;
            ++g_requests;

            spdlog::info("[pkg] {:08X}: requested cast of {:08X} ({}) -- fill DISPATCHED, "
                         "not yet observed", id, g_holder.spellID,
                         a_target ? "at reference" : "self/alias-0");
            return Decline::None;
        }

    }  // namespace

    const char* PhaseName(Phase a_phase) {
        switch (a_phase) {
        case Phase::Idle:      return "Idle";
        case Phase::Requested: return "Requested";
        case Phase::Filled:    return "Filled";
        case Phase::Running:   return "Running";
        case Phase::Done:      return "Done";
        default:               return "?";
        }
    }

    const char* DeclineName(Decline a_reason) {
        switch (a_reason) {
        case Decline::None:         return "none";
        case Decline::Disabled:     return "bUsePackages off";
        case Decline::NoRecord:     return "record unresolved";
        case Decline::NoVM:         return "VM unreachable";
        case Decline::QuestStopped: return "command quest not running";
        case Decline::Busy:         return "alias held by another follower";
        case Decline::Contention:   return "alias layer owned at >= our priority";
        case Decline::BadInputs:    return "package inputs unresolvable";
        default:                    return "?";
        }
    }

    bool Available() {
        return Config::g_usePackages.load() &&
               Forms::g_commandQuest != nullptr &&
               Forms::g_castPackage  != nullptr &&
               VM() != nullptr &&
               CommandAlias() != nullptr;
    }

    Decline CastSelf(RE::Actor* a_follower, RE::SpellItem* a_spell) {
        const auto r = Begin(a_follower, a_spell, nullptr);
        if (r != Decline::None) {
            ++g_declines;
            spdlog::debug("[pkg] {:08X}: declined -- {}",
                          a_follower ? a_follower->GetFormID() : 0u, DeclineName(r));
        }
        return r;
    }

    Decline CastAt(RE::Actor* a_follower, RE::SpellItem* a_spell,
                   RE::TESObjectREFR* a_target) {
        if (!a_target) return CastSelf(a_follower, a_spell);
        const auto r = Begin(a_follower, a_spell, a_target);
        if (r != Decline::None) {
            ++g_declines;
            spdlog::debug("[pkg] {:08X}: declined -- {}",
                          a_follower ? a_follower->GetFormID() : 0u, DeclineName(r));
        }
        return r;
    }

    void Pump() {
        if (g_holder.phase == Phase::Idle) return;

        auto* actor = HolderActor();
        if (!actor) {
            // Died, unloaded, or the id no longer resolves. The fill outlives
            // the actor's 3D, so it still has to come off.
            ClearAlias("holder unresolvable");
            return;
        }

        const bool filled  = FillObserved(g_holder.actorID);
        const bool offered = PackageOffered(actor);
        const bool running = PackageRunning(actor);

        switch (g_holder.phase) {
        case Phase::Requested:
            if (filled) {
                SetPhase(Phase::Filled, "alias fill observed");
                // Nudge the AI to reconsider now that alias membership changed.
                //
                // a_resetAI IS FALSE AND MUST STAY FALSE. ALYSLC field-proved
                // that resetting AI clears the combat group and the NEXT HIT
                // DOES ZERO DAMAGE -- and a gambit fires in combat by
                // definition, so this call is always the in-combat case.
                //
                // Whether the nudge is even needed is an open question:
                // ENGINE_NOTES §0.7 found EvaluatePackage a no-op for
                // CONDITION-driven selection, but this route selects by alias
                // MEMBERSHIP, so the fill is itself the state change. Logged
                // either way so the field can settle it.
                actor->EvaluatePackage(true, false);
                spdlog::info("[pkg] {:08X}: EvaluatePackage(immediate, NO reset) after fill",
                             g_holder.actorID);
            } else if (Since(g_holder.startAt) > kFillTimeout) {
                ++g_timeouts;
                spdlog::warn("[pkg] {:08X}: fill NEVER OBSERVED after {:.1f}s -- ForceRefTo was "
                             "dispatched but the alias did not take. Reserved by a mod that "
                             "does not allow sharing?", g_holder.actorID, kFillTimeout);
                ClearAlias("fill timeout");
            }
            break;

        case Phase::Filled:
            if (running) {
                SetPhase(Phase::Running, "package is the actor's current package");
            } else if (!filled) {
                // Someone else cleared or stole the alias under us.
                spdlog::warn("[pkg] {:08X}: alias fill vanished before the package ran",
                             g_holder.actorID);
                ClearAlias("fill lost");
            } else if (Since(g_holder.startAt) > kRunTimeout) {
                ++g_timeouts;
                spdlog::warn("[pkg] {:08X}: filled but the package never ran in {:.1f}s "
                             "(offered={}) -- outranked by a higher-priority package?",
                             g_holder.actorID, kRunTimeout, offered ? "Y" : "n");
                ClearAlias("run timeout");
            }
            break;

        case Phase::Running:
            if (!running) {
                SetPhase(Phase::Done, "package no longer current");
                ++g_completions;
            } else if (Since(g_holder.startAt) > kRunTimeout) {
                // §4.5c: the hard timeout. The package's own CastTime/Cooldown/
                // NumToCast inputs bound it normally; this is the backstop for
                // when they do not, because a package that never ends is a
                // follower MFO has taken away from the player.
                ++g_timeouts;
                spdlog::warn("[pkg] {:08X}: package still running after {:.1f}s -- hard timeout",
                             g_holder.actorID, kRunTimeout);
                ClearAlias("hard timeout");
            }
            break;

        case Phase::Done:
            ClearAlias("action complete");
            break;

        default:
            break;
        }
    }

    void Release(RE::FormID a_actorID) {
        if (g_holder.phase == Phase::Idle) return;
        if (g_holder.actorID != a_actorID) return;
        ClearAlias("released");
    }

    void ReleaseAll(const char* a_why) {
        if (g_holder.phase == Phase::Idle) {
            // Clear anyway on the lifecycle edges: a fill from a PREVIOUS
            // session is in the save and this module has no memory of it, so
            // "we think we hold nothing" is not evidence the alias is empty.
            if (Forms::g_commandQuest && CommandAlias() && VM()) {
                RE::ObjectRefHandle handle{};
                Forms::g_commandQuest->CreateRefHandleByAliasID(handle, kAliasCommandActor);
                if (handle.get()) {
                    spdlog::warn("[pkg] {} -- alias 0 was filled with {:08X} and MFO did not put "
                                 "it there. A previous session's latch, restored from the save. "
                                 "Clearing.", a_why, handle.get()->GetFormID());
                    DispatchAlias("Clear", nullptr);
                }
            }
            return;
        }
        ClearAlias(a_why);
    }

    Status Get() {
        Status s;
        s.phase        = g_holder.phase;
        s.holder       = g_holder.actorID;
        s.spell        = g_holder.spellID;
        s.phaseSeconds = Since(g_holder.phaseAt);
        s.totalSeconds = Since(g_holder.startAt);
        if (g_holder.phase != Phase::Idle) {
            s.fillObserved = FillObserved(g_holder.actorID);
            auto* actor    = HolderActor();
            s.packageLive  = PackageRunning(actor);
        }
        s.requests    = g_requests.load();
        s.completions = g_completions.load();
        s.declines    = g_declines.load();
        s.timeouts    = g_timeouts.load();
        return s;
    }

    void ClearTransientState() {
        ResetHolder();
        g_requests    = 0;
        g_completions = 0;
        g_declines    = 0;
        g_timeouts    = 0;
    }

}
