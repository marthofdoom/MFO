// logistics/PickupSound.cpp -- MFO-caused follower pickups sound AT THE FOLLOWER (batch A, ClickUp 86e3haxqx).
// A new engine seat, so its own file in the logistics subsystem (CLAUDE.md "a new mechanism is a new file").
//
// THE ROOT CAUSE (disassembly of 1.6.1170, 1.5.97 and 1.7.104; Docs/ENGINE_NOTES.md section 0.50).
// MFO's loose-item pickup is `TESObjectREFR::ActivateRef(follower)` on the main thread (logistics/Service.cpp,
// the route-2b ARRIVAL). The engine routes it TESBoundObject::Activate (vslot 0x37) -> the follower's
// Actor::PickUpObject (vslot 0xCC) -> `this->PlayPickUpSound(base, pickup=1, use=0)` (vslot 0xA3). For an
// NPC that is Actor::PlayPickUpSound (1.6.1170 0x65FE90, id 37196): it picks the item's pickup SNDR,
// builds it with flags 0x11 (mode 1 = 3D), sets the position to the NPC, follows its 3D and plays it. So
// the engine DOES place the sound at the follower. What makes it sound like the player picked it up is the
// SNDR's OUTPUT MODEL: every vanilla item pickup sound uses SOMMono01400Player1st (000B4058: NAM1 flags 0x02,
// NO "attenuates with distance" bit, no ANAM attenuation, defined speaker output) or SOMUIDefault (000B75FB,
// flags 0). Gold (ITMGoldUpSD 0003E952), armor, potions, keys, arrows, coin pouches, books and ingredients
// all play at full volume at any distance -- heard exactly like the player's own pickup (the player's
// PlayerCharacter::PlayPickUpSound, 0x743EB0, builds the same SNDRs 2D with flags 0x1A).
//
// THE SEAT. A CALL-SITE trampoline on the one `BSAudioManager::BuildSoundDataFromDescriptor` call inside
// Actor::PlayPickUpSound (+0x14A on 1.6.1170 and 1.7.104, +0x73 on 1.5.97). The thunk calls the engine's
// build unchanged and, ONLY while MFO's own ActivateRef for a managed follower is on this thread's stack
// (Scope below), replaces the built handle's output model with SOMMono01400 (Skyrim.esm 0005A28A: attenuates
// with distance, HRTF, 150..1400 units -- the positional object model of NPCHumanWoodPickup, the same model
// marth chose for the lockpick SNDR copies) when the SNDR's own model does not attenuate. The engine then
// positions the handle at the follower, follows his 3D and plays it, exactly as before. Composition, not
// substitution (principle 3): the engine still picks the sound, positions it and plays it; only the one
// facet that made it heard as the player's (the non-spatial output model) is denied, by the engine's own
// mechanism -- BSSoundHandle::SetOutputModel between build and play is what the engine itself does in its
// UI sound helper (1.6.1170 0x97AD20) and its actor sound paths.
//
// SCOPE. Every other caller of Actor::PlayPickUpSound (any NPC's own pickups or drops, other mods' pickups,
// the job worker's container transfers) passes straight through: the scope is a thread_local set only on the
// main thread around MFO's ActivateRef. The player's own pickups never reach this site (PlayerCharacter
// overrides vslot 0xA3).
//
// THREADING. Install: plugin load (SKSEPluginLoad), before any game thread runs, like Board's input
// trampoline. The thunk runs on whatever thread calls Actor::PlayPickUpSound; outside the scope it only reads
// a thread_local and forwards. Inside the scope it runs on the MAIN thread (MainThread::Post's lambda in
// Service.cpp), so the form lookup and SetOutputModel run on main.
//
// GATES. Exact builds only (Runtime::Known). Three verified rows (native/VerifiedAddresses.h): the function
// (PickupSound.Actor.PlayPickUpSound on 1.6.1170 / 1.5.97; on 1.7.104, whose MIT id table has no id 37196,
// the raw-RVA row PickupSound.Actor.PlayPickUpSound.1_7_104), BuildSoundDataFromDescriptor and
// SetOutputModel. The function is read from the Character vtable's slot 0xA3 (the vtable is a verified row),
// so no 1.7.104-absent id is ever resolved (an id miss is FATAL in the fork). Runtime checks before writing:
// the slot holds the verified function, the site holds E8, and its rel32 lands on BuildSoundDataFromDescriptor.
// Any failure refuses the seat by name and the pickup sound stays the engine's (principle 7: logged, no
// fallback, never installed anyway).
//
// SIGNATURES (principle 6, from the disassembly, not CommonLib): the call site passes rcx = BSAudioManager
// singleton, rdx = &BSSoundHandle (stack, 0xC bytes, soundID -1 / 0 / 0), r8 = BSISoundDescriptor* (the
// BGSSoundDescriptorForm + 0x20 subobject; every producer at this site is a BGSSoundDescriptorForm), r9d =
// 0x11; the return value is not read. SetOutputModel (1.6.1170 0xCAF660, id 67624) takes rcx = the handle and
// rdx = BSISoundOutputModel* = the BGSSoundOutput + 0x20 subobject (the engine passes `form + 0x20`;
// CommonLib's BSSoundHandle::SetOutputModel(const BGSSoundOutput*) would pass the form itself, so it is NOT
// used). BGSStandardSoundDef is the only BGSSoundDescriptor class on all three builds (one registered
// creator), outputModel at +0x48, BGSSoundOutput data.flags at +0x28 (descriptor fill, vslot 1).

