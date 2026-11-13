// THE OFF-CLASS STORAGE SELF-TESTS (dev builds only; feat/offclass-storage, ClickUp 86e3t212u). Same rules as TestsT2.cpp: pure reads
// inside step(), no spawn, no engine mutation. Design section 9 (Docs/research/2026-11/offclass-storage-design.md), adjusted to the
// 2026-11-12 rulings (no errand stop, classes OFF = no storing, armor in scope, interim safe storage):
//
//   storage.decide_store     the pure store judge, table-driven over DecideStore.
//   storage.decide_retrieve  the pure retrieval judge, table-driven over DecideRetrieve.
//   storage.class_sets       PlannedFor over the real class table: mastered + future-in-line, unrestricted sets excluded.
//   storage.record_roundtrip the 'STOR' v1 codec on a CoSaveBuffer: identical round trip, 0xFF ids never written, flags masked,
//                            an implausible item count aborts and keeps what was parsed.
//   storage.chest            the MFO chest (MFO.esp 0x919) resolves, is PERSISTENT, enabled, and its base does not respawn.
//   storage.chest_roundtrip  review F1: a REAL round trip of one Iron Dagger (the runner's fixture) through the MFO chest by the live
//                            road (Papyrus::DispatchMoveItem both ways, each hop confirmed from his count), run by Storage::Tick
//                            while the runner's probe bit kProbeStorageMove arms it. The retrieval hop is the one whose receiver
//                            (the scriptless chest) needed the binding fix.

#include "selftest/SelfTest_internal.h"
#include "logistics/Storage.h"
#include "Serialization.h"
#include "Forms.h"

#include <format>

namespace MFO::SelfTest {

    namespace {
        namespace St = Logistics::Storage;
        using Classes::Id;
        namespace W = Classes::Weapon;

        // Skyrim.esm forms (load-order agnostic through VanillaForm). Verified 2026-11-13 with Linux-Native-Tools/tools/esp_query.py
        // on the Tuxborn Skyrim.esm.
        constexpr RE::FormID kIronSwordLocal      = 0x00012EB7;   // WEAP IronSword
        constexpr RE::FormID kIronArmorLocal      = 0x00012E49;   // ARMO ArmorIronCuirass
        constexpr RE::FormID kIronDaggerLocal     = 0x0001397E;   // WEAP IronDagger (the round-trip fixture)

        St::StoreInput StoreBase() {
            St::StoreInput in;
            in.bucket = St::Bucket::TwoHand; in.classBit = W::kTwoHand;
            in.classesOn = true; in.currentSet = W::kOneHand | W::kShield; in.currentKind = St::Kind::kRated;
            in.plan.weapons = W::kTwoHand; in.plan.kinds = St::Kind::kClothing;
            in.storageReady = true; in.singleCopy = true;
            in.rank = 20.0f; in.bestOtherRank = -1.0f; in.value = 500;
            in.buyMult = 1.6f; in.sellMult = 0.6f; in.minMargin = 100;
            return in;
        }

        Step DecideStoreStep(Ctx&) {
            struct Row { const char* name; St::StoreInput in; St::Verdict want; };
            std::vector<Row> rows;
            auto add = [&](const char* n, auto&& f, St::Verdict w) { St::StoreInput in = StoreBase(); f(in); rows.push_back({ n, in, w }); };
            add("planned, top, margin clears", [](St::StoreInput&) {}, St::Verdict::Store);
            add("no safe storage", [](St::StoreInput& i) { i.storageReady = false; }, St::Verdict::Sell);
            add("classes off", [](St::StoreInput& i) { i.classesOn = false; }, St::Verdict::Sell);
            add("in his class now", [](St::StoreInput& i) { i.currentSet |= W::kTwoHand; }, St::Verdict::Sell);
            add("unrestricted set (T1)", [](St::StoreInput& i) { i.currentSet = 0; }, St::Verdict::Sell);
            add("no planned class uses it", [](St::StoreInput& i) { i.plan.weapons = W::kBow; }, St::Verdict::Sell);
            add("outranked by a carried piece", [](St::StoreInput& i) { i.bestOtherRank = 25.0f; }, St::Verdict::Sell);
            add("equal rank, tie lost", [](St::StoreInput& i) { i.bestOtherRank = 20.0f; i.tieLoses = true; }, St::Verdict::Sell);
            add("equal rank, tie won", [](St::StoreInput& i) { i.bestOtherRank = 20.0f; i.tieLoses = false; }, St::Verdict::Store);
            add("margin under the minimum", [](St::StoreInput& i) { i.value = 50; }, St::Verdict::Sell);
            add("factors 1.0 / 1.0", [](St::StoreInput& i) { i.buyMult = 1.0f; i.sellMult = 1.0f; }, St::Verdict::Sell);
            add("two copies", [](St::StoreInput& i) { i.singleCopy = false; }, St::Verdict::Sell);
            add("bucket none", [](St::StoreInput& i) { i.bucket = St::Bucket::None; }, St::Verdict::Sell);
            add("armor of a planned kind", [](St::StoreInput& i) { i.bucket = St::Bucket::BodyCloth; i.classBit = 0; i.armorKind = St::Kind::kClothing; }, St::Verdict::Store);
            add("armor of his own kind", [](St::StoreInput& i) { i.bucket = St::Bucket::BodyRated; i.classBit = 0; i.armorKind = St::Kind::kRated; }, St::Verdict::Sell);
            add("armor of an unplanned kind", [](St::StoreInput& i) { i.bucket = St::Bucket::BodyCloth; i.classBit = 0; i.armorKind = St::Kind::kClothing; i.plan.kinds = 0; }, St::Verdict::Sell);
            std::string bad;
            for (const auto& r : rows) {
                const auto o = St::DecideStore(r.in);
                if (o.verdict != r.want)
                    bad += std::format("[{}: got {} ({}), use {} sale {}] ", r.name, o.verdict == St::Verdict::Store ? "Store" : "Sell", o.why,
                                       o.useValue, o.saleValue);
            }
            if (!bad.empty()) return Step::Fail("DecideStore rows wrong: " + bad);
            return Step::Pass(std::format("{} DecideStore rows as expected", rows.size()));
        }

