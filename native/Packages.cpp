#include "PCH.h"
#include "Packages.h"
#include "Followers.h"
#include "Config.h"
#include "Forms.h"
#include "Sightline.h"   // concentration hold: the mid-stream line-of-fire watch

namespace MFO::Packages {

    // Defined at the bottom of the file (the loot-clear readback); forward-
    // declared HERE, above the anonymous namespace, because the cast-package
    // release (ClearAlias) now runs the same measured-detach proof as every
    // loot/retreat eviction.
    static void VerifyDetachedFrom(RE::TESQuest* a_quest, RE::Actor* a_follower,
                                   const char* a_tag, const char* a_questName,
                                   const char* a_why);

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
        constexpr std::int8_t kTargTypeAlias    = 4;   // the Target slot, FOE route -> alias 1
        constexpr std::int8_t kTargTypeSelf     = 6;   // the Target slot, SELF route
        constexpr std::int8_t kTargTypeRef      = 0;   // the Target slot, foe route

        // Alias 0 is the FOLLOWER -- the carrier AND, for a self-cast, the
        // target. ALPC delivers the package BY the actor being in the alias,
        // so the follower cannot be in alias 1; that inversion is the single
        // most important structural fact in this system.
        constexpr std::uint32_t kAliasCommandActor  = 0;   // carries the foe package
        constexpr std::uint32_t kAliasCommandTarget = 1;   // the victim, for the foe route
        // FORCED SELF-CAST (SPEC-self-cast-forced): the follower's OTHER carrier
        // alias, whose ALPC is MFO_CastPackageSelf (t6 self, NO QNAM). A follower
        // is only ever in alias 0 (foe cast) OR alias 2 (self cast), never both
        // -- the single holder guarantees it -- so each alias still carries
        // exactly one package. No target alias: t6 aims the caster at himself.
        constexpr std::uint32_t kAliasCommandSelfActor = 2;

        // §4.5c: an action is BOUNDED. Both halves -- a completion condition
        // and a hard timeout -- or a stuck package is a follower MFO has
        // quietly taken away from the player forever.
        //
        // 12 s, not the old 20: the cast package now fires MID-COMBAT (the
        // hybrid forced cast), and the package ROOTS the actor while it owns
        // him (§0.27). A cast takes a few seconds to start (§0.21) plus a
        // couple to charge and release; a follower still rooted at 12 s is a
        // follower the fight has moved past, and holding him another 8 makes
        // the backstop worse than the failure it bounds.
        constexpr float kFillTimeout = 3.0f;    // Requested -> Filled
        constexpr float kRunTimeout  = 12.0f;   // Filled/Running -> Done