#include "Logistics_internal.h"
#include "Runtime.h"   // Known(), SeatVerified(): the exact-version gate and the mit-3.7 F1 self-check

namespace MFO::Logistics::PickupSound {

    namespace {
        // Skyrim.esm SOMMono01400: attenuates with distance (NAM1 flags 0x01), HRTF, 150..1400 units.
        constexpr RE::FormID kPositionalModel = 0x0005A28A;

        constexpr std::size_t kSlotPlayPickUpSound = 0xA3;   // Actor/Character vslot (SE/AE; VR is refused)

        // The trampoline is this seat's own (Board.cpp owns SKSE::GetTrampoline(); a second
        // AllocTrampoline would release the block Board's stub lives in). Never freed: a thread still
        // inside the stub at process exit must not find it unmapped.
        SKSE::Trampoline* g_tramp = nullptr;
        std::atomic<bool> g_installed{ false };

        using SetOutputModelFn = void(RE::BSSoundHandle*, const RE::BSISoundOutputModel*);
        REL::Relocation<SetOutputModelFn>& SetOutputModelRel() {
            static REL::Relocation<SetOutputModelFn> func{ REL::RelocationID(66363, 67624) };
            return func;
        }

        // The MFO pickup in flight on THIS thread (main only; Scope sets it).
        struct InFlight {
            bool       active = false;
            bool       flora = false;
            RE::FormID follower = 0;
            RE::FormID ref = 0;
            int        hits = 0;
        };
        thread_local InFlight t_scope;

        struct BuildCall {
            static bool thunk(RE::BSAudioManager* a_mgr, RE::BSSoundHandle& a_handle,
                              RE::BSISoundDescriptor* a_desc, std::uint32_t a_flags) {
                const bool built = func(a_mgr, a_handle, a_desc, a_flags);
                auto& sc = t_scope;
                if (!sc.active) return built;   // every caller but MFO's own pickup: untouched
                ++sc.hits;
                auto* form = a_desc ? static_cast<RE::BGSSoundDescriptorForm*>(a_desc) : nullptr;
                const RE::FormID sndID = form ? form->GetFormID() : 0;
                if (a_handle.soundID == RE::BSSoundHandle::kInvalidID) {
                    spdlog::warn("[pickupsnd] {:08X}: pickup sound {:08X} for ref {:08X} did not build (flags 0x{:X}) "
                                 "-- nothing to place", sc.follower, sndID, sc.ref, a_flags);
                    return built;
                }
                auto* def   = form ? static_cast<RE::BGSStandardSoundDef*>(form->soundDescriptor) : nullptr;
                auto* model = def ? def->outputModel : nullptr;
                const RE::FormID modelID = model ? model->GetFormID() : 0;
                if (model && model->data.flags.any(RE::BGSSoundOutput::Data::Flag::kAttenuatesWithDistance)) {
                    spdlog::info("[pickupsnd] {:08X}: pickup sound {:08X} for ref {:08X} -- its output model {:08X} "
                                 "already attenuates with distance; left as is (positional at the follower)",
                                 sc.follower, sndID, sc.ref, modelID);
                    return built;
                }
                static RE::BGSSoundOutput* s_positional = RE::TESForm::LookupByID<RE::BGSSoundOutput>(kPositionalModel);
                if (!s_positional) {
                    static bool s_logged = false;
                    if (!s_logged) {
                        s_logged = true;
                        spdlog::error("[pickupsnd] Skyrim.esm SOMMono01400 ({:08X}) is not a loaded sound output model "
                                      "-- follower pickup sounds keep the engine's non-positional model",
                                      kPositionalModel);
                    }
                    return built;
                }
                SetOutputModelRel()(&a_handle, static_cast<const RE::BSISoundOutputModel*>(s_positional));
                spdlog::info("[pickupsnd] {:08X}: pickup sound {:08X} for ref {:08X} -- output model {:08X} does not "
                             "attenuate (heard as the player's pickup); replaced with SOMMono01400 {:08X}, positional "
                             "at the follower",
                             sc.follower, sndID, sc.ref, modelID, kPositionalModel);
                return built;
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

        std::uintptr_t Rel32Target(std::uintptr_t a_site) {
            return a_site + 5 + static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(a_site + 1));
        }
    }