        Step DecideRetrieveStep(Ctx&) {
            St::RetrieveInput b;
            b.bucket = St::Bucket::TwoHand; b.classBit = W::kTwoHand; b.currentSet = W::kTwoHand; b.saleKeeps = true;
            struct Row { const char* name; St::RetrieveInput in; bool want; };
            std::vector<Row> rows;
            auto add = [&](const char* n, auto&& f, bool w) { St::RetrieveInput in = b; f(in); rows.push_back({ n, in, w }); };
            add("in class, the sale path keeps it", [](St::RetrieveInput&) {}, true);
            add("off-class now", [](St::RetrieveInput& i) { i.currentSet = W::kBow; }, false);
            add("unrestricted set", [](St::RetrieveInput& i) { i.currentSet = 0; }, true);
            add("the sale path would not keep it", [](St::RetrieveInput& i) { i.saleKeeps = false; }, false);
            add("bucket none", [](St::RetrieveInput& i) { i.bucket = St::Bucket::None; }, false);
            add("armor of his kind", [](St::RetrieveInput& i) { i.bucket = St::Bucket::BodyRated; i.classBit = 0; i.armorKind = St::Kind::kRated; i.currentKind = St::Kind::kRated; }, true);
            add("armor of the other kind", [](St::RetrieveInput& i) { i.bucket = St::Bucket::BodyRated; i.classBit = 0; i.armorKind = St::Kind::kRated; i.currentKind = St::Kind::kClothing; }, false);
            std::string bad;
            for (const auto& r : rows) {
                const char* why = "";
                if (St::DecideRetrieve(r.in, &why) != r.want) bad += std::format("[{}: got {} ({})] ", r.name, !r.want, why);
            }
            const char* w1 = ""; const char* w2 = "";
            St::RetrieveInput c = b; St::DecideRetrieve(c, &w1); c.classChanged = true; St::DecideRetrieve(c, &w2);
            if (std::string_view(w2) != "class change" || std::string_view(w1) == "class change") bad += "[classChanged must change only the reason] ";
            if (!bad.empty()) return Step::Fail("DecideRetrieve rows wrong: " + bad);
            return Step::Pass(std::format("{} DecideRetrieve rows as expected; reasons '{}' / '{}'", rows.size(), w1, w2));
        }

