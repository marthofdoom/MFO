// THE BUFF-POTION SELF-TEST (dev builds only; feat/buff-potions, ClickUp 1dfcuabd18f). Same rules as TestsT2.cpp: reads only inside step(), the
// runner adds the fixture potion and puts it back. One test:
//
//   buffpotion.resist_scores: the follower is given ONE Potion of Resist Fire (Skyrim.esm, a list-agnostic base form). The test waits until the
//   SpellBook mirror (the real main-thread measure: catalog-first classification, the live-effect read) shows that potion under the Resist Fire
//   kind, then runs the REAL generator and scorer on the book with a synthetic fight:
//     (a) a foe on him whose spell is fire        -> a DrinkBuff candidate for Resist Fire exists and scores above the act floor;
//     (b) the same foe with no element (a plain brawler) -> that candidate scores 0 (buff_threat vetoes: nothing to resist);
//     (c) the kind already live on him            -> it scores 0 (already_active gate).
//   The potion is never drunk and no foe is spawned: what is judged is the family's scoring of the real kit, not the drink road (the road is the
//   restore potions' own EquipObject road, MAP.md "DRINK ROAD").

#include "selftest/SelfTest_internal.h"
#include "iaus/Generate.h"
#include "iaus/SpellBook.h"
#include "ItemCatalog.h"
#include "logistics/PotionFx.h"

namespace MFO::SelfTest {

    namespace {

        constexpr RE::FormID kResistFire50Local = 0x0003EAE9;   // ALCH ResistFire50 (Skyrim.esm, verified 2026-11-12 with tools/esp_inspect.py)

        const char* BuffPre(Ctx& c) {
            const RE::FormID pot = VanillaForm(kResistFire50Local);
            auto*            alc = pot ? RE::TESForm::LookupByID<RE::AlchemyItem>(pot) : nullptr;
            if (!alc) return "Skyrim.esm ResistFire50 (0003EAE9) resolvable";
            c.s->set[0] = { pot };
            return nullptr;
        }
        void BuffFix(Ctx& c, FixtureRequest& r) { r.items.push_back({ c.s->set[0][0], 1 }); }

        // The Resist Fire candidate of a generated set, scored with the shared catalogue; -1 = no such candidate.
        float ResistFireScore(const Iaus::Snapshot& a_snap, const Iaus::ClassGate& a_gate) {
            const auto& t = Iaus::DefaultTuning();
            const auto  gen = Iaus::Generate(a_snap, a_gate, t);
            for (const auto& cand : gen.actions)
                if (cand.family == Iaus::Family::DrinkBuff && cand.f.buffKind == static_cast<std::uint8_t>(Iaus::BuffKind::ResistFire))
                    return Iaus::ScoreOne(cand, Iaus::Mind{}, Iaus::Profile{}, t, 3, 0.0f, nullptr);
            return -1.0f;
        }

        Step BuffStep(Ctx& c) {
            const RE::FormID pot = c.s->set[0][0];
            if (CountItem(c.follower, pot) < 1) {
                if (c.elapsed > 20.0f) return Step::Fail("the fixture Potion of Resist Fire never reached his pack");
                return Step::Continue();
            }
            const auto book = Iaus::SpellBook::Read(c.follower);   // posts a refresh when stale; the container sink dirties it on the fixture add
            const std::size_t fi = static_cast<std::size_t>(Iaus::BuffKind::ResistFire);
            if (!book || book->kit.buff[fi].count < 1) {
                if (c.elapsed > 30.0f)
                    return Step::Fail(std::format("the SpellBook never showed the potion under Resist Fire (book {}, count {})", book ? "read" : "none",
                                                  book ? book->kit.buff[fi].count : -1));
                return Step::Continue();
            }
            const Iaus::PotionBuff& b = book->kit.buff[fi];
            if (b.form != pot) return Step::Fail(std::format("Resist Fire's best potion is {:08X}, not the fixture {:08X}", b.form, pot));
            auto* alc = RE::TESForm::LookupByID<RE::AlchemyItem>(pot);
            const bool viaCatalog = Catalog::PotionEffects(pot) != nullptr;

            Iaus::Snapshot s;
            s.hasBook = true;
            s.book = *book;
            s.book.kit.buff[fi].active = false;   // judge the scoring, not whatever he has on at this moment
            s.playerId = 0x14;
            s.drawn = Iaus::PostureKind::Melee;
            s.meleeReach = 200.0f;
            s.combatSec = 0.0f;
            s.self.id = c.id; s.self.hp = s.self.hpMax = 100.0f; s.self.sp = s.self.spMax = 100.0f; s.self.mp = s.self.mpMax = 100.0f; s.self.level = 10;
            Iaus::Combatant foe;
            foe.id = 0x00000F01; foe.hp = foe.hpMax = 100.0f; foe.dist = 300.0f; foe.level = 10; foe.targetId = c.id;
            foe.caster = true; foe.incomingAv = 41;   // a fire mage on him (RE::ActorValue::kResistFire)
            s.foes.push_back(foe);
            const Iaus::ClassGate gate{};   // unresolved: nothing class-gated

            const float fire = ResistFireScore(s, gate);
            s.foes[0].caster = false; s.foes[0].incomingAv = -1;
            const float plain = ResistFireScore(s, gate);
            s.foes[0].caster = true; s.foes[0].incomingAv = 41;
            s.book.kit.buff[fi].active = true;
            const float live = ResistFireScore(s, gate);

            const float floor = Iaus::DefaultTuning().actFloor;
            const std::string ev = std::format("'{}' ({:08X}) classified by the {}: Resist Fire magnitude {:.0f} for {:.0f} s, {} carried; family drink_buff scores: fire foe {:.3f} "
                                               "(act floor {:.2f}), foe with no element {}, kind already live {}",
                                               alc && alc->GetName() ? alc->GetName() : "?", pot, viaCatalog ? "patcher catalog" : "runtime fallback", b.magnitude,
                                               b.duration, b.count, fire, floor, plain <= 0.0f ? "0/none" : std::format("{:.3f}", plain),
                                               live <= 0.0f ? "0/none" : std::format("{:.3f}", live));
            if (!(fire > floor)) return Step::Fail("the matching threat does not score above the act floor: " + ev);
            if (plain > 0.0f) return Step::Fail("a foe with no element still scores the resist potion: " + ev);
            if (live > 0.0f) return Step::Fail("a kind already live still scores: " + ev);
            return Step::Pass(ev);
        }

    }

    void RegisterBuffPotion() {
        Register({ "buffpotion.resist_scores", "1dfcuabd18f",
                   "(self-setup) one Potion of Resist Fire is added to his pack; the real SpellBook measure + Generate + ScoreOne run on a synthetic fire foe (no spawn, nothing is drunk)",
                   60.0f, BuffPre, BuffFix, BuffStep, nullptr });
    }

}