    Scope::Scope(RE::FormID a_follower, RE::FormID a_ref, bool a_flora) {
        if (!g_installed.load(std::memory_order_acquire) || t_scope.active) return;
        t_scope = InFlight{ true, a_flora, a_follower, a_ref, 0 };
        owner_ = true;
    }

    Scope::~Scope() {
        if (!owner_) return;
        const InFlight sc = t_scope;
        t_scope = InFlight{};
        if (sc.hits > 0) return;
        // No engine pickup sound ran inside this ActivateRef. Expected for a coin purse (the engine plays
        // the harvest sound only when the PLAYER harvests: TESProduceForm harvest, 1.6.1170 0x1E8EF0) and
        // for an activation that took nothing (the [acquire] readback next tick says TOOK or NO-OP).
        spdlog::info("[pickupsnd] {:08X}: ActivateRef on ref {:08X} ran no engine pickup sound{}",
                     sc.follower, sc.ref,
                     sc.flora ? " (coin purse: an NPC harvest plays none)"
                              : " (see the [acquire] readback: a TOOK here means a pickup path this seat does not see)");
    }

    void Install() {
        if (!Runtime::Known()) {
            spdlog::warn("[pickupsnd] runtime {} not supported ({}) -- seat NOT installed; follower pickup sounds "
                         "stay non-positional",
                         REL::Module::get().version().string("."), Runtime::GateReason());
            return;
        }
        // In-function offset of the BuildSoundDataFromDescriptor call, by EXACT build (rule 11).
        const std::uintptr_t off = Runtime::IsVerified1_5_97() ? 0x73 : 0x14A;   // 1.6.1170 and 1.7.104: 0x14A

        const REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_Character[0] };
        if (!Runtime::SeatVerified(vtbl.address(), "PickupSound.Character.PlayPickUpSoundSlot")) {
            spdlog::error("[pickupsnd] seat NOT installed (self-check refused the Character vtable)");
            return;
        }
        const auto fn = *reinterpret_cast<const std::uintptr_t*>(vtbl.address() + 8 * kSlotPlayPickUpSound);
        const char* fnSeat = Runtime::IsVerified1_7_104() ? "PickupSound.Actor.PlayPickUpSound.1_7_104"
                                                          : "PickupSound.Actor.PlayPickUpSound";
        if (!Runtime::SeatVerified(fn, fnSeat)) {
            spdlog::error("[pickupsnd] seat NOT installed: Character vtable slot 0x{:X} holds 0x{:X}, not the verified "
                          "Actor::PlayPickUpSound (another plugin hooked the slot?)",
                          kSlotPlayPickUpSound, fn);
            return;
        }
        const REL::Relocation<std::uintptr_t> build{ REL::RelocationID(66404, 67666) };
        if (!Runtime::SeatVerified(build.address(), "PickupSound.BSAudioManager.BuildSoundDataFromDescriptor") ||
            !Runtime::SeatVerified(SetOutputModelRel().address(), "PickupSound.BSSoundHandle.SetOutputModel")) {
            spdlog::error("[pickupsnd] seat NOT installed (self-check refused BuildSoundDataFromDescriptor / "
                          "SetOutputModel)");
            return;
        }
        const std::uintptr_t site = fn + off;
        const auto op = *reinterpret_cast<const std::uint8_t*>(site);
        if (op != 0xE8 || Rel32Target(site) != build.address()) {
            spdlog::error("[pickupsnd] seat NOT installed: Actor::PlayPickUpSound+0x{:X} holds opcode {:02X}{} "
                          "(another plugin patched it?)",
                          off, op,
                          op == 0xE8 ? std::format(" calling 0x{:X}, not BuildSoundDataFromDescriptor 0x{:X}",
                                                   Rel32Target(site), build.address())
                                     : std::string{});
            return;
        }
        g_tramp = new SKSE::Trampoline("MFO.PickupSound");
        g_tramp->create(64);
        BuildCall::func = g_tramp->write_call<5>(site, BuildCall::thunk);
        g_installed.store(true, std::memory_order_release);
        spdlog::info("[pickupsnd] seat installed: Actor::PlayPickUpSound (RVA 0x{:X}) +0x{:X} on {} -- pickups MFO "
                     "makes a follower do play positionally at him (non-attenuating models -> SOMMono01400)",
                     fn - REL::Module::get().base(), off, Runtime::BuildLabel());
    }
}