        Step ClassSetsStep(Ctx&) {
            std::string bad;
            // Knight equipped, Melee + Archer mastered: the Archer set is planned, the unrestricted T1 adds no weapon bit.
            const auto k = St::PlannedFor(static_cast<std::uint8_t>(Id::Knight), Classes::Bit(Id::Melee) | Classes::Bit(Id::Archer), true);
            if (!(k.weapons & W::kBow)) bad += "[Knight + mastered Archer: bow not planned] ";
            if (k.weapons & W::kUnrestricted) bad += "[an unrestricted set leaked into the plan] ";
            if (k.classes & Classes::Bit(Id::Knight)) bad += "[the equipped class is in its own plan] ";
            if (!(k.classes & Classes::Bit(Id::GreatKnight))) bad += "[Great Knight is ahead of Knight on the melee line but not planned] ";
            if (k.classes & Classes::Bit(Id::Wizard)) bad += "[Wizard (mage root) planned for a Knight] ";
            if (k.classes & Classes::Bit(Id::Dragonborn)) bad += "[Dragonborn (no T1 root) planned] ";
            // Warrior (no shield) sees the shield through Great Knight, a future class of his line.
            const auto w = St::PlannedFor(static_cast<std::uint8_t>(Id::Warrior), Classes::Bit(Id::Melee), true);
            if (!(w.weapons & W::kShield)) bad += "[Warrior: the shield of a future melee class is not planned] ";
            if (w.kinds & St::Kind::kClothing) bad += "[Warrior with no mage class plans clothing] ";
            // Evoker with a mastered Knight plans rated armor; with nothing but the mage line, only clothing.
            const auto e1 = St::PlannedFor(static_cast<std::uint8_t>(Id::Evoker), Classes::Bit(Id::Mage) | Classes::Bit(Id::Knight), true);
            const auto e2 = St::PlannedFor(static_cast<std::uint8_t>(Id::Evoker), Classes::Bit(Id::Mage), true);
            if (!(e1.kinds & St::Kind::kRated)) bad += "[Evoker + mastered Knight: rated armor not planned] ";
            if (e2.kinds & St::Kind::kRated) bad += "[Evoker on the mage line alone plans rated armor] ";
            if (St::KindOf(Id::Evoker, false) != St::Kind::kRated) bad += "[robes off: the mage line must wear rated armor] ";
            if (St::PlannedFor(Classes::kNoClass, ~Classes::Mask{ 0 }, true).classes != 0) bad += "[no class equipped must plan nothing] ";
            if (!bad.empty()) return Step::Fail("PlannedFor wrong: " + bad);
            return Step::Pass(std::format("Knight plan classes {:X} weapons {:X}; Warrior weapons {:X}; Evoker kinds {:X} / {:X}", k.classes, k.weapons,
                                          w.weapons, e1.kinds, e2.kinds));
        }

        const char* RoundtripPre(Ctx& c) {
            const RE::FormID sword = VanillaForm(kIronSwordLocal);
            const RE::FormID armor = VanillaForm(kIronArmorLocal);
            if (!sword || !armor) return "Skyrim.esm IronSword 00012EB7 + ArmorIronCuirass 00012E49 resolvable";
            c.s->set[0] = { sword, armor };
            return nullptr;
        }

        Step RoundtripStep(Ctx& c) {
            const RE::FormID sword = c.s->set[0][0], armor = c.s->set[0][1];
            constexpr RE::FormID kFol = 0x00012345, kRuntimeFol = 0xFF000801;
            St::Table t;
            t.stored[kFol] = { St::Item{ sword, 1, 0 }, St::Item{ armor, 1, St::Item::kUnverified } };
            t.stored[kRuntimeFol] = { St::Item{ sword, 1, 0 } };   // runtime follower: never written
            t.stored[0x00012347] = { St::Item{ 0xFF000123, 1, 0 } };  // runtime base: never written (and the follower with it)
            CoSaveBuffer buf;
            St::LoadStats ws;
            St::WriteBody(buf, t, &ws);
            St::Table back; St::LoadStats rs;
            buf.Rewind();
            const bool ok = St::ReadBody(buf, 1, back, rs);
            std::string bad;
            if (!ok) bad += "[the round trip aborted] ";
            if (ws.skippedRuntime != 2) bad += std::format("[{} runtime ids skipped, want 2] ", ws.skippedRuntime);
            if (back.stored.size() != 1 || back.stored[kFol].size() != 2) bad += "[stored differ] ";
            else {
                const auto& v = back.stored[kFol];
                if (v[0].base != sword || v[0].flags != 0 || v[1].base != armor || v[1].flags != St::Item::kUnverified) bad += "[items differ] ";
            }
            // Hand-built body: flags 0xFF reads as 0x03; count 7 clamps to 1; a duplicate base is dropped; then an item count of 17
            // aborts and keeps what was parsed.
            CoSaveBuffer raw;
            raw.WriteRecordData(std::uint32_t{ 2 });
            raw.WriteRecordData(kFol); raw.WriteRecordData(std::uint32_t{ 2 });
            raw.WriteRecordData(sword); raw.WriteRecordData(std::uint16_t{ 7 }); raw.WriteRecordData(std::uint8_t{ 0xFF });
            raw.WriteRecordData(sword); raw.WriteRecordData(std::uint16_t{ 1 }); raw.WriteRecordData(std::uint8_t{ 0 });
            raw.WriteRecordData(RE::FormID{ 0x00012346 }); raw.WriteRecordData(std::uint32_t{ 17 });
            raw.Rewind();
            St::Table r2; St::LoadStats s2;
            const bool ok2 = St::ReadBody(raw, 1, r2, s2);
            if (ok2) bad += "[an item count of 17 did not abort] ";
            if (r2.stored[kFol].size() != 1 || r2.stored[kFol][0].flags != St::Item::kAllFlags || r2.stored[kFol][0].count != 1)
                bad += "[the masked / clamped item was not kept as parsed] ";
            if (s2.droppedItems != 1) bad += std::format("[{} dropped item(s), want 1 (the duplicate base)] ", s2.droppedItems);
            if (!bad.empty()) return Step::Fail("STOR v1 codec: " + bad);
            return Step::Pass(std::format("STOR v1 round trip identical ({} bytes), {} runtime ids skipped, flags 0xFF -> 0x{:02X}, count 7 -> 1, "
                                          "duplicate base dropped, itemCount 17 aborted with {} piece(s) kept", buf.Bytes().size(), ws.skippedRuntime,
                                          r2.stored[kFol].empty() ? 0 : r2.stored[kFol][0].flags, r2.stored[kFol].size()));
        }