        // CLAIM-AND-RELEASE POP (the §0.23 gap): once the cast is OBSERVED
        // (the [cast] sink saw the holder fire OUR spell), hold this long for
        // the release animation to finish, then hand the follower back. The
        // engine's own Running -> Done edge (package no longer current) still
        // releases too -- this is the EARLIER of the two, so a package whose
        // CastTime/Cooldown inputs would keep him another cycle lets go at
        // one cast, which is what a gambit asked for.
        constexpr float kPostCastLinger = 1.0f;

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
            // The pop's completion condition: the [cast] sink observed the
            // holder fire OUR spell (NotifyCast). Pump releases kPostCastLinger
            // after -- the "cast happened, hand him back" edge §0.23 lacked.
            bool              castSeen   = false;
            Clock::time_point castSeenAt{};
            // CONCENTRATION HOLD (see Packages.h). holdSeconds == 0 is the
            // fire-and-forget lifecycle above; > 0 replaces the linger with
            // the caller's bound and arms the early-release watches. targetID
            // is the aim ref, kept for the ffWatch line check. Same
            // single-writer discipline as every other g_holder field.
            CastHold          hold{};
            RE::FormID        targetID   = 0;
            // FORCED SELF-CAST: this holder is a self-cast (carrier alias 2,
            // MFO_CastPackageSelf) rather than a foe cast (alias 0, MFO_
            // CastPackage). Selects which alias/package every lifecycle helper
            // observes and releases. Default false = the unchanged foe route.
            bool              self       = false;
        };
        Holder g_holder;

        // Which carrier alias / package the CURRENT holder occupies. Every
        // observe/release helper routes through these so the self and foe
        // streams share one state machine without duplicating it. Both read the
        // single-writer g_holder, same discipline as every other holder access.
        std::uint32_t HolderActorAlias() {
            return g_holder.self ? kAliasCommandSelfActor : kAliasCommandActor;
        }
        RE::TESPackage* HolderPackage() {
            return g_holder.self ? Forms::g_castPackageSelf : Forms::g_castPackage;
        }

        // THE LIVE-STREAM MIRROR, for the CONSENT HOOKS. g_holder is single-
        // writer (the serialized task queue) and lock-free, so the caster-
        // thread hooks may not read it. This atomic mirrors exactly one fact
        // -- "MFO's bounded concentration stream for (actor, spell) is live"
        // -- packed (actor << 32 | spell), 0 = none. Set in the same Begin
        // call that dispatches the fill -- package selection runs at AI
        // cadence, so the package's own cast always finds it live -- and
        // cleared in ResetHolder (every release path, ReleaseAll included).
        std::atomic<std::uint64_t> g_liveStream{ 0 };

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

        // FILL AND CLEAR THE ALIAS.
        //
        // NATIVE FIRST, VM as fallback. The native is `TESQuest::ForceRefTo
        // (aliasID, refr)` -- SKSE's own PapyrusReferenceAlias reaches it as
        // CALL_MEMBER_FN(alias->owner, ForceRefTo), and SKSE64 2.2.6 (whose
        // CURRENT_RELEASE_RUNTIME is 1.6.1170, this runtime) declares it at
        // GameForms.h:1704 as offset 0x003CDEE0. Decoding versionlib for
        // 1.6.1170 maps that offset to ID 25052, which lands exactly among the
        // other TESQuest members: 25003 EnsureQuestStarted, 25014 ResetQuest,
        // 25052 ForceRefTo, 25066 CreateRefHandleByAliasID.
        //
        // This is NOT a hand-guessed offset of the kind that burned StartCombat
        // (§0.12): it comes from SKSE's own source for this exact runtime and
        // was cross-checked against the address library.
        //
        // WHY IT MATTERS: the native is SYNCHRONOUS. The alias is filled when
        // the call returns, which deletes the fill->valid window §0.24 calls
        // load-bearing and demotes the watchdog from mandatory to defensive.
        //
        // AE ONLY. The SE (1.5.97) id could not be verified -- no SE-era SKSE
        // source is on disk and the SE/AE id windows are not index-aligned. On
        // anything but AE this returns false and the VM path runs.
        bool ForceRefToNative(RE::TESQuest* a_quest, std::uint32_t a_aliasID,
                              RE::TESObjectREFR* a_ref) {
            if (!a_quest) return false;
            if (!REL::Module::IsAE()) return false;   // id unverified off AE

            using func_t = std::uint32_t (*)(RE::TESQuest*, std::uint32_t, RE::TESObjectREFR*);
            static const REL::Relocation<func_t> func{ REL::ID(25052) };
            func(a_quest, a_aliasID, a_ref);
            return true;
        }

        // Dispatch ReferenceAlias.ForceRefTo / .Clear through the VM.
        // The fallback when the native is unavailable (SE), and the route for
        // Clear, which has no verified native id.
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

        // The statically-known UNAM uids of the two inputs we mutate, on the
        // vanilla UseMagic template 000504F5 (dumped in ENGINE_NOTES §0.17:
        // input 3 "Spell", input 4 "Target"). LAST-RESORT ONLY -- used when
        // BOTH name maps miss the name (below). Safe even if wrong: ReadTarget
        // independently validates the resolved slot's reported TYPE NAME
        // before anything is written through it, so a bad uid resolves to a
        // slot that declines loudly rather than a silent stomp.
        constexpr std::int8_t kUidSpellUseMagic  = 3;
        constexpr std::int8_t kUidTargetUseMagic = 4;

        // One diagnostic dump of what a name map ACTUALLY contains -- emitted
        // only on the both-maps-missed path, once per session, so the next
        // deck log settles the runtime shape instead of leaving us to infer it.
        void DumpNameMap(const char* a_which, RE::BGSPackageDataNameMap* a_maps) {
            if (!a_maps) {
                spdlog::error("[pkg]   {} name map: NULL", a_which);
                return;
            }
            std::string names;
            for (const auto& nm : a_maps->nameMap) {
                names += std::format("{}'{}'={}", names.empty() ? "" : ", ",
                                     nm.name.empty() ? "<empty>" : nm.name.c_str(),
                                     static_cast<int>(nm.uid));
            }
            spdlog::error("[pkg]   {} name map ({} entr{}): {}", a_which,
                          a_maps->nameMap.size(),
                          a_maps->nameMap.size() == 1 ? "y" : "ies",
                          names.empty() ? "<none>" : names);
        }

        // Resolve a template input NAME to the live IPackageData* that fills
        // it. Two indirections, both of which are the engine's own:
        //
        //   nameMap : BNAM name  -> UNAM uid   (the TEMPLATE's declaration)
        //   uids[i] : slot i     -> UNAM uid   (which input THIS instance fills)
        //
        // so name -> uid -> slot -> data[slot].
        //
        // THE v1.0.27-31 BUG, fixed here: the instance's nameMap is NON-NULL
        // at runtime yet does NOT declare "Spell" -- the ESP instance's ANAM
        // subrecords carry input TYPES ("TargetSelector", "SingleRef"), while
        // the human input NAMES live only on the vanilla UseMagic template
        // (000504F5). The old code fell back to the template's map ONLY when
        // the instance map POINTER was null, so every force-on-miss errored
        // "template input 'Spell' is not declared" and silently fell to the
        // invisible CastSpellImmediate path -- the forced cast NEVER fired.
        // Now: a name-lookup MISS (not just a missing map) also consults the
        // template's map; only when BOTH miss does the static-uid last resort
        // run, with a one-time dump of what each map really held.
        RE::IPackageData* FindInput(RE::TESPackage* a_pkg, std::string_view a_name) {
            auto* cpd = CustomData(a_pkg);
            if (!cpd) return nullptr;

            auto lookup = [&](RE::BGSPackageDataNameMap* a_maps) -> std::int8_t {
                if (!a_maps) return -1;
                for (const auto& nm : a_maps->nameMap) {
                    if (!nm.name.empty() && a_name == std::string_view(nm.name.c_str())) {
                        return nm.uid;
                    }
                }
                return -1;
            };

            auto* instMaps = cpd->nameMap.get();
            RE::BGSPackageDataNameMap* tplMaps = nullptr;
            if (cpd->templateParent) {
                if (auto* tpl = CustomData(cpd->templateParent)) {
                    tplMaps = tpl->nameMap.get();
                }
            }

            // Instance first, then the template ON A MISS -- not only on a
            // null pointer. The instance inherits its declarations through
            // PKCU.template, so the template's map is authoritative for names.
            std::int8_t uid = lookup(instMaps);
            if (uid < 0) uid = lookup(tplMaps);

            if (uid < 0) {
                // BOTH missed. Say exactly what each map held -- once -- so
                // the next deck session confirms the runtime shape, then fall
                // back to the statically-known template uids. ReadTarget's
                // type-name guard keeps a wrong uid from ever being written
                // through (it declines loudly instead).
                static std::atomic<bool> s_dumped{ false };
                if (!s_dumped.exchange(true)) {
                    spdlog::error("[pkg] input '{}' missing from BOTH name maps on package "
                                  "{:08X} -- falling back to static template uids "
                                  "(Spell={}, Target={}). Maps as found:",
                                  a_name, a_pkg->GetFormID(),
                                  kUidSpellUseMagic, kUidTargetUseMagic);
                    DumpNameMap("instance", instMaps);
                    DumpNameMap("template", tplMaps);
                }
                if      (a_name == kInputSpell)  uid = kUidSpellUseMagic;
                else if (a_name == kInputTarget) uid = kUidTargetUseMagic;
                else {
                    spdlog::error("[pkg] template input '{}' is not declared on package {:08X} "
                                  "and has no static fallback uid", a_name, a_pkg->GetFormID());
                    return nullptr;
                }
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
        RE::PackageTarget* ReadTarget(RE::IPackageData* a_pd, std::string_view a_what) {
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
            // Deliberately NO targType equality check. The layout proof is the
            // TYPE NAME above -- if that reads as a PackageTarget carrier, the
            // offset model is right. targType is DATA we overwrite every call,
            // so demanding it match beforehand made a routine authoring
            // difference fire the alarm reserved for "our offset model is
            // wrong", poisoning the one diagnostic the whole layout-safety
            // story rests on. Log it as an observation instead.
            spdlog::debug("[pkg] {} input '{}' at +0x{:X}: authored targType {}",
                          a_what, name, off, static_cast<int>(pt->targType));
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

            // The guard validates the LAYOUT (that we really found a
            // PackageTarget at the modelled offset, via its reported type
            // name). It does NOT require the authored targType to equal the one
            // we are about to write -- we overwrite targType by design, and a
            // routine authoring difference must not fire the alarm that means
            // "our offset model is wrong".
            auto* spellTarget  = ReadTarget(spellPd,  "Spell");
            auto* targetTarget = ReadTarget(targetPd, "Target");
            if (!spellTarget || !targetTarget) return false;

            // BOTH guards passed -- only now do we write.
            spellTarget->targType      = kTargTypeObjectID;
            spellTarget->target.object = a_spell;
            spellTarget->value         = 0;

            if (a_ref) {
                // FOE ROUTE, field-proven (probe 5): targType 4 -> ALIAS 1.
                //
                // NOT a live ObjectRefHandle in targType 0. That route also
                // works (probes 1-3) but names the actor at AUTHOR time, so it
                // cannot express "whoever the rule picked this tick". The alias
                // can: the caller fills alias 1 with the victim, the package
                // points at alias 1, and the same record serves every target.
                // This is CWFinaleLeaderExecuteEnemyLeader's shape.
                targetTarget->targType       = kTargTypeAlias;
                targetTarget->target.aliasID = kAliasCommandTarget;
                targetTarget->value          = 0;
            } else {
                // SELF ROUTE -- UNREACHABLE from Begin(): SetInputs is called
                // ONLY with a foe target. The forced self-cast (SPEC-self-cast-
                // forced) uses SetSelfSpell() on the SEPARATE MFO_CastPackageSelf
                // record instead, whose t6 target is authored STATICALLY (the
                // DLL never writes a targType at runtime for self). Kept only as
                // defence in depth should a future caller bypass that.
                targetTarget->targType      = kTargTypeSelf;
                targetTarget->target.object = nullptr;
                targetTarget->value         = 0;
            }
            return true;
        }

        // FORCED SELF-CAST: point MFO_CastPackageSelf's Spell input at the chosen
        // spell, and NOTHING ELSE. The Target input stays as the ESP authored it
        // -- targType 6 (self), no QNAM -- so at runtime the DLL never writes a
        // targType into a self record (writing t6 into a QNAM-carrying record
        // was the rev-4 crash; §0.22 proved the static t6/no-QNAM shape clean).
        // Returns false without writing if the input fails its layout guard.
        bool SetSelfSpell(RE::SpellItem* a_spell) {
            auto* pkg = Forms::g_castPackageSelf;
            if (!pkg) return false;
            auto* spellPd = FindInput(pkg, kInputSpell);
            if (!spellPd) return false;
            auto* spellTarget = ReadTarget(spellPd, "Spell(self)");
            if (!spellTarget) return false;
            spellTarget->targType      = kTargTypeObjectID;
            spellTarget->target.object = a_spell;
            spellTarget->value         = 0;
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
            // The holder's OWN carrier alias -- 0 (foe) or 2 (self). Called only
            // for the live holder (Pump), so g_holder.self is authoritative.
            quest->CreateRefHandleByAliasID(handle, HolderActorAlias());
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
            auto* pkg = HolderPackage();
            if (!a_actor || !pkg) return false;
            return a_actor->CheckForCurrentAliasPackage() == pkg;
        }

        bool PackageRunning(RE::Actor* a_actor) {
            auto* pkg = HolderPackage();
            if (!a_actor || !pkg) return false;
            return a_actor->GetCurrentPackage() == pkg;
        }

        void ResetHolder() {
            g_holder = Holder{};
            g_liveStream.store(0, std::memory_order_relaxed);
        }

        // Defined below with the eviction-marker machinery (#48); declared here
        // because the cast release now evicts too.
        RE::TESObjectREFR* EvictMarkerRef();

        // RELEASE the cast alias. Safe to call in any phase.
        //
        // BY EVICTION, exactly like LootTravelClear/RetreatClear (#48/#73):
        // force-fill alias 0 with the non-actor XMarker so the follower's alias
        // instance is REPLACED and the framework reclaims him. The old VM
        // Clear() stays only as the off-AE / marker-less fallback -- MFO's
        // aliases carry no script, which is exactly why the loot quest's VM
        // Clear silently failed (v0.8.2 lesson, Config.h bLootTravel note), so
        // it cannot be the primary release for a package that now fires every
        // missed cast. NEVER the player: alias 0 carries the UseMagic ALPC,
        // and an actor parked in a package-carrying alias is the furniture-
        // ejection bug (v1.0.25/26).
        void ClearAlias(const char* a_why) {
            const auto id    = g_holder.actorID;
            auto*      quest = Forms::g_commandQuest;
            auto*      mark  = EvictMarkerRef();   // marker or null -- no player fallback here

            const std::uint32_t actorAlias = HolderActorAlias();   // 0 foe / 2 self
            bool released = false;
            if (quest && mark && ForceRefToNative(quest, actorAlias, mark)) {
                spdlog::info("[pkg] {:08X}: released ({}) -- evicted (marker){}", id, a_why,
                             g_holder.self ? " [self]" : "");
                released = true;
            } else if (DispatchAlias("Clear", nullptr)) {
                spdlog::info("[pkg] {:08X}: released ({}) -- VM Clear (no marker/off-AE)",
                             id, a_why);
                released = true;
            }
            if (!released) {
                // The fill is SERIALIZED INTO THE SAVE. A release we could not
                // perform is not a cosmetic miss -- it is a follower who comes
                // back latched on every subsequent load. The ReleaseAll load
                // sweep is the self-heal.
                spdlog::error("[pkg] {:08X}: RELEASE FAILED on both routes ({}) -- the alias "
                              "fill persists in the save; the load sweep will evict it", id, a_why);
            }
            // Measured readback, same idiom as every loot/retreat release:
            // "the alias now holds the marker" is not proof the follower
            // detached. Re-evaluate so the framework reclaims him THIS tick.
            if (auto* actor = HolderActor()) {
                actor->EvaluatePackage(true, false);   // never resetAI -- combat group
                // CONCENTRATION: CUT THE STREAM. Eviction re-evaluates the
                // package away, but a channel the graph already opened can
                // outlive the package by a beat -- and a beam that lingers
                // even one beat past "teammate crossed the line" is the
                // friendly fire this hold exists to prevent. InterruptCast on
                // all three plausible sources (the package's casting source
                // is not observable); no refund -- package casts spend
                // nothing (§0.22). Same worker-side call Actuation's
                // bDriveCaster probe ships.
                if (g_holder.hold.holdSeconds > 0.0f) {
                    using CS = RE::MagicSystem::CastingSource;
                    for (const auto src : { CS::kLeftHand, CS::kRightHand, CS::kInstant }) {
                        if (auto* mc = actor->GetMagicCaster(src)) mc->InterruptCast(false);
                    }
                    spdlog::info("[pkg] {:08X}: concentration stream interrupted ({})",
                                 id, a_why);
                }
                VerifyDetachedFrom(quest, actor, "pkg", "MFO_CommandQuest", a_why);
            }
            ResetHolder();
        }

        Decline Begin(RE::Actor* a_follower, RE::SpellItem* a_spell,
                      RE::TESObjectREFR* a_target, const CastHold& a_hold) {
            if (!Config::g_usePackages.load()) return Decline::Disabled;
            if (!a_follower || !a_spell)       return Decline::NoRecord;

            const bool self = (a_target == nullptr);

            // FORCED SELF-CAST GATE (SPEC-self-cast-forced). Self is served by a
            // DEDICATED record (MFO_CastPackageSelf, command-quest alias 2):
            // authored targType 6, NO QNAM -- §0.22's proven-clean probe-6 shape,
            // the one that REVOKED #67. It is deliberately NOT the shipped
            // MFO_CastPackage: that carries a QNAM (foe target t4 -> alias 1),
            // and writing t6 into a QNAM-carrying record at runtime is the rev-4
            // crash cell. The route stays behind bCastSelf until the production
            // path (arbitrary self spell, in combat, via alias fill/evict) is
            // deck-confirmed; with the gate OFF (default) self declines exactly
            // as it always has and the caller falls back to the silent apply.
            if (self && !Config::g_castSelf.load()) {
                spdlog::debug("[pkg] {:08X}: cast_self declined -- self route gated OFF "
                              "(bCastSelf); silent fallback", a_follower->GetFormID());
                return Decline::SelfRoute;
            }

            auto* quest = Forms::g_commandQuest;
            auto* pkg   = self ? Forms::g_castPackageSelf : Forms::g_castPackage;
            if (!quest || !pkg)                return Decline::NoRecord;
            if (!VM() || !CommandAlias())      return Decline::NoVM;

            // The self route is native-only (AE): the VM fallback fills alias 0,
            // the FOE carrier -- wrong for self. The foe route is already
            // effectively AE-only (its alias-1 target fill is), so this loses
            // nothing. Off AE, self declines and the caller falls back.
            if (self && !REL::Module::IsAE()) return Decline::NoVM;

            const std::uint32_t actorAlias = self ? kAliasCommandSelfActor
                                                  : kAliasCommandActor;

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

            // INPUTS. Foe: point Spell (t1) AND Target (t4 -> alias 1). Self:
            // point ONLY the Spell on MFO_CastPackageSelf -- its t6 Target is
            // authored statically and MUST NOT be rewritten at runtime (that
            // write, into a QNAM-carrying record, was the rev-4 crash; the self
            // record carries no QNAM and we never touch its target).
            const bool inputsOk = self ? SetSelfSpell(a_spell)
                                       : SetInputs(a_spell, a_target);
            if (!inputsOk) return Decline::BadInputs;

            // FOE ROUTE: fill the TARGET alias (1) with the victim BEFORE the
            // actor alias, the same no-rooting order as loot/retreat -- the
            // package's t4 target must resolve when alias 0 instances it.
            // Native-only: alias 1 has no VM fallback, so off AE the foe route
            // declines and the caller falls back. Self skips this entirely --
            // t6 aims the caster at himself, no target alias.
            if (!self) {
                if (!REL::Module::IsAE()) return Decline::NoVM;
                if (!ForceRefToNative(quest, kAliasCommandTarget, a_target)) {
                    spdlog::error("[pkg] {:08X}: TARGET alias {} fill failed -- refusing to "
                                  "cast at a stale target", id, kAliasCommandTarget);
                    return Decline::NoVM;
                }
            }

            // Fill the holder's carrier alias (0 foe / 2 self). Native first --
            // synchronous, so the alias is filled when this returns. VM fallback
            // only for the FOE route on SE (self already returned NoVM off AE,
            // and the VM fallback fills alias 0 regardless of route).
            bool filled = ForceRefToNative(quest, actorAlias, a_follower);
            if (filled) {
                spdlog::info("[pkg] {:08X}: alias {} filled NATIVELY (synchronous){}",
                             id, actorAlias, self ? " [self]" : "");
            } else if (!self && DispatchAlias("ForceRefTo", a_follower)) {
                spdlog::info("[pkg] {:08X}: alias {} fill DISPATCHED via VM (async)",
                             id, actorAlias);
            } else {
                spdlog::error("[pkg] {:08X}: ForceRefTo failed on BOTH routes", id);
                return Decline::NoVM;
            }

            ResetHolder();
            g_holder.actorID  = id;
            g_holder.self     = self;
            g_holder.spellID  = a_spell->GetFormID();
            g_holder.targetID = a_target ? a_target->GetFormID() : 0;
            g_holder.hold     = a_hold;
            g_holder.startAt  = Clock::now();
            g_holder.phaseAt  = Clock::now();
            g_holder.phase    = Phase::Requested;
            if (a_hold.holdSeconds > 0.0f) {
                g_liveStream.store((static_cast<std::uint64_t>(id) << 32) |
                                       a_spell->GetFormID(),
                                   std::memory_order_relaxed);
            }
            ++g_requests;

            spdlog::info("[pkg] {:08X}: requested cast of {:08X} ({}){} -- fill DISPATCHED, "
                         "not yet observed", id, g_holder.spellID,
                         self ? "on self (alias 2)" : "at reference",
                         a_hold.holdSeconds > 0.0f
                             ? std::format(" [concentration hold {:.1f}s]", a_hold.holdSeconds)
                             : std::string());
            return Decline::None;
        }

        // ── OPTION A: loot-travel alias plumbing (DESIGN behaviour layer) ────
        // P7: kMaxLootSlots concurrent excursions. The loot quest's aliases are
        // INTERLEAVED per slot -- actor alias 2*slot (carries the slot's travel
        // package), target alias 2*slot+1 (the loot ref the PLDT t8 points at).
        // Slot 0 resolves to 0/1, byte-identical to the shipped single-slot route.
        constexpr std::uint32_t LootActorAlias(int a_slot) {
            return static_cast<std::uint32_t>(2 * a_slot);
        }
        constexpr std::uint32_t LootTargetAlias(int a_slot) {
            return static_cast<std::uint32_t>(2 * a_slot + 1);
        }

        // CLAIM MODEL: the loot quest ships at STATIC priority 60 (MFO_GenerateESP
        // .py QUEST_PRIORITY, above the follower framework's ~50). Engage = fill
        // alias 0 with the follower (claims him at 60 -> travels). Release = EVICT
        // him from alias 0 by force-filling the eviction marker (a non-actor:
        // never the player, #48 furniture-ejection). A runtime priority flip
        // does NOT re-arbitrate the claim (deck-proven, ENGINE_NOTES 0.35b), so
        // the number is never touched at runtime -- the alias occupancy is.

        RE::BGSRefAlias* LootAlias(std::uint32_t a_id) {
            auto* quest = Forms::g_lootQuest;
            if (!quest) return nullptr;
            for (auto* base : quest->aliases) {
                if (!base || base->aliasID != a_id) continue;
                return skyrim_cast<RE::BGSRefAlias*>(base);
            }
            return nullptr;
        }

        // ── RETREAT PROBE plumbing (see Packages.h) ─────────────────────────
        // Same aliases-by-convention as the loot quest: 0 = the follower
        // (carries MFO_RetreatPackage), 1 = the destination (the PLAYER).
        constexpr std::uint32_t kAliasRetreatActor  = 0;
        constexpr std::uint32_t kAliasRetreatTarget = 1;

        // Who is currently claimed by the retreat quest, and since when. The
        // Scheduler reads these to drive per-tick instrumentation; the alias
        // itself is the engine-authoritative state (this is a mirror, reset on
        // every clear / ReleaseAll / revert).
        struct RetreatHold {
            RE::FormID        actorID = 0;
            Clock::time_point startAt{};
            RE::NiPoint3      startPos{};   // where she was when the retreat began (movement proof)
        };
        RetreatHold g_retreatHold;

        // EVICTION MARKER (#48 furniture-ejection). Releasing a follower from a
        // package-carrying ACTOR alias means forcing some OTHER ref into that
        // alias to displace him. We used the PLAYER -- but the alias runs a
        // travel/flee PACKAGE, and forcing the player into a package alias makes
        // the engine pull him OUT OF FURNITURE (chairs, workbench, mining) to
        // run it. The Logistics "left leash" re-arm churn fired that eviction
        // ~1/sec (deck: 5x "evicted (left leash)" in 8s). Fix: displace with a
        // NON-ACTOR XMarker -- it releases the follower identically (he loses
        // the alias package and reverts to follow), but nothing runs a package
        // on a non-actor, so nothing is ejected. Minted once per session on the
        // MAIN THREAD (kPostLoadGame/kNewGame); force-persisted so the handle
        // survives cell unloads. EvictionRef() falls back to the player only if
        // the marker was never minted, so a follower is never left stranded on
        // the package.
        RE::ObjectRefHandle g_evictMarker{};

        RE::TESObjectREFR* EvictMarkerRef() {
            auto ptr = g_evictMarker.get();
            auto* m = ptr.get();
            // Handle-table indices are rebuilt per load, so a handle minted in a
            // PREVIOUS save can resolve to a different ref entirely. Only trust
            // it if it still resolves to our XMarker base (0x3B).
            if (m && m->GetBaseObject() && m->GetBaseObject()->GetFormID() == 0x3B)
                return m;
            return nullptr;
        }

        RE::TESObjectREFR* EvictionRef() {
            if (auto* m = EvictMarkerRef()) return m;
            return RE::PlayerCharacter::GetSingleton();
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
        case Decline::SelfRoute:    return "self route barred (QNAM+t6 unprobed)";
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

    // ⚠ DEAD FILL PATH (unreachable on the live path; kept, not deleted).
    // SPEC-self-cast-forced superseded the package self-route: self-casts now go
    // through Actuation::CastSelfDirect (direct effect apply, no alias/package).
    // Every live caller intercepts the self case BEFORE reaching a
    // `self ? CastSelf : CastAt` branch (CastOn's self fork + the concentration
    // self guard), so `self` is always false there and this FILL is never entered;
    // CastAt's `if(!a_target) return CastSelf(...)` is likewise never called with a
    // null target on the live path. LEFT IN PLACE deliberately: the frozen 0x835
    // MFO_CastPackageSelf record and the alias-2 sweep in ReleaseAll() are still
    // REQUIRED (old saves mid-self-stream re-latch alias 2 on load without it), and
    // churning the frozen FormID buys nothing. Do not wire this back without
    // re-proving the alias route actually TRIGGERS a self cast (it did not: it only
    // equips — deck 2026-08-17). Safe to remove wholesale only alongside the sweep.
    Decline CastSelf(RE::Actor* a_follower, RE::SpellItem* a_spell, const CastHold& a_hold) {
        const auto r = Begin(a_follower, a_spell, nullptr, a_hold);
        if (r != Decline::None) {
            ++g_declines;
            spdlog::debug("[pkg] {:08X}: declined -- {}",
                          a_follower ? a_follower->GetFormID() : 0u, DeclineName(r));
        }
        return r;
    }

    bool StreamLive(RE::FormID a_actor, RE::FormID a_spell) {
        if (!a_actor || !a_spell) return false;   // never match the empty mirror
        return g_liveStream.load(std::memory_order_relaxed) ==
               ((static_cast<std::uint64_t>(a_actor) << 32) | a_spell);
    }

    Decline CastAt(RE::Actor* a_follower, RE::SpellItem* a_spell,
                   RE::TESObjectREFR* a_target, const CastHold& a_hold) {
        if (!a_target) return CastSelf(a_follower, a_spell, a_hold);
        const auto r = Begin(a_follower, a_spell, a_target, a_hold);
        if (r != Decline::None) {
            ++g_declines;
            spdlog::debug("[pkg] {:08X}: declined -- {}",
                          a_follower ? a_follower->GetFormID() : 0u, DeclineName(r));
        }
        return r;
    }

    namespace {

        // The heal stream's release condition: the watched actor's health is
        // (near-)full. Runs on the same serialized queue as Pump; the reads
        // are the Actuation reserve-gate idiom (GetPermanentActorValue + the
        // temporary modifier = the live maximum). An unresolvable/dead watch
        // reads as TOPPED -- there is nothing left to heal, stop streaming.
        constexpr float kConcHealedFrac = 0.95f;
        bool HealWatchTopped(RE::FormID a_id) {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(a_id);
            if (!a || a->IsDead() || !a->Is3DLoaded()) return true;
            auto* avo = a->AsActorValueOwner();
            if (!avo) return true;
            const float cur = avo->GetActorValue(RE::ActorValue::kHealth);
            const float mx  = avo->GetPermanentActorValue(RE::ActorValue::kHealth) +
                              a->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary,
                                                       RE::ActorValue::kHealth);
            return mx <= 0.0f || cur >= kConcHealedFrac * mx;
        }

        // The pop's completion condition, HOLD-AWARE. Fire-and-forget
        // (holdSeconds == 0): one linger after the observed cast --
        // unchanged. Concentration (holdSeconds > 0): the stream is BOUNDED
        // -- release when the hold elapses, earlier when the watched heal
        // target tops off, earlier still when a teammate crosses the line of
        // fire. nullptr = keep holding. Always anchored on castSeen: the
        // clock starts when the STREAM is observed, not when it was asked
        // for, so a slow package start cannot eat the hold.
        const char* CastPopDue() {
            if (!g_holder.castSeen) return nullptr;
            if (g_holder.hold.holdSeconds <= 0.0f) {
                return Since(g_holder.castSeenAt) > kPostCastLinger
                           ? "cast observed -- releasing" : nullptr;
            }
            if (Since(g_holder.castSeenAt) > g_holder.hold.holdSeconds)
                return "concentration hold elapsed";
            if (g_holder.hold.healWatch != 0 && HealWatchTopped(g_holder.hold.healWatch))
                return "heal target topped off";
            if (g_holder.hold.ffWatch &&
                Sightline::TeammateInFireLine(g_holder.actorID, g_holder.targetID))
                return "teammate crossed the line of fire";
            return nullptr;
        }

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
            // The pop's completion condition can beat the Running observation:
            // the [cast] sink fires the instant the spell releases, while
            // GetCurrentPackage is only sampled here at tick cadence.
            // CastPopDue is the hold-aware form: linger for fire-and-forget,
            // the caller's bound + early-release watches for concentration.
            if (const char* pop = CastPopDue()) {
                SetPhase(Phase::Done, pop);
                ++g_completions;
            } else if (running) {
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
            // THE POP (§0.23's missing release): the cast we commanded was
            // OBSERVED -- one linger for the release animation, then hand the
            // follower back rather than letting the package's own Cooldown/
            // NumToCast keep him rooted for another cycle. The natural
            // "package no longer current" edge stays as the other half.
            // CastPopDue is hold-aware (concentration bounds, see above).
            if (const char* pop = CastPopDue()) {
                SetPhase(Phase::Done, pop);
                ++g_completions;
            } else if (!running) {
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

    void NotifyCast(RE::FormID a_actorID, RE::FormID a_spellID) {
        // The completion signal for the pop: the [cast] sink saw the holder
        // fire the commanded spell. Runs on the same serialized SKSE task
        // queue as Pump (both are AddTask drains), so no lock -- the same
        // single-writer discipline as every other g_holder touch.
        if (g_holder.phase == Phase::Idle || g_holder.phase == Phase::Done) return;
        if (g_holder.actorID != a_actorID || g_holder.spellID != a_spellID)  return;
        if (g_holder.castSeen) return;
        g_holder.castSeen   = true;
        g_holder.castSeenAt = Clock::now();
        // For a concentration hold this line announces the BOUND, not the
        // linger -- the field log must show the promised release up front.
        spdlog::info("[pkg] {:08X}: commanded cast OBSERVED ({:08X}) -- releasing in {:.1f}s{}",
                     a_actorID, a_spellID,
                     g_holder.hold.holdSeconds > 0.0f ? g_holder.hold.holdSeconds
                                                      : kPostCastLinger,
                     g_holder.hold.holdSeconds > 0.0f ? " (concentration hold, or earlier)"
                                                      : "");
    }

    void EnsureEvictMarker() {
        // MAIN THREAD ONLY (PlaceObjectAtMe mutates the cell); called from the
        // kPostLoadGame/kNewGame messaging handler, which is. One per session --
        // the once-guard makes the per-load call free.
        if (EvictMarkerRef()) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        auto* base = RE::TESForm::LookupByID<RE::TESObjectSTAT>(0x0000003B);   // vanilla XMarker
        if (!base) {
            spdlog::warn("[loot] XMarker 0x3B missing -- eviction falls back to player");
            return;
        }
        auto ref = player->PlaceObjectAtMe(base, true);   // force-persist: handle must survive cell unload
        if (ref) {
            g_evictMarker = ref->GetHandle();
            spdlog::info("[loot] eviction marker minted {:08X}", ref->GetFormID());
        }
    }

    // Defined below (the loot-clear readback); the load sweep uses it on the
    // PLAYER to prove the #48b un-latch actually stripped his alias packages.
    static void VerifyDetachedFrom(RE::TESQuest* a_quest, RE::Actor* a_follower,
                                   const char* a_tag, const char* a_questName,
                                   const char* a_why);

    void ReleaseAll(const char* a_why) {
        // OPTION A first, and unconditionally, and LOAD-BEARING. The loot quest is
        // STATIC priority 60 and the alias is engine-SERIALIZED, so a save made
        // mid-travel loads with the follower still filled into alias 0 -> claimed
        // at 60 -> stranded (walks to a stale corpse). Priority is static, so we
        // cannot "lower" our way out; EVICT -- force-fill alias 0 with the
        // eviction marker (never the player: #48, the alias package would yank
        // him out of furniture) -- to hand him back. Runs on kPreLoadGame /
        // post-load reconcile / kNewGame / revert, so this is the reset for
        // every in-session load.
        //
        // #48b: the PLAYER is swept too, and it is THE fix for saves written by
        // <= 1.0.24, whose release-by-eviction parked HIM in these package-
        // carrying aliases. The fill is engine-serialized, so he arrives every
        // session already latched -- carrying up to five travel/flee alias
        // packages -- and the old "held != player" guard skipped him FOREVER.
        // That standing latch is what broke furniture even with everything
        // else idle (deck 1.0.25: marker minted, zero evictions all session,
        // still ejected -- the ancient engine quirk needs only the latch, not
        // our churn). Displacing him requires a non-actor to displace WITH,
        // so the player is only evicted once the marker exists (post-load
        // reconcile runs after EnsureEvictMarker); a marker-less sweep keeps
        // the old skip rather than pointlessly filling player-with-player.
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* ev = EvictionRef();
        const bool haveMarker = ev && ev != player;   // a real non-actor to displace with
        bool sweptPlayer = false;
        if (auto* quest = Forms::g_lootQuest) {
            // P7: every slot's actor alias, not just slot 0 -- any of the four
            // could hold a follower (or the player, #48b) from an old save.
            for (int slot = 0; slot < kMaxLootSlots; ++slot) {
                RE::ObjectRefHandle h{};
                quest->CreateRefHandleByAliasID(h, LootActorAlias(slot));
                auto* held = h.get() ? h.get().get() : nullptr;
                // Evict any ACTOR occupant. Actor-ness, not identity, skips a
                // marker parked by a previous release (a PRIOR session's marker
                // is a different REFR than this session's).
                if (held && player && held->As<RE::Actor>() &&
                    (held != player || haveMarker)) {
                    ForceRefToNative(quest, LootActorAlias(slot), ev);
                    spdlog::info("[loot] {} -- loot alias (slot {}) held {:08X}{}; evicted (marker)",
                                 a_why, slot, held->GetFormID(),
                                 held == player ? " (the PLAYER, #48b)" : "");
                    if (held == player) sweptPlayer = true;
                }
            }
        }

        // RETREAT PROBE: same eviction on load, same reason -- the retreat alias
        // is engine-serialized, so a save made mid-retreat loads with the
        // follower still claimed at static 60 and marching to the player's
        // SAVED position through any fight in the way. (The retreat TARGET alias
        // also holds the player, but it carries no ALPC package -- inert, left
        // alone. Only the ACTOR alias latches.)
        if (auto* quest = Forms::g_retreatQuest) {
            RE::ObjectRefHandle h{};
            quest->CreateRefHandleByAliasID(h, kAliasRetreatActor);
            auto* held = h.get() ? h.get().get() : nullptr;
            if (held && player && held->As<RE::Actor>() &&
                (held != player || haveMarker)) {
                ForceRefToNative(quest, kAliasRetreatActor, ev);
                spdlog::info("[retreat] {} -- retreat alias held {:08X}{}; evicted (marker)",
                             a_why, held->GetFormID(),
                             held == player ? " (the PLAYER, #48b)" : "");
                if (held == player) sweptPlayer = true;
            }
        }
        g_retreatHold = RetreatHold{};

        // CAST-PACKAGE sweep (#73's second half, applied to the hybrid forced
        // cast): the command alias fill is engine-serialized like loot's and
        // retreat's, so a save written MID-CAST loads with the follower still
        // claimed at static 60 by a UseMagic alias package -- rooted, aimed at
        // a stale alias-1 target, on every subsequent load. The concentration
        // hold widens the filled window (up to kRunTimeout), so mid-stream
        // saves hit this sweep MORE OFTEN -- same handling, higher hit rate;
        // the in-session half (holder + StreamLive mirror) is cleared by the
        // phase!=Idle ClearAlias below via ResetHolder. This sweep runs
        // whether or not this session's holder mirror knows anything (a
        // PREVIOUS session's latch is exactly the case the mirror cannot see).
        // Evict any ACTOR occupant, the player included (#48b); the VM Clear
        // is the off-AE fallback only. BOTH carrier aliases are swept: alias 0
        // (foe cast) AND alias 2 (self cast, SPEC-self-cast-forced) -- the self
        // fill is engine-serialized exactly like the foe fill, so a save written
        // mid-self-stream latches alias 2 on every subsequent load unless it is
        // evicted here too.
        if (auto* quest = Forms::g_commandQuest) {
            for (const std::uint32_t aliasID : { kAliasCommandActor, kAliasCommandSelfActor }) {
                RE::ObjectRefHandle h{};
                quest->CreateRefHandleByAliasID(h, aliasID);
                auto* held = h.get() ? h.get().get() : nullptr;
                if (held && player && held->As<RE::Actor>() &&
                    (held != player || haveMarker)) {
                    if (!ForceRefToNative(quest, aliasID, ev))
                        DispatchAlias("Clear", nullptr);   // off-AE: scriptless-alias
                                                           // Clear may no-op; AE never
                                                           // reaches this arm
                    spdlog::info("[pkg] {} -- cast alias {} held {:08X}{}; evicted (marker)",
                                 a_why, aliasID, held->GetFormID(),
                                 held == player ? " (the PLAYER, #48b)" : "");
                    if (held == player) sweptPlayer = true;
                }
            }
        }

        // #48b READBACK: if the player was displaced, prove he actually lost
        // the alias package instances -- same measured-detach idiom as every
        // follower release. "The alias now holds the marker" is not that proof.
        if (sweptPlayer && player) {
            VerifyDetachedFrom(Forms::g_lootQuest, player, "loot", "MFO_LootQuest", "player sweep");
            VerifyDetachedFrom(Forms::g_retreatQuest, player, "retreat", "MFO_RetreatQuest", "player sweep");
            VerifyDetachedFrom(Forms::g_commandQuest, player, "pkg", "MFO_CommandQuest", "player sweep");
        }

        // The alias itself was swept above regardless of phase; ClearAlias here
        // is what resets the holder mirror and runs the measured readback on a
        // follower THIS session commanded.
        if (g_holder.phase != Phase::Idle) ClearAlias(a_why);
    }

    // ── OPTION A: loot travel ────────────────────────────────────────────────
    bool LootTravelFill(RE::Actor* a_follower, RE::TESObjectREFR* a_ref, int a_slot) {
        if (!Config::g_lootTravel.load())          return false;
        if (a_slot < 0 || a_slot >= kMaxLootSlots) return false;
        auto* quest = Forms::g_lootQuest;
        if (!a_follower || !a_ref || !quest)       return false;
        // The native ForceRefTo id is AE-only (see ForceRefToNative). Off AE we
        // simply do not offer travel -- the caller falls back to arm's-reach.
        if (!REL::Module::IsAE())                  return false;
        if (!quest->IsRunning()) {
            spdlog::error("[loot] MFO_LootQuest {:08X} is NOT RUNNING -- Data/SEQ/MFO.seq "
                          "missing or stale? Travel unavailable.", quest->GetFormID());
            return false;
        }
        // Fill the TARGET first, then the ACTOR: when the follower's alias
        // instances the travel package, its PLDT destination (alias 1) is
        // already resolved. Both are synchronous native fills.
        // The loot quest ships at STATIC priority 60 (> the follower framework's
        // 50). Filling alias 0 with the follower NOW, while priority is already
        // 60, is what claims him: the engine locks in an actor's owning quest at
        // ALIAS-FILL time (deck WALK diagnostic: a RUNTIME raise to 60 read
        // prio=60 but left him on PlayerFollowerPackage -- the raise never
        // re-arbitrated). TARGET (alias 1) first so the destination is resolved
        // when alias 0 instances the package -- no claimed-with-no-destination
        // (rooting) window.
        if (!ForceRefToNative(quest, LootTargetAlias(a_slot), a_ref)) return false;
        if (!ForceRefToNative(quest, LootActorAlias(a_slot), a_follower)) {
            LootTravelClear("actor fill failed", nullptr, a_slot);
            return false;
        }
        // Nudge the follower to evaluate now rather than wait for the engine's
        // own pass. (true, false): NEVER resetAI -- it clears the combat group.
        a_follower->EvaluatePackage(true, false);
        spdlog::info("[loot] {:08X}: travel dispatched to {:08X} (slot {}, quest prio={})",
                     a_follower->GetFormID(), a_ref->GetFormID(), a_slot,
                     static_cast<int>(quest->data.priority));
        return true;
    }

    bool LootTravelRetarget(RE::Actor* a_follower, RE::TESObjectREFR* a_ref, int a_slot) {
        // A LEG BOUNDARY inside an ongoing excursion: point the follower at the
        // NEXT loot ref WITHOUT releasing. Only alias 1 (the destination) is
        // refilled; alias 0 (the follower) is left ALONE -- so he stays claimed
        // (the quest's static 60) and the engine never hands him back between
        // corpses -- no turn-around. This is the batch's whole point. Replacement-
        // while-filled is deck-proven (the v0.8.2/3 churn logs re-targeted alias 1
        // corpse-to-corpse); EvaluatePackage re-plans the running Travel package
        // to the new destination. A movement change, not a loot mutation.
        if (!Config::g_lootTravel.load())          return false;
        if (a_slot < 0 || a_slot >= kMaxLootSlots) return false;
        auto* quest = Forms::g_lootQuest;
        if (!a_follower || !a_ref || !quest)       return false;
        if (!REL::Module::IsAE())                  return false;
        if (!quest->IsRunning())                   return false;
        if (!ForceRefToNative(quest, LootTargetAlias(a_slot), a_ref)) return false;
        a_follower->EvaluatePackage(true, false);
        spdlog::info("[loot] {:08X}: leg -> {:08X} (excursion, slot {}, prio={})",
                     a_follower->GetFormID(), a_ref->GetFormID(), a_slot,
                     static_cast<int>(quest->data.priority));
        return true;
    }

    // READBACK for the one unmeasured sub-step of the whole claim model: does the
    // ex-actor actually LOSE his MFO_LootQuest alias instance when we evict him
    // (fill alias 0 with the eviction marker)? The quest-side "alias holds the
    // marker" is NOT proof the follower detached. Walk his ExtraAliasInstanceArray (same
    // reader as ForeignOwnerBlocks) -- if an MFO_LootQuest package remains, the
    // eviction did NOT free him, and "released" is a lie. Run on EVERY release
    // now (Clear is the hot path), so the first deck run settles it either way.
    // Generic over the owning quest so the retreat probe's clear runs the SAME
    // measured readback as loot's (identical claim model, identical proof).
    static void VerifyDetachedFrom(RE::TESQuest* a_quest, RE::Actor* a_follower,
                                   const char* a_tag, const char* a_questName,
                                   const char* a_why) {
        if (!a_follower || !a_quest) return;
        bool stillClaimed = false;
        if (auto* arr = a_follower->extraList.GetByType<RE::ExtraAliasInstanceArray>()) {
            RE::BSReadLockGuard guard(arr->lock);
            for (auto* inst : arr->aliases) {
                if (inst && inst->quest == a_quest &&
                    inst->instancedPackages && !inst->instancedPackages->empty()) {
                    stillClaimed = true;
                    break;
                }
            }
        }
        if (stillClaimed)
            spdlog::error("[{}] EVICT INCOMPLETE ({}) -- {:08X} STILL carries an "
                          "{} alias package after displace", a_tag, a_why,
                          a_follower->GetFormID(), a_questName);
        else
            spdlog::info("[{}] {:08X} detached from {} alias ({})",
                         a_tag, a_follower->GetFormID(), a_tag, a_why);
    }

    void VerifyDetached(RE::Actor* a_follower, const char* a_why) {
        VerifyDetachedFrom(Forms::g_lootQuest, a_follower, "loot", "MFO_LootQuest", a_why);
    }

    void LootTravelClear(const char* a_why, RE::Actor* a_follower, int a_slot) {
        // RELEASE BY EVICTION. The loot quest is STATIC priority 60 (> the
        // framework's 50), so a filled alias 0 ALWAYS claims the follower -- and
        // dropping the number does NOT un-claim him (the engine locks the owning
        // quest at ALIAS-FILL time; the runtime raise/drop never re-arbitrates --
        // deck WALK diagnostic, ENGINE_NOTES 0.36). So to release, EVICT him:
        // force-fill alias 0 with the eviction MARKER -- a real ForceRefTo
        // (works; the null clear is a no-op, 0.34) -- which replaces the
        // follower's alias instance so the framework reclaims him and he
        // follows. A non-actor never runs the alias package, so occupying the
        // slot is inert (#48: the player here got yanked out of furniture); the
        // next dispatch replaces the marker.
        if (a_slot < 0 || a_slot >= kMaxLootSlots) return;
        auto* quest = Forms::g_lootQuest;
        if (!quest) return;
        if (auto* ev = EvictionRef())
            ForceRefToNative(quest, LootActorAlias(a_slot), ev);   // no-op off AE, fine
        // Re-evaluate NOW so the framework reclaims him this tick, not on the
        // engine's slow pass. (true,false) -- never resetAI. Without the follower
        // here the eviction still frees him on the next engine evaluation.
        if (a_follower) {
            a_follower->EvaluatePackage(true, false);
            VerifyDetached(a_follower, a_why);   // prove the eviction actually freed him
        }
        spdlog::info("[loot] travel released ({}) -- slot {} evicted (marker)", a_why, a_slot);
    }

    void LootTravelEvictIf(RE::FormID a_id) {
        // Evict a_id from whichever slot's ACTOR alias he currently occupies, keyed
        // to alias OCCUPANCY across ALL slots (not the caller's intent map -- the
        // alias is never emptied, so occupancy is the authoritative state). With
        // release-by-eviction (LootTravelClear) a slot normally holds the MARKER
        // between excursions, so this now matters for ONE case: a follower dismissed
        // DURING an excursion, before Clear ran. If he is still in his actor alias
        // when he leaves the roster, his framework claim is gone, so MFO's static-60
        // claim is his sole one -- he'd walk to the stale corpse and re-latch on
        // every load. Evict him (fill the marker -- a real ForceRefTo; the null
        // clear is a no-op, 0.34) so he detaches. Scans every slot because a
        // dismissed follower could hold any of the four.
        auto* quest = Forms::g_lootQuest;
        if (!quest) return;
        for (int slot = 0; slot < kMaxLootSlots; ++slot) {
            RE::ObjectRefHandle h{};
            quest->CreateRefHandleByAliasID(h, LootActorAlias(slot));
            auto* held = h.get() ? h.get().get() : nullptr;
            if (!held || held->GetFormID() != a_id) continue;   // a_id is not in this slot

            if (auto* ev = EvictionRef())
                ForceRefToNative(quest, LootActorAlias(slot), ev);   // no-op off AE, fine
            if (auto* actor = held->As<RE::Actor>())
                VerifyDetached(actor, "dismissed");   // prove the ex-actor detached
        }
    }

    // ── RETREAT PROBE (see Packages.h) ───────────────────────────────────────
    // Cloned from LootTravelFill: the SAME claim model on the SAME machinery,
    // differing only in which quest/records and in that alias 1 is the PLAYER.
    bool RetreatFill(RE::Actor* a_follower) {
        auto* quest = Forms::g_retreatQuest;
        if (!a_follower || !quest || !Forms::g_retreatPackage) return false;
        // The native ForceRefTo id is AE-only (see ForceRefToNative). Off AE
        // the probe simply never runs -- no VM fallback, no partial data.
        if (!REL::Module::IsAE())                  return false;
        if (!quest->IsRunning()) {
            spdlog::error("[retreat] MFO_RetreatQuest {:08X} is NOT RUNNING -- Data/SEQ/MFO.seq "
                          "missing or stale? Probe unavailable.", quest->GetFormID());
            return false;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;
        // ONE retreat at a time -- one quest, one alias pair, same single-holder
        // shape as loot travel. A second fill would silently evict the first
        // mid-measurement and corrupt both data points.
        if (g_retreatHold.actorID && g_retreatHold.actorID != a_follower->GetFormID()) {
            spdlog::debug("[retreat] {:08X}: declined -- alias held by {:08X}",
                          a_follower->GetFormID(), g_retreatHold.actorID);
            return false;
        }
        // Fill the TARGET first, then the ACTOR -- same no-rooting order as
        // loot: the destination (the player) is resolved before alias 0
        // instances the travel package. Both fills synchronous native.
        if (!ForceRefToNative(quest, kAliasRetreatTarget, player)) return false;
        if (!ForceRefToNative(quest, kAliasRetreatActor, a_follower)) {
            RetreatClear("actor fill failed");
            return false;
        }
        // DISENGAGE so the travel actually MOVES her. Field (Auri, v1.0.22): the
        // retreat package went active (took=true) but she never relocated -- an
        // actor with a live combat target keeps fighting, and a kIgnoreCombat Travel
        // package only lets the package RUN, it does not out-compete the combat
        // behaviour driving her. StopCombat() clears HER target (not the group, so
        // NOT resetAI) -> the travel wins; if foes re-aggro, kIgnoreCombat sustains
        // the walk. (The old "0.36 reliable" read took=true + dPlayer closing as
        // success, but dPlayer also closes when the PLAYER approaches -- it never
        // proved she moved.) Called each retreat tick in the Scheduler too, since
        // hostiles re-initiate combat.
        a_follower->StopCombat();
        // Static 60 is already in the record; the fill IS the claim (0.36).
        // (true, false): NEVER resetAI -- it clears the combat group.
        a_follower->EvaluatePackage(true, false);
        g_retreatHold.actorID = a_follower->GetFormID();
        g_retreatHold.startAt = Clock::now();
        g_retreatHold.startPos = a_follower->GetPosition();   // measure HER movement, not dPlayer
        spdlog::info("[retreat] {:08X}: dispatched + StopCombat (prio={}, kIgnoreCombat)",
                     a_follower->GetFormID(), static_cast<int>(quest->data.priority));
        return true;
    }

    void RetreatClear(const char* a_why, RE::Actor* a_follower) {
        // RELEASE BY EVICTION, verbatim from LootTravelClear: force-fill alias 0
        // with the eviction MARKER (a real ForceRefTo -- the null clear is a
        // no-op, 0.34; a priority flip never re-arbitrates, 0.36; never the
        // player, #48 -- the flee package would yank him out of furniture),
        // then prove the detach with the same ExtraAliasInstanceArray readback.
        // NEVER a VM Clear.
        auto* quest = Forms::g_retreatQuest;
        if (!quest) return;
        if (auto* ev = EvictionRef())
            ForceRefToNative(quest, kAliasRetreatActor, ev);   // no-op off AE, fine
        if (a_follower) {
            a_follower->EvaluatePackage(true, false);   // reclaim THIS tick; never resetAI
            VerifyDetachedFrom(quest, a_follower, "retreat", "MFO_RetreatQuest", a_why);
        }
        g_retreatHold = RetreatHold{};
        spdlog::info("[retreat] travel released ({}) -- evicted (marker)", a_why);
    }

    RE::FormID RetreatHolder() { return g_retreatHold.actorID; }
    float      RetreatSeconds() { return Since(g_retreatHold.startAt); }
    RE::NiPoint3 RetreatStartPos() { return g_retreatHold.startPos; }

    void RetreatEvictIf(RE::FormID a_id) {
        // The dismissal-path twin of LootTravelEvictIf: keyed to alias
        // OCCUPANCY, no-op unless a_id is in the slot. A follower dismissed
        // mid-retreat has no framework claim left, so MFO's static-60 claim is
        // his sole one -- evict (fill the marker; the null clear is a no-op,
        // 0.34) so he detaches instead of re-latching on every load.
        auto* quest = Forms::g_retreatQuest;
        if (!quest) return;
        RE::ObjectRefHandle h{};
        quest->CreateRefHandleByAliasID(h, kAliasRetreatActor);
        auto* held = h.get() ? h.get().get() : nullptr;
        if (!held || held->GetFormID() != a_id) return;

        if (auto* ev = EvictionRef())
            ForceRefToNative(quest, kAliasRetreatActor, ev);   // no-op off AE, fine
        if (auto* actor = held->As<RE::Actor>())
            VerifyDetachedFrom(quest, actor, "retreat", "MFO_RetreatQuest", "dismissed");
        if (g_retreatHold.actorID == a_id) g_retreatHold = RetreatHold{};
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
        g_retreatHold = RetreatHold{};
        g_requests    = 0;
        g_completions = 0;
        g_declines    = 0;
        g_timeouts    = 0;
    }

}
