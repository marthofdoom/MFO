#pragma once
// tools/mcls_byte_test/stub/RE/Skyrim.h -- HOST STAND-IN for CommonLib's RE/Skyrim.h, on the include path for ONE
// purpose: to compile the real native/classes/State.cpp (the 'MCLS' co-save codec) with the host compiler. Only the
// names the real headers on that path mention exist (native/PCH.h, State.h, Config.h, Vocabulary.h, CombatStyle.h,
// Serialization.h, classes/Classes.h). Nothing here is behaviour the test relies on.
#include <cstdint>

namespace RE {
    using FormID = std::uint32_t;
    enum class ActorValue : std::int32_t { kNone = -1, kHealth = 24, kMagicka = 25, kStamina = 26 };
    enum class ACTOR_VALUE_MODIFIER : std::uint32_t { kPermanent = 0, kTemporary = 1, kDamage = 2 };
    struct ActorValueOwner {
        float GetActorValue(ActorValue) { return 0.0f; }
        float GetPermanentActorValue(ActorValue) { return 0.0f; }
    };
    class Actor {
    public:
        ActorValueOwner* AsActorValueOwner() { return nullptr; }
        float GetActorValueModifier(ACTOR_VALUE_MODIFIER, ActorValue) { return 0.0f; }
    };
    class CombatController;
    namespace MagicSystem {
        enum class SpellType : std::uint32_t { kSpell = 0 };
        enum class CastingType : std::uint32_t { kConstantEffect = 0, kFireAndForget = 1 };
    }
    class SpellItem {
    public:
        MagicSystem::SpellType   GetSpellType() const { return MagicSystem::SpellType::kSpell; }
        MagicSystem::CastingType GetCastingType() const { return MagicSystem::CastingType::kFireAndForget; }
    };
}