        Step ChestStep(Ctx& c) {
            auto* ref = Forms::g_storageChest;
            if (!ref) return Step::Fail("MFO.esp 0x919 MFO_StorageChestRef did not resolve (old MFO.esp?)");
            auto* base = ref->GetBaseObject();
            auto* cont = base ? base->As<RE::TESObjectCONT>() : nullptr;
            if (!cont) return Step::Fail("the storage chest ref's base is not a container");
            if (!ref->IsPersistent()) return Step::Fail("the storage chest ref is NOT persistent (it would leave memory with its cell)");
            if (ref->IsDisabled() || ref->IsDeleted()) return Step::Fail("the storage chest ref is disabled or deleted");
            if (cont->data.flags.all(RE::CONT_DATA::Flag::kRespawn)) return Step::Fail("the storage chest's base RESPAWNS (it would empty itself)");
            auto* cell = ref->GetParentCell();
            return Step::Pass(std::format("MFO chest {:08X} (base {:08X} '{}'), persistent, no respawn, cell {:08X}; stored for {:08X}: {} piece(s)",
                                          ref->GetFormID(), cont->GetFormID(), cont->GetName() ? cont->GetName() : "?",
                                          cell ? cell->GetFormID() : 0u, c.id, St::StoredOf(c.id).size()));
        }

        const char* ChestRoundTripPre(Ctx& c) {
            if (!Forms::g_storageChest) return "MFO.esp 0x919 MFO_StorageChestRef resolvable";
            const RE::FormID dagger = VanillaForm(kIronDaggerLocal);
            if (!dagger) return "Skyrim.esm IronDagger 0001397E resolvable";
            if (c.inCombat) return "out of combat (Storage::Tick runs in the idle branch)";
            c.s->set[1] = { dagger };
            return nullptr;
        }
        void ChestRoundTripFix(Ctx& c, FixtureRequest& r) {
            r.items.push_back({ c.s->set[1][0], 1 });
            r.probes |= kProbeStorageMove;
        }
        Step ChestRoundTripStep(Ctx& c) {
            const auto rt = St::DevReadRoundTrip(c.id);
            if (!rt.armed) {
                if (c.elapsed > 10.0f) return Step::Fail("the round trip was never armed (probe kProbeStorageMove not applied)");
                return Step::Continue();
            }
            if (!rt.done) return Step::Continue();
            return rt.ok ? Step::Pass(rt.evidence) : Step::Fail(rt.evidence);
        }
    }

    void RegisterStorage() {
        Register({ "storage.decide_store", "86e3t212u", "(pure) the store judge over a table of inputs", 10.0f, nullptr, nullptr, DecideStoreStep, nullptr });
        Register({ "storage.decide_retrieve", "86e3t212u", "(pure) the retrieval judge over a table of inputs", 10.0f, nullptr, nullptr, DecideRetrieveStep, nullptr });
        Register({ "storage.class_sets", "86e3t212u", "(pure) PlannedFor over the real class table", 10.0f, nullptr, nullptr, ClassSetsStep, nullptr });
        Register({ "storage.record_roundtrip", "86e3t212u", "(pure) the STOR v1 codec on a byte buffer (Skyrim.esm forms resolvable)", 10.0f,
                   RoundtripPre, nullptr, RoundtripStep, nullptr });
        Register({ "storage.chest", "86e3t212u", "(pure) the MFO storage chest (MFO.esp 0x919) resolves, persistent, no respawn", 10.0f, nullptr, nullptr,
                   ChestStep, nullptr });
        Register({ "storage.chest_roundtrip", "86e3t212u review F1",
                   "(self-setup) one Iron Dagger is added to his pack; Storage::Tick moves it into the MFO chest and back by Papyrus RemoveItem, "
                   "each hop confirmed from his count (out of combat, idle)",
                   120.0f, ChestRoundTripPre, ChestRoundTripFix, ChestRoundTripStep, nullptr });
    }

}
